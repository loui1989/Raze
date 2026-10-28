// Raze RT: smoke, as RTGL1's volumetric puffs (the Doom 64 RT smoke volume).
//
// What makes smoke, and how it looks, comes from Duke-RT's LIGHTOVR:
//
//   smokestyle      how a puff looks and moves (radius, growth, rise, life, colour...)
//   smokeactorrule  an actor class that makes smoke when it appears (an explosion)
//                   or all the while it exists (a fire, a flying rocket)
//   smokeeventrule  smoke for a game event: the player's shots
//   smokeemitter    a map's own smoke source (the mood smoke out of a window)
//
// Duke-RT renders smoke in a fluid grid of its own; RTGL1 has a froxel volume fed
// with up to RG_MAX_SMOKE_PUFFS soft spheres a frame. So a style is read as a puff:
// its radius, expansion, rise, buoyancy, drag and turbulence drive a simple
// particle, its density envelope (attack, sustain, release) fades it, and the
// packing below is Doom 64 RT's (rt_main.cpp), which is what the shader expects.
//
// Units: LIGHTOVR is in map units (1/32 m here), positions in Build world space.

#include "rt_backend.h"

#include "lightoverlay.h"
#include "coreactor.h"
#include "gamefuncs.h"
#include "texturemanager.h"
#include "buildtiles.h"
#include "i_time.h"

#include <array>
#include <random>
#include <unordered_map>
#include <unordered_set>

CVARD( Bool, rt_smoke, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: volumetric smoke (explosions, fires, rockets, shots, map smoke)" )
CVARD( Float, rt_smoke_density, 2.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: how thick the smoke is" )
CVARD( Float, rt_smoke_amount, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: how many puffs the smoke sources make (0..2)" )
CVARD( Float, rt_smoke_far, 40.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: metres around the player in which smoke is made" )
// Off by default: lit from every light, smoke near Duke's many small, strong lights
// (screens, grates, lamps) turned into fields of white dots, one per froxel that
// happened to sample the close light
CVARD( Bool, rt_smoke_illum, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: smoke is lit by every light, not only the moon (prone to white dots)" )
CVARD( Bool, rt_smoke_debug, false, 0, "RT: print the smoke pool's state" )
CVARD( Float, rt_smoke_maxlight, 2.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: ceiling on the light inside smoke (against white specks)" )
CVARD( Float, rt_smoke_light_near, 1.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: metres within which a light fades out of the smoke it sits in" )
CVARD( Float, rt_fog, 0.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: ground fog over open-air ground (0 = none, 1 = some, 2 = thick)" )
CVARD( Bool, rt_smoke_wake, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: walking, flying things and blasts clear and push smoke and fog aside (it flows back in)" )
CVARD( Float, rt_smoke_wake_strength, 1.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: how strongly they clear and push it (0..3)" )
CVARD( Float, rt_smoke_bullet_radius, 0.35f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: metres round a bullet's track that it clears through smoke and fog (bigger than a real bullet's, to be seen)" )
CVARD( Float, rt_fog_glow, 0.3f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: how much light of its own the ground fog has (its grey: higher = paler, lower = darker)" )
CVARD( Float, rt_fog_height, 1.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: how high the ground fog lies (1 = about knee to waist high)" )
CVARD( Float, rt_smoke_history, 10.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: frames the volume accumulates while there is smoke (more = smoother, slower to let go)" )

namespace
{

using namespace rtx;

constexpr float kUnit = 1.0f / 32.0f; // map units to metres

struct Puff
{
    FVector3 pos;      // metres, RTGL1 space
    FVector3 vel;      // m/s
    FVector3 albedo;   // 0..1
    float    radius  = 0.f;
    float    radius0 = 0.f;
    float    density = 0.f; // optical depth per metre at the core
    float    growth  = 0.f; // m/s
    float    buoy    = 0.f; // m/s^2 upward
    float    drag    = 0.f; // 1/s
    float    turb    = 0.f; // m/s^2 lateral
    float    phase   = 0.f;
    float    age     = 0.f;
    float    life    = 1.f;
    float    attack = 0.f, sustain = 0.f, release = 0.f; // envelope, seconds (0 = (1-t)^2 fade)
    bool     ambient = false; // continuous sources give way to events
    bool     fog     = false; // a bank of ground fog
    FVector3 push;            // a shove (a blast, someone walking through): fades by itself
};

std::array< Puff, RG_MAX_SMOKE_PUFFS > g_puffs;
uint32_t                               g_count = 0;
void AddBlast( const FVector3& c, float radius ); // the wake, below
std::mt19937                           g_rng{ 0x5EED };

float Rnd( float a, float b )
{
    return std::uniform_real_distribution< float >( a, b )( g_rng );
}

FVector3 RndUnit()
{
    for( ;; )
    {
        FVector3 v{ Rnd( -1, 1 ), Rnd( -1, 1 ), Rnd( -1, 1 ) };
        const float l2 = v.LengthSquared();
        if( l2 > 1e-4f && l2 <= 1.f )
        {
            return v / std::sqrt( l2 );
        }
    }
}

FVector3 WorldToRTv( double x, double y, double z )
{
    return { float( x ) * kUnit, float( -y ) * kUnit, float( -z ) * kUnit };
}

Puff* Alloc( bool ambient )
{
    if( g_count < g_puffs.size() )
    {
        return &g_puffs[ g_count++ ];
    }
    // full: an event takes the oldest ambient puff, else the oldest of all; an
    // ambient source never pushes out an event's
    int best = -1;
    float bestAge = -1.f;
    for( uint32_t i = 0; i < g_count; i++ )
    {
        const Puff& p = g_puffs[ i ];
        if( ambient && !p.ambient )
        {
            continue;
        }
        const float key = p.age / std::max( p.life, 0.01f ) + ( p.ambient ? 10.f : 0.f );
        if( key > bestAge )
        {
            bestAge = key;
            best    = int( i );
        }
    }
    return best >= 0 ? &g_puffs[ best ] : nullptr;
}

// one puff of a style, at 'pos' (metres), pushed along 'dir' (unit) by 'speed' m/s
void SpawnFromStyle( const ParsedLightOverlaySmokeStyle& st, const FVector3& pos, const FVector3& dir, float speed,
                     float densityScale, float radiusScale, float spawnRadiusM, bool ambient )
{
    Puff* p = Alloc( ambient );
    if( !p )
    {
        return;
    }
    const float lobe = Rnd( std::min( st.lobeRadiusRandom[ 0 ], st.lobeRadiusRandom[ 1 ] ),
                            std::max( st.lobeRadiusRandom[ 0 ], st.lobeRadiusRandom[ 1 ] ) );
    *p = Puff{};
    p->pos     = pos + ( spawnRadiusM > 0.f ? RndUnit() * ( spawnRadiusM * Rnd( 0.f, 1.f ) ) : FVector3{ 0, 0, 0 } );
    p->radius0 = std::max( 0.04f, st.radius * radiusScale * lobe * kUnit );
    p->radius  = p->radius0;
    p->density = std::max( 0.f, st.density * densityScale ) * float( *rt_smoke_density );
    p->growth  = std::max( 0.f, st.expansionVelocity ) * kUnit;
    p->buoy    = st.buoyancy * 0.1f;
    p->drag    = std::max( 0.f, st.drag );
    p->turb    = std::max( 0.f, st.turbulence ) * kUnit;
    p->phase   = Rnd( 0.f, 6.2831853f );
    p->life    = std::clamp( st.lifetime, 0.3f, 6.f );
    p->attack  = st.densityAttackSeconds;
    p->sustain = st.densitySustainSeconds;
    p->release = st.densityReleaseSeconds;
    p->albedo  = { std::clamp( st.albedo[ 0 ], 0.f, 1.f ), std::clamp( st.albedo[ 1 ], 0.f, 1.f ), std::clamp( st.albedo[ 2 ], 0.f, 1.f ) };
    p->ambient = ambient;
    p->vel     = FVector3{ 0, 0, st.riseVelocity * kUnit } + RndUnit() * ( st.velocityRandom * kUnit * Rnd( 0.f, 1.f ) ) +
             dir * speed;
}

// how many puffs a rule's count becomes: RTGL1 has 128 for everything
uint32_t Scaled( uint32_t count, float divide )
{
    const float n = float( count ) / divide * std::clamp( float( *rt_smoke_amount ), 0.f, 2.f );
    return n <= 0.f ? 0u : std::max( 1u, uint32_t( n + 0.5f ) );
}

FVector3 ConeDir( const FVector3& axis, float coneDeg )
{
    if( coneDeg <= 0.f )
    {
        return axis;
    }
    if( coneDeg >= 179.f )
    {
        return RndUnit();
    }
    for( int tries = 0; tries < 16; tries++ )
    {
        const FVector3 d = RndUnit();
        if( ( d | axis ) >= std::cos( coneDeg * 3.14159265f / 180.f ) )
        {
            return d;
        }
    }
    return axis;
}

const ParsedLightOverlaySmokeStyle* StyleOf( const ResolvedLightOverlaySet& set, bool resolved, uint32_t index )
{
    return resolved && index < set.smokeStyles.Size() ? &set.smokeStyles[ index ] : nullptr;
}

// ---------------------------------------------------------------------------------
// sources

struct ActorState
{
    double   nextTime = 0;
    uint32_t seenGen  = 0;
};
std::unordered_map< uint64_t, ActorState > g_actors; // (actor index << 8 | rule)
std::unordered_set< const DCoreActor* >    g_seen;   // actors seen last frame (spawn trigger)
uint32_t                                   g_seenGen = 1;
double                                     g_emitterNext[ 64 ]{};
uint32_t                                   g_lastShot   = 0;
uint32_t                                   g_levelGen   = ~0u;

void ActorSources( const ResolvedLightOverlaySet& set, double now, const FVector3& eye, float farM )
{
    if( set.smokeActorRules.Size() == 0 )
    {
        return;
    }
    std::unordered_set< const DCoreActor* > seenNow;
    g_seenGen++;

    TSpriteIterator< DCoreActor > it;
    while( auto actor = it.Next() )
    {
        PClass* cls = actor->GetClass();
        if( !cls )
        {
            continue;
        }
        for( unsigned r = 0; r < set.smokeActorRules.Size(); r++ )
        {
            const auto& rule = set.smokeActorRules[ r ];
            if( !rule.actorClassResolved || !rule.actorClass || !( cls == rule.actorClass || cls->IsDescendantOf( rule.actorClass ) ) )
            {
                continue;
            }
            const auto* st = StyleOf( set, rule.styleResolved, rule.styleIndex );
            if( !st )
            {
                continue;
            }
            seenNow.insert( actor );

            const FVector3 pos = WorldToRTv( actor->spr.pos.X + rule.offset[ 0 ], actor->spr.pos.Y + rule.offset[ 1 ],
                                             actor->spr.pos.Z + rule.offset[ 2 ] );
            if( ( pos - eye ).LengthSquared() > farM * farM )
            {
                continue;
            }
            const uint64_t key   = ( uint64_t( actor->GetIndex() ) << 8 ) | r;
            ActorState&    state = g_actors[ key ];
            state.seenGen        = g_seenGen;

            const float speed = rule.velocityScale; // m/s along the cone
            if( rule.trigger == LightOverlaySmokeTrigger::Spawn )
            {
                if( g_seen.count( actor ) )
                {
                    continue; // already burst
                }
                // an explosion's smoke: the blast clears and shoves what is round it (the wake)
                if( strstr( cls->TypeName.GetChars(), "xplosion" ) )
                {
                    AddBlast( pos, 2.2f );
                }
                // a burst: Duke-RT's 72-lobe cloud becomes a dozen puffs
                const uint32_t n = Scaled( rule.count, 6.f );
                for( uint32_t i = 0; i < n; i++ )
                {
                    SpawnFromStyle( *st, pos, ConeDir( { 0, 0, 1 }, rule.velocityCone ), speed * Rnd( 0.3f, 1.f ),
                                    rule.densityScale, rule.radiusScale * 0.6f, rule.spawnRadius * kUnit, false );
                }
            }
            else
            {
                if( now < state.nextTime )
                {
                    continue;
                }
                state.nextTime = now + std::max( 0.03f, rule.intervalSeconds ) * Rnd( 0.8f, 1.2f );
                // continuous sources (fires) are ambient: they never starve the events
                const bool     ambient = rule.trigger == LightOverlaySmokeTrigger::Interval && rule.velocityScale <= 0.f;
                const uint32_t n       = Scaled( rule.count, 6.f );
                for( uint32_t i = 0; i < n; i++ )
                {
                    SpawnFromStyle( *st, pos, ConeDir( { 0, 0, 1 }, rule.velocityCone ), speed, rule.densityScale,
                                    rule.radiusScale * 0.5f, rule.spawnRadius * kUnit, ambient );
                }
            }
        }
    }
    g_seen.swap( seenNow );

    // forget actors that are gone
    for( auto i = g_actors.begin(); i != g_actors.end(); )
    {
        i = i->second.seenGen != g_seenGen ? g_actors.erase( i ) : std::next( i );
    }
}

void EmitterSources( const ResolvedLightOverlaySet& set, double now, const FVector3& eye, float farM )
{
    for( unsigned e = 0; e < set.mapSmokeEmitterRules.Size() && e < 64; e++ )
    {
        const auto& rule = set.mapSmokeEmitterRules[ e ];
        const auto* st   = StyleOf( set, rule.styleResolved, rule.styleIndex );
        LightOverlayMapSmokeEmitterRectangle rect;
        if( !st || !BuildLightOverlayMapSmokeEmitterRectangle( rule, rect ) )
        {
            continue;
        }
        if( now < g_emitterNext[ e ] )
        {
            continue;
        }
        g_emitterNext[ e ] = now + std::max( 0.05f, rule.intervalSeconds ) * Rnd( 0.8f, 1.2f );

        const uint32_t n = Scaled( rule.count, 1.5f );
        for( uint32_t i = 0; i < n; i++ )
        {
            const float u = Rnd( -1, 1 ), v = Rnd( -1, 1 );
            const float wx = rect.center[ 0 ] + rect.halfAxisU[ 0 ] * u + rect.halfAxisV[ 0 ] * v;
            const float wy = rect.center[ 1 ] + rect.halfAxisU[ 1 ] * u + rect.halfAxisV[ 1 ] * v;
            const float wz = rect.center[ 2 ] + rect.halfAxisU[ 2 ] * u + rect.halfAxisV[ 2 ] * v;
            const FVector3 pos = WorldToRTv( wx, wy, wz );
            if( ( pos - eye ).LengthSquared() > farM * farM )
            {
                break;
            }
            FVector3 nrm = WorldToRTv( rect.normal[ 0 ], rect.normal[ 1 ], rect.normal[ 2 ] );
            if( nrm.LengthSquared() > 1e-8f )
            {
                nrm.MakeUnit();
            }
            SpawnFromStyle( *st, pos, ConeDir( nrm, rule.velocityCone ), rule.velocityScale, rule.densityScale,
                            rule.radiusScale * 0.6f, rule.spawnRadius * kUnit, true );
        }
    }
}

void ShotSource( const ResolvedLightOverlaySet& set, const FVector3& eye, const FVector3& fwd, const FVector3& right,
                 const FVector3& up )
{
    if( g_rtShotCounter == g_lastShot )
    {
        return;
    }
    g_lastShot = g_rtShotCounter;
    if( !g_rtShotEvent )
    {
        return;
    }
    for( unsigned i = 0; i < set.smokeEventRules.Size(); i++ )
    {
        const auto& rule = set.smokeEventRules[ i ];
        if( rule.id.CompareNoCase( g_rtShotEvent ) != 0 )
        {
            continue;
        }
        const auto* st = StyleOf( set, rule.styleResolved, rule.styleIndex );
        if( !st )
        {
            return;
        }
        // offset: right, forward, down from the eye, in map units
        const FVector3 muzzle = eye + right * ( rule.offset[ 0 ] * kUnit ) + fwd * ( rule.offset[ 1 ] * kUnit ) -
                                up * ( rule.offset[ 2 ] * kUnit );
        const uint32_t n = Scaled( rule.count, 2.5f );
        for( uint32_t k = 0; k < n; k++ )
        {
            const FVector3 jitter = right * ( Rnd( -1, 1 ) * rule.offsetRandom[ 0 ] * kUnit ) +
                                    up * ( Rnd( -1, 1 ) * rule.offsetRandom[ 2 ] * kUnit );
            // a filament off the barrel: small parcels, carried forward along the aim
            SpawnFromStyle( *st, muzzle + jitter, ConeDir( fwd, rule.velocityCone ), rule.velocityScale * Rnd( 0.4f, 1.f ),
                            rule.densityScale, std::max( rule.radiusScale, 0.6f ), rule.spawnRadius * kUnit, false );
        }
        return;
    }
}

// ---------------------------------------------------------------------------------
// the game's own smoke (g_rtSmokeSources): Blood. Drawn with Duke-RT's styles.

ParsedLightOverlaySmokeStyle MakeStyle( float density, FVector3 albedo, float radius, float expansion, float life,
                                        float rise, float velRandom, float buoyancy, float drag, float turb,
                                        float attack, float sustain, float release, float lobe0, float lobe1 )
{
    ParsedLightOverlaySmokeStyle st;
    st.density                 = density;
    st.albedo[ 0 ]             = albedo.X;
    st.albedo[ 1 ]             = albedo.Y;
    st.albedo[ 2 ]             = albedo.Z;
    st.radius                  = radius;
    st.expansionVelocity       = expansion;
    st.lifetime                = life;
    st.riseVelocity            = rise;
    st.velocityRandom          = velRandom;
    st.buoyancy                = buoyancy;
    st.drag                    = drag;
    st.turbulence              = turb;
    st.densityAttackSeconds    = attack;
    st.densitySustainSeconds   = sustain;
    st.densityReleaseSeconds   = release;
    st.lobeRadiusRandom[ 0 ]   = lobe0;
    st.lobeRadiusRandom[ 1 ]   = lobe1;
    return st;
}

// Duke-RT's duke_explosion_smoke, duke_fire_smoke, duke_trail_smoke, duke_muzzle_smoke
const ParsedLightOverlaySmokeStyle& GameStyle( RtSmokeKind kind )
{
    static const ParsedLightOverlaySmokeStyle styles[] = {
        MakeStyle( 3.45f, { 0.34f, 0.32f, 0.30f }, 18.f, 12.f, 5.f, 9.f, 12.f, 2.f, 0.45f, 2.5f, 0.04f, 1.55f, 3.45f, 0.42f, 0.68f ),
        MakeStyle( 3.0f, { 0.30f, 0.29f, 0.28f }, 7.f, 10.f, 18.f, 120.f, 20.f, 7.f, 0.05f, 42.f, 0.2f, 1.5f, 4.f, 0.30f, 0.45f ),
        MakeStyle( 1.2f, { 0.42f, 0.44f, 0.46f }, 10.f, 2.f, 4.f, 7.f, 2.f, 5.5f, 0.7f, 10.2f, 0.f, 0.18f, 0.57f, 0.75f, 1.25f ),
        MakeStyle( 6.0f, { 0.44f, 0.43f, 0.42f }, 5.f, 3.f, 1.35f, 0.f, 0.f, 0.f, 1.2f, 1.f, 0.f, 0.30f, 0.50f, 0.75f, 1.25f ),
    };
    return styles[ std::min( size_t( kind ), std::size( styles ) - 1 ) ];
}

std::unordered_set< uint64_t >          g_gameBurst;   // bursts made (their sources still listed)
std::unordered_map< uint64_t, double >  g_gameNext;    // continuous sources: when they smoke next

void GameSources( double now, const FVector3& eye, const FVector3& fwd, const FVector3& right, const FVector3& up,
                  float farM )
{
    std::unordered_set< uint64_t >         burstNow;
    std::unordered_map< uint64_t, double > nextNow;
    for( const RtSmokeSource& src : g_rtSmokeSources )
    {
        const uint64_t key   = src.id ^ ( uint64_t( src.kind ) << 60 );
        const float    scale = std::clamp( src.scale, 0.1f, 4.f );
        const auto&    st    = GameStyle( src.kind );
        FVector3       pos   = WorldToRTv( src.pos[ 0 ], src.pos[ 1 ], src.pos[ 2 ] );
        switch( src.kind )
        {
            case RtSmokeKind::Explosion:
            case RtSmokeKind::Shot:
            {
                burstNow.insert( key );
                if( g_gameBurst.count( key ) )
                {
                    break;
                }
                if( src.kind == RtSmokeKind::Explosion )
                {
                    // (the blast clears and shoves the smoke and fog round it: the wake)
                    AddBlast( pos + FVector3{ 0.f, 0.f, 0.8f }, 2.2f * scale );
                    pos.Z += 48.f * kUnit * scale; // the cloud's middle above the blast
                    if( ( pos - eye ).LengthSquared() > farM * farM )
                    {
                        break;
                    }
                    const uint32_t n = Scaled( 72, 6.f );
                    for( uint32_t i = 0; i < n; i++ )
                    {
                        SpawnFromStyle( st, pos, RndUnit(), Rnd( 0.3f, 1.f ), 1.5f * scale, 2.5f * 0.6f * scale,
                                        18.f * kUnit * scale, false );
                    }
                }
                else
                {
                    // off the barrel: right, forward and down of the eye
                    const FVector3 muzzle = eye + right * ( 6.f * kUnit ) + fwd * ( 20.f * kUnit ) - up * ( 8.f * kUnit );
                    const uint32_t n      = Scaled( 5, 2.5f );
                    for( uint32_t i = 0; i < n; i++ )
                    {
                        SpawnFromStyle( st, muzzle + up * ( Rnd( -1, 1 ) * 2.f * kUnit ), ConeDir( fwd, 12.f ),
                                        1.5f * Rnd( 0.4f, 1.f ), 1.f * scale, 0.6f * scale, 2.f * kUnit, false );
                    }
                }
                break;
            }
            case RtSmokeKind::Fire:
            case RtSmokeKind::Trail:
            {
                const bool fire = src.kind == RtSmokeKind::Fire;
                if( fire )
                {
                    pos.Z += 32.f * kUnit;
                }
                auto        it   = g_gameNext.find( key );
                double      next = it != g_gameNext.end() ? it->second : 0.0;
                if( ( pos - eye ).LengthSquared() <= farM * farM && now >= next )
                {
                    next = now + ( fire ? 0.5 : 0.05 ) * Rnd( 0.8f, 1.2f );
                    const uint32_t n = Scaled( fire ? 9 : 14, 6.f );
                    for( uint32_t i = 0; i < n; i++ )
                    {
                        if( fire )
                        {
                            SpawnFromStyle( st, pos, { 0, 0, 1 }, 0.f, 3.f * scale, 1.5f * scale, 4.f * kUnit, true );
                        }
                        else
                        {
                            SpawnFromStyle( st, pos, ConeDir( { 0, 0, 1 }, 22.f ), 0.5f, 1.f * scale, 0.5f * scale,
                                            2.f * kUnit, false );
                        }
                    }
                }
                nextNow[ key ] = next;
                break;
            }
        }
    }
    g_gameBurst.swap( burstNow );
    g_gameNext.swap( nextNow );
}

// ---------------------------------------------------------------------------------
// ground fog (rt_fog): soft banks lying on open-air ground around the viewer, drifting
// with a slow wind, made and let go as the viewer moves. Lit by the moon.

struct FogSector
{
    int      sect;
    DVector4 box; // min x, min y, max x, max y (game units)
};
std::vector< FogSector > g_fogSectors;
uint32_t                 g_fogGen = ~0u;
FVector3                 g_wind{ 0.2f, 0.1f, 0.f };

void FindFogSectors()
{
    g_fogSectors.clear();
    for( unsigned i = 0; i < sector.Size(); i++ )
    {
        auto& sec = sector[ i ];
        // open air: the sky above, or (Blood) a room over a room whose upper part is
        if( !( ( sec.ceilingstat & CSTAT_SECTOR_SKY ) || sec.portalflags == PORTAL_SECTOR_CEILING ) )
        {
            continue;
        }
        // not the upper part of a room over a room (its floor is an opening), not sky
        // underneath, not a shut door
        if( sec.portalflags == PORTAL_SECTOR_FLOOR || ( sec.floorstat & CSTAT_SECTOR_SKY ) ||
            sec.floorz - sec.ceilingz < 32.0 || sec.walls.Size() < 3 )
        {
            continue;
        }
        DVector4 b( 1e30, 1e30, -1e30, -1e30 );
        for( auto& w : sec.walls )
        {
            b.X = std::min( b.X, w.pos.X ), b.Y = std::min( b.Y, w.pos.Y );
            b.Z = std::max( b.Z, w.pos.X ), b.W = std::max( b.W, w.pos.Y );
        }
        g_fogSectors.push_back( { int( i ), b } );
    }
    const float a = Rnd( 0.f, 6.2831853f );
    g_wind        = { std::cos( a ) * 0.1f, std::sin( a ) * 0.1f, 0.f };
}

const FogSector* FogSectorAt( double x, double y )
{
    for( const FogSector& f : g_fogSectors )
    {
        if( x >= f.box.X && x <= f.box.Z && y >= f.box.Y && y <= f.box.W && inside( x, y, &sector[ f.sect ] ) == 1 )
        {
            return &f;
        }
    }
    return nullptr;
}

void FogSources( float dt )
{
    if( g_fogGen != RT_LevelGeneration() )
    {
        g_fogGen = RT_LevelGeneration();
        FindFogSectors();
    }
    const float  amount = std::clamp( float( *rt_fog ), 0.f, 3.f );
    const float  height = std::clamp( float( *rt_fog_height ), 0.2f, 4.f );
    const float  reachM = 30.f;
    const DVector2 view( g_rtViewPos[ 0 ], g_rtViewPos[ 1 ] );

    // banks out of reach (or no fog any more) fade out
    uint32_t live = 0;
    for( uint32_t i = 0; i < g_count; i++ )
    {
        Puff& p = g_puffs[ i ];
        if( !p.fog )
        {
            continue;
        }
        const DVector2 at( p.pos.X / kUnit, -p.pos.Y / kUnit );
        const bool     away = amount <= 0.f || ( at - view ).Length() * kUnit > reachM + 6.f;
        if( away )
        {
            const float end = p.attack + p.sustain;
            if( p.age < end )
            {
                // straight to the release, from the density it has now
                p.sustain = std::max( 0.f, p.age - p.attack );
            }
            continue;
        }
        live++;
    }
    if( amount <= 0.f || g_fogSectors.empty() || g_rtViewSector < 0 )
    {
        return;
    }

    const uint32_t target = uint32_t( std::min( 72.f, 26.f * amount ) );
    // a few a frame: banks fade in over seconds anyway
    for( int made = 0; made < 3 && live < target && g_count < g_puffs.size(); made++ )
    {
        for( int tries = 0; tries < 12; tries++ )
        {
            // a point on the ground around the viewer, more of them further out (even cover)
            const float    r = reachM * std::sqrt( Rnd( 0.f, 1.f ) ) / kUnit;
            const float    a = Rnd( 0.f, 6.2831853f );
            const DVector2 q = view + DVector2( std::cos( a ) * r, std::sin( a ) * r );
            const FogSector* fs = FogSectorAt( q.X, q.Y );
            if( !fs )
            {
                continue;
            }
            // open air all around it too: a bank by a wall would show through into the
            // room behind it (the volume does not stop at walls)
            const float radius = Rnd( 1.6f, 2.8f ) * height;
            const double rr    = radius * 0.8f / kUnit;
            bool         open  = true;
            for( int k = 0; k < 6 && open; k++ )
            {
                const double b = k * 1.0471976;
                open           = FogSectorAt( q.X + std::cos( b ) * rr, q.Y + std::sin( b ) * rr ) != nullptr;
            }
            if( !open )
            {
                continue;
            }
            const double floorz = getflorzofslopeptr( &sector[ fs->sect ], q.X, q.Y );
            Puff& p   = g_puffs[ g_count++ ];
            p         = Puff{};
            p.fog     = true;
            p.ambient = true;
            p.radius0 = p.radius = radius;
            p.pos     = WorldToRTv( q.X, q.Y, floorz ) + FVector3{ 0.f, 0.f, Rnd( -0.35f, 0.15f ) * p.radius };
            p.density = 0.16f;
            p.growth  = 0.015f;
            p.turb    = 0.04f;
            p.phase   = Rnd( 0.f, 6.2831853f );
            p.life    = Rnd( 28.f, 48.f );
            p.attack  = 6.f;
            p.release = 7.f;
            p.sustain = std::max( 0.f, p.life - p.attack - p.release );
            // mist, not smoke (Smoke.h, Raze RT's shaders): it does not absorb, and it glows
            // pale grey with a light of its own, rt_fog_glow -- as smoke it was a dark bank
            // lit only by the moon, soot rather than fog. Marked by its red at 2 or more.
            p.albedo  = { 2.f + std::clamp( float( *rt_fog_glow ), 0.f, 4.f ), 0.80f, 0.84f };
            p.vel     = g_wind + FVector3{ Rnd( -0.05f, 0.05f ), Rnd( -0.05f, 0.05f ), 0.f };
            live++;
            break;
        }
    }
}

void Simulate( float dt )
{
    for( uint32_t i = 0; i < g_count; )
    {
        Puff& p = g_puffs[ i ];
        p.age += dt;
        if( p.age >= p.life )
        {
            p = g_puffs[ --g_count ];
            continue;
        }
        // turbulence: a slow curl, stronger as the puff ages (it leaves laminar)
        const float k    = std::min( 1.f, p.age / std::max( 0.2f, p.life * 0.3f ) );
        const float tsec = p.age * 1.7f + p.phase;
        const FVector3 curl{ std::sin( tsec ) * p.turb * k, std::cos( tsec * 1.3f ) * p.turb * k, 0.f };

        p.vel += ( FVector3{ 0, 0, p.buoy } + curl ) * dt;
        p.vel *= std::max( 0.f, 1.f - p.drag * dt );
        p.pos += ( p.vel + p.push ) * dt;
        p.push *= std::max( 0.f, 1.f - 1.6f * dt );
        p.radius += p.growth * dt;
        i++;
    }
}

// The wake (rt_smoke_wake): what moves through the smoke and the fog clears it, and the
// smoke flows back in -- the player walking through a fog bank, a rocket's flight, a
// blast's ball (which also shoves the puffs round it away). A clearing is a "carve": a
// soft sphere sent with the puffs, its density negative (Raze RT's shaders, Smoke.h: it
// thins the smoke where it is, as much as it is strong). A trail of them, each fading.
//
// The clearing must be STRONG to be seen at all: a fog bank is dense (it is drawn as
// a veil, its density thinned only by the froxel's depth), so thinning it by half
// still reads as fog. A carve is sent stronger than 1 -- the shader clamps the
// clearing at 1 -- which gives it a fully clear core over most of its radius and a
// soft rim; as it ages it weakens and shrinks, the fog flowing back in from its edge.
//
// A bullet's track is a carve too, stretched along the shot: the shader's puff is an
// ellipsoid round an axis (sent as the view's for a puff, here the shot's), so a few
// long ones laid end to end make a tube through the smoke.
struct Carve
{
    FVector3 pos;
    float    radius   = 0.5f;
    float    strength = 1.f;
    float    age      = 0.f;
    float    life     = 2.f;
    float    grow     = 0.f; // seconds to reach its radius (a blast's ball), 0 = at once
    bool     mistOnly = false; // clears the ground fog only (a rocket: not its own smoke trail)
    FVector3 axis{ 0.f, 0.f, 0.f }; // a stretched one (a bullet's track): along this...
    float    halfLen  = 0.f;        // ...this far each way (0 = a ball)
};
std::vector< Carve >                          g_carves;
constexpr size_t                              kMaxCarves  = 96;                         // kept
constexpr uint32_t                            kSendCarves = RG_MAX_SMOKE_PUFFS * 3 / 8; // sent (the freshest)
std::unordered_map< int, FVector3 >           g_movers;      // actor -> where it was last frame
std::unordered_map< int, FVector3 >           g_moverTrail;  // actor -> its last trail point
FVector3                                      g_viewTrail{ 1e9f, 1e9f, 1e9f };
FVector3                                      g_viewLast{ 1e9f, 1e9f, 1e9f };

Carve& AddCarve( const FVector3& pos, float radius, float strength, float life, float grow = 0.f, bool mistOnly = false )
{
    if( g_carves.size() >= kMaxCarves )
    {
        // the oldest gives way
        size_t oldest = 0;
        for( size_t i = 1; i < g_carves.size(); i++ )
            if( g_carves[ i ].age / g_carves[ i ].life > g_carves[ oldest ].age / g_carves[ oldest ].life ) oldest = i;
        g_carves[ oldest ] = g_carves.back();
        g_carves.pop_back();
    }
    Carve c;
    c.pos      = pos;
    c.radius   = radius;
    c.strength = strength;
    c.life     = life;
    c.grow     = grow;
    c.mistOnly = mistOnly;
    g_carves.push_back( c );
    return g_carves.back();
}

// is there smoke or fog near here at all (a carve with nothing to clear is not sent)
bool SmokeNear( const FVector3& pos, float radius )
{
    for( uint32_t i = 0; i < g_count; i++ )
    {
        const float r = g_puffs[ i ].radius + radius;
        if( ( g_puffs[ i ].pos - pos ).LengthSquared() < r * r ) return true;
    }
    return false;
}

// a blast: the puffs round it shoved away
void AddBlast( const FVector3& c, float radius )
{
    if( !*rt_smoke_wake ) return;
    const float strength = std::clamp( float( *rt_smoke_wake_strength ), 0.f, 3.f );
    for( uint32_t i = 0; i < g_count; i++ )
    {
        Puff&          p = g_puffs[ i ];
        FVector3       d = p.pos - c;
        const float    l = d.Length();
        const float    R = radius * 1.6f + p.radius * 0.5f;
        if( l >= R ) continue;
        d = l > 1e-3f ? d / l : FVector3{ 0, 0, 1 };
        p.push += d * ( 5.f * strength * ( 1.f - l / R ) );
    }
    // (no clearing of its own: the blast's own smoke billows up right there -- shoving
    // the smoke and fog that was there out of the way is what opens the space)
}

// a bullet: a tube through the smoke and the fog along its track (metres), the smoke
// puffs it passes through shoved off the line
void AddTrail( FVector3 a, FVector3 b )
{
    if( !*rt_smoke_wake || g_count == 0 ) return;
    const float strength = std::clamp( float( *rt_smoke_wake_strength ), 0.f, 3.f );
    const float radius   = std::clamp( float( *rt_smoke_bullet_radius ), 0.05f, 2.f );
    if( strength <= 0.f ) return;
    FVector3    dir = b - a;
    float       len = dir.Length();
    if( len < 0.5f ) return;
    dir /= len;
    len = std::min( len, 50.f );
    // (not from inside the barrel: from a little way out)
    a += dir * std::min( 0.4f, len * 0.5f );
    len -= std::min( 0.4f, len * 0.5f );

    // pieces up to 2.5 m each way, one every piece's half length: end to end they
    // make an even tube (the clearings of two neighbours add up where they meet)
    const int   n    = std::max( 1, int( std::ceil( len / 2.5f ) ) );
    const float half = len / n;
    for( int k = 0; k < n; k++ )
    {
        const FVector3 c = a + dir * ( len * ( k + 0.5f ) / n );
        if( !SmokeNear( c, half + radius ) ) continue;
        Carve& cv  = AddCarve( c, radius, 3.f * strength, 2.5f );
        cv.axis    = dir;
        cv.halfLen = half;
    }
    for( uint32_t i = 0; i < g_count; i++ )
    {
        Puff& p = g_puffs[ i ];
        if( p.fog ) continue;
        FVector3    off   = p.pos - a;
        const float along = off | dir;
        if( along < -p.radius || along > len + p.radius ) continue;
        off -= dir * along;
        const float l = off.Length();
        const float R = p.radius + radius * 2.f;
        if( l >= R ) continue;
        p.push += ( l > 1e-3f ? off / l : RndUnit() ) * ( 2.5f * strength * ( 1.f - l / R ) );
    }
}

void WakeSources( float dt, const FVector3& eye, float farM )
{
    // the carves age; the faded ones go
    for( size_t i = 0; i < g_carves.size(); )
    {
        g_carves[ i ].age += dt;
        if( g_carves[ i ].age >= g_carves[ i ].life )
        {
            g_carves[ i ] = g_carves.back();
            g_carves.pop_back();
            continue;
        }
        i++;
    }
    if( !*rt_smoke_wake || g_count == 0 )
    {
        g_viewLast = eye;
        return;
    }
    const float strength = std::clamp( float( *rt_smoke_wake_strength ), 0.f, 3.f );

    // the player: his body (from the eye down), and a trail where he went
    {
        const FVector3 body = eye - FVector3{ 0.f, 0.f, 0.7f };
        const float    step = ( eye - g_viewLast ).Length();
        if( g_viewLast.X < 1e8f && step > 1e-4f && step < 2.f && dt > 0.f )
        {
            // the puffs he walks through are nudged along
            const FVector3 move = ( eye - g_viewLast ) / dt;
            for( uint32_t i = 0; i < g_count; i++ )
            {
                Puff& p = g_puffs[ i ];
                const float l = ( p.pos - body ).Length();
                const float R = p.radius + 0.6f;
                if( l < R ) p.push += FVector3{ move.X, move.Y, 0.f } * ( 0.6f * strength * ( 1.f - l / R ) * dt );
            }
        }
        // a clearing round him every half metre, a little ahead of him as he goes (he
        // parts the fog), and the trail of them behind him filling in over seconds
        FVector3 ahead{ 0.f, 0.f, 0.f };
        if( g_viewLast.X < 1e8f && dt > 0.f )
        {
            const FVector2 mv{ eye.X - g_viewLast.X, eye.Y - g_viewLast.Y };
            const float    sp = mv.Length() / dt;
            if( sp > 1.f && sp < 60.f ) ahead = FVector3{ mv.X, mv.Y, 0.f } / mv.Length() * 0.4f;
        }
        g_viewLast = eye;
        if( ( body - g_viewTrail ).Length() > 0.5f && SmokeNear( body, 1.3f ) )
        {
            AddCarve( body + ahead, 1.3f, 2.5f * strength, 5.f );
            g_viewTrail = body;
        }
    }

    // fast movers: rockets, fireballs, flares, thrown things -- a trail behind them
    std::unordered_map< int, FVector3 > nowPos;
    TSpriteIterator< DCoreActor > it;
    while( auto a = it.Next() )
    {
        if( ( a->spr.cstat & CSTAT_SPRITE_INVISIBLE ) || a->spr.scale.X == 0 ) continue;
        const FVector3 pos = WorldToRTv( a->spr.pos.X, a->spr.pos.Y, a->spr.pos.Z );
        if( ( pos - eye ).LengthSquared() > farM * farM ) continue;
        if( ( FVector2{ pos.X - eye.X, pos.Y - eye.Y } ).Length() < 1.f ) continue; // (the player himself)
        const int idx = a->GetIndex();
        nowPos[ idx ] = pos;
        auto last = g_movers.find( idx );
        if( last == g_movers.end() || dt <= 0.f ) continue;
        const float speed = ( pos - last->second ).Length() / dt;
        if( speed < 7.f || speed > 200.f ) continue;
        // the fog it flies through: a tunnel (the fog only -- a rocket's own smoke
        // trail is right behind it)
        auto& trail = g_moverTrail[ idx ];
        if( ( pos - trail ).Length() > 0.6f && SmokeNear( pos, 0.8f ) )
        {
            AddCarve( pos, 0.7f, 2.5f * strength, 3.f, 0.f, true );
            trail = pos;
        }
        // and the older smoke it passes is swept aside (not its own new trail)
        const FVector3 dir = ( pos - last->second ) / std::max( 1e-4f, float( ( pos - last->second ).Length() ) );
        for( uint32_t i = 0; i < g_count; i++ )
        {
            Puff& p = g_puffs[ i ];
            if( p.fog || p.age < 1.f ) continue;
            FVector3    off = p.pos - pos;
            off -= dir * ( off | dir );
            const float l = off.Length();
            const float R = p.radius + 0.5f;
            if( l >= R || ( p.pos - pos ).Length() > R * 1.5f ) continue;
            p.push += ( l > 1e-3f ? off / l : FVector3{ 0, 0, 1 } ) * ( 2.f * strength * ( 1.f - l / R ) * dt * 8.f );
        }
    }
    g_movers = std::move( nowPos );
    for( auto t = g_moverTrail.begin(); t != g_moverTrail.end(); )
    {
        if( !g_movers.count( t->first ) ) t = g_moverTrail.erase( t );
        else ++t;
    }
}

float Envelope( const Puff& p )
{
    if( p.attack > 0.f || p.sustain > 0.f || p.release > 0.f )
    {
        if( p.attack > 0.f && p.age < p.attack )
        {
            return p.age / p.attack;
        }
        const float a = p.age - p.attack;
        if( a < p.sustain )
        {
            return 1.f;
        }
        const float r = std::max( p.release, 0.05f );
        const float e = 1.f - ( a - p.sustain ) / r;
        // and never past the puff's life
        const float t = 1.f - p.age / p.life;
        return std::clamp( std::min( e, t * 4.f ), 0.f, 1.f );
    }
    const float t = std::clamp( p.age / p.life, 0.f, 1.f );
    return ( 1.f - t ) * ( 1.f - t );
}

} // namespace

// Once a frame, before the frame is drawn: make, move and pack the smoke.
// 'far' is the froxel volume's far plane (metres).
uint32_t RT_UpdateSmoke( const RgFloat3D& eyeIn, const RgFloat3D& fwdIn, const RgFloat3D& upIn, const RgFloat3D& rightIn,
                         float far, RgFloat4D* outPuffs, RgFloat4D* outAlbDen, RgFloat4D* outShape )
{
    static uint64_t s_last = 0;
    const uint64_t  nowMs  = I_msTime();
    const float     dt     = s_last ? std::clamp( float( nowMs - s_last ) / 1000.f, 0.f, 0.1f ) : 0.f;
    s_last                 = nowMs;
    const double now       = double( nowMs ) / 1000.0;

    if( g_levelGen != RT_LevelGeneration() )
    {
        g_levelGen = RT_LevelGeneration();
        g_count    = 0;
        g_actors.clear();
        g_seen.clear();
        std::fill( std::begin( g_emitterNext ), std::end( g_emitterNext ), 0.0 );
        g_lastShot = g_rtShotCounter;
        g_gameBurst.clear();
        g_gameNext.clear();
        g_carves.clear();
        g_movers.clear();
        g_moverTrail.clear();
        g_viewTrail = g_viewLast = FVector3{ 1e9f, 1e9f, 1e9f };
    }

    if( !*rt_smoke )
    {
        g_count = 0;
        return 0;
    }

    const FVector3 eye{ eyeIn.data[ 0 ], eyeIn.data[ 1 ], eyeIn.data[ 2 ] };
    const FVector3 fwd{ fwdIn.data[ 0 ], fwdIn.data[ 1 ], fwdIn.data[ 2 ] };
    const FVector3 up{ upIn.data[ 0 ], upIn.data[ 1 ], upIn.data[ 2 ] };
    const FVector3 right{ rightIn.data[ 0 ], rightIn.data[ 1 ], rightIn.data[ 2 ] };
    const float    farM = std::max( 1.f, float( *rt_smoke_far ) );

    // the game is paused (or the menu is up): the smoke stands still
    if( dt > 0.f )
    {
        const ResolvedLightOverlaySet& set = GetResolvedLightOverlaySet();
        ShotSource( set, eye, fwd, right, up );
        ActorSources( set, now, eye, farM );
        EmitterSources( set, now, eye, farM );
        // a game without smoke rules (Blood) names its own smoke
        if( set.smokeActorRules.Size() == 0 && set.smokeEventRules.Size() == 0 )
        {
            GameSources( now, eye, fwd, right, up, farM );
        }
        FogSources( dt );
        WakeSources( dt, eye, farM );
        Simulate( dt );
    }

    // packing (Doom 64 RT's): the puff's along-view radius is padded to half a
    // froxel slice, so the grid can resolve it, and its density thinned to match
    constexpr int   kSlices    = 64;
    constexpr float kCellCoeff = 0.001f;
    const float     sliceM     = std::max( 0.001f, far / float( kSlices ) );

    struct Cand
    {
        float    d2;
        uint32_t idx;
    };
    // the wake's carves first (each as a puff of negative density, see above), those
    // with smoke to clear; then the puffs, the nearest if they do not all fit
    uint32_t nc = 0;
    if( *rt_smoke_wake && g_count > 0 && !g_carves.empty() )
    {
        // the freshest first, if they do not all fit
        std::vector< const Carve* > order;
        order.reserve( g_carves.size() );
        for( const Carve& c : g_carves ) order.push_back( &c );
        std::sort( order.begin(), order.end(), []( const Carve* a, const Carve* b ) {
            return a->age / std::max( a->life, 0.01f ) < b->age / std::max( b->life, 0.01f );
        } );
        for( const Carve* cp : order )
        {
            const Carve& c = *cp;
            if( nc >= kSendCarves ) break;
            const float t = std::clamp( c.age / std::max( c.life, 0.01f ), 0.f, 1.f );
            // weaker and smaller as it ages: the smoke flows back in from its rim
            const float fade   = 1.f - t;
            const float shrink = 0.45f + 0.55f * ( 1.f - t );
            const float r      = shrink * ( c.grow > 0.f ? c.radius * ( 0.35f + 0.65f * std::min( 1.f, c.age / c.grow ) ) : c.radius );
            const float reach  = std::max( r, c.halfLen );
            if( fade <= 0.01f || ( c.pos - eye ).Length() > far + reach || !SmokeNear( c.pos, reach ) ) continue;
            if( c.halfLen > 0.f )
            {
                // a bullet's track: long along the shot, its radius across it
                outPuffs[ nc ] = RgFloat4D{ c.pos.X, c.pos.Y, c.pos.Z, std::max( c.halfLen, 0.5f * sliceM ) };
                outShape[ nc ] = RgFloat4D{ r, c.axis.X, c.axis.Y, c.axis.Z };
            }
            else
            {
                FVector3    vdir = c.pos - eye;
                const float vlen = vdir.Length();
                vdir             = vlen > 1e-4f ? vdir / vlen : FVector3{ 0.f, 0.f, 1.f };
                outPuffs[ nc ]   = RgFloat4D{ c.pos.X, c.pos.Y, c.pos.Z, std::max( r, 0.5f * sliceM ) };
                outShape[ nc ]   = RgFloat4D{ r, vdir.X, vdir.Y, vdir.Z };
            }
            outAlbDen[ nc ] = RgFloat4D{ c.mistOnly ? 2.8f : 0.8f, 0.81f, 0.84f, -c.strength * fade };
            nc++;
        }
    }

    std::array< Cand, RG_MAX_SMOKE_PUFFS > cand{};
    uint32_t                               n = 0;
    for( uint32_t i = 0; i < g_count; i++ )
    {
        const float d2 = ( g_puffs[ i ].pos - eye ).LengthSquared();
        if( d2 < far * far )
        {
            cand[ n++ ] = { d2, i };
        }
    }
    if( n + nc > RG_MAX_SMOKE_PUFFS )
    {
        std::sort( cand.begin(), cand.begin() + n, []( const Cand& a, const Cand& b ) { return a.d2 < b.d2; } );
        n = RG_MAX_SMOKE_PUFFS - nc;
    }

    for( uint32_t k = 0; k < n; k++ )
    {
        const uint32_t i = nc + k;
        const uint32_t ci = k;
        const Puff& p      = g_puffs[ cand[ ci ].idx ];
        const float dilute = p.radius > p.radius0 ? ( p.radius0 / p.radius ) : 1.f;
        const float rAlong = std::max( p.radius, 0.5f * sliceM );
        const float thin   = p.radius / rAlong;

        FVector3    vdir = p.pos - eye;
        const float vlen = vdir.Length();
        vdir             = vlen > 1e-4f ? vdir / vlen : FVector3{ 0.f, 0.f, 1.f };

        outPuffs[ i ]  = RgFloat4D{ p.pos.X, p.pos.Y, p.pos.Z, rAlong };
        outShape[ i ]  = RgFloat4D{ p.radius, vdir.X, vdir.Y, vdir.Z };
        // (a fog bank's glow as rt_fog_glow is now: the slider works on the banks there are)
        const float albR = p.fog ? 2.f + std::clamp( float( *rt_fog_glow ), 0.f, 4.f ) : p.albedo.X;
        outAlbDen[ i ] = RgFloat4D{ albR, p.albedo.Y, p.albedo.Z,
                                    p.density * thin * dilute * sliceM / kCellCoeff * Envelope( p ) };
    }

    if( *rt_smoke_debug )
    {
        static int s_tick = 0;
        if( ( ++s_tick % 60 ) == 0 )
        {
            Printf( "RT smoke: %u live, %u sent, %u wake carves sent (of %u), far %.0f m\n", g_count, n, nc, unsigned( g_carves.size() ), far );
        }
    }
    return n + nc;
}

CCMD( rt_smoke_test )
{
    // a puff cloud two metres in front of the player
    const ResolvedLightOverlaySet& set = GetResolvedLightOverlaySet();
    if( set.smokeStyles.Size() == 0 )
    {
        Printf( "No smoke styles loaded (LIGHTOVR)\n" );
        return;
    }
    Printf( "RT smoke: %u styles, %u actor rules, %u event rules, %u map emitters\n", set.smokeStyles.Size(),
            set.smokeActorRules.Size(), set.smokeEventRules.Size(), set.mapSmokeEmitterRules.Size() );
    g_rtShotEvent = "nri.smoke.test";
    g_rtShotCounter++;
}

// a hitscan shot (a bullet, a pellet): its track, from where it was fired to where it
// hit, in map coordinates -- it leaves a tube through the smoke and fog it crossed
// (rt_smoke_wake; the game calls this: Blood's actFireVector, Duke's HitscanAttack)
void RT_AddShotTrail( const DVector3& from, const DVector3& to )
{
    if( !RT_IsActive() || !*rt_smoke ) return;
    AddTrail( WorldToRTv( from.X, from.Y, from.Z ), WorldToRTv( to.X, to.Y, to.Z ) );
}
