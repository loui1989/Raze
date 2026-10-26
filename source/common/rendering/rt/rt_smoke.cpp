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
};

std::array< Puff, RG_MAX_SMOKE_PUFFS > g_puffs;
uint32_t                               g_count = 0;
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
        p.pos += p.vel * dt;
        p.radius += p.growth * dt;
        i++;
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

    for( uint32_t i = 0; i < n; i++ )
    {
        const Puff& p      = g_puffs[ cand[ i ].idx ];
        const float dilute = p.radius > p.radius0 ? ( p.radius0 / p.radius ) : 1.f;
        const float rAlong = std::max( p.radius, 0.5f * sliceM );
        const float thin   = p.radius / rAlong;

        FVector3    vdir = p.pos - eye;
        const float vlen = vdir.Length();
        vdir             = vlen > 1e-4f ? vdir / vlen : FVector3{ 0.f, 0.f, 1.f };

        outPuffs[ i ]  = RgFloat4D{ p.pos.X, p.pos.Y, p.pos.Z, rAlong };
        outShape[ i ]  = RgFloat4D{ p.radius, vdir.X, vdir.Y, vdir.Z };
        outAlbDen[ i ] = RgFloat4D{ p.albedo.X, p.albedo.Y, p.albedo.Z,
                                    p.density * thin * dilute * sliceM / kCellCoeff * Envelope( p ) };
    }

    if( *rt_smoke_debug )
    {
        static int s_tick = 0;
        if( ( ++s_tick % 60 ) == 0 )
        {
            Printf( "RT smoke: %u live, %u sent, far %.0f m\n", g_count, n, far );
        }
    }
    return n;
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
