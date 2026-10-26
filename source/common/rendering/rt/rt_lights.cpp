// Raze RT: the analytic lights given to RTGL1 each frame.
//
//  - Duke-RT's LIGHTOVR (core/lightoverlay.cpp, mounted with the overlay):
//      map lights, surface lights and the map's directional light (the moon),
//      actor rules (fires, explosions, projectiles, pickups...) at the live actors.
//  - sector lights: a soft light near the ceiling of every lit indoor sector, from
//    the sector's shade, standing in for Duke-RT's sector ambient lighting.
//
// Coordinates: Raze's world space is x, y, z with z down; RTGL1's is x, -y, -z in
// metres. Duke-RT's "path tracing" space (surface lights, actor offsets, directions)
// is x, -z, -y; so RTGL1 = (pt.x, pt.z, pt.y).

#include "rt_backend.h"

#include "lightoverlay.h"
#include "coreactor.h"
#include "gamefuncs.h"
#include "maptypes.h"
#include "texinfo.h"
#include "mapinfo.h"
#include "texturemanager.h"
#include "buildtiles.h"

#include <unordered_map>

CVARD( Bool, rt_lightovr, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: lights from Duke-RT's LIGHTOVR" )
CVARD( Float, rt_lightovr_scale, 10.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: brightness of LIGHTOVR point lights" )
CVARD( Float, rt_lightovr_sunscale, 20.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: brightness of the LIGHTOVR directional light (lux per unit)" )
CVARD( Bool, rt_lightovr_actors, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: LIGHTOVR actor lights (fire, explosions, projectiles)" )
CVARD( Bool, rt_sector_lights, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: a soft light in every lit indoor sector" )
CVARD( Float, rt_sector_light_scale, 30.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: brightness of the sector lights" )
CVARD( Float, rt_sector_light_min, 0.35f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: sectors darker than this (0..1 of full light) get no sector light" )
CVARD( Float, rt_sector_light_height, 0.2f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: sector light height, 0 = at the ceiling, 1 = on the floor" )
CVARD( Float, rt_sector_light_smooth, 0.15f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: how fast sector lights follow shade changes (1 = at once)" )
CVARD( Float, rt_switch_light_boost, 2.5f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: how much brighter a switched-on room's light is than an ordinary room's" )
CVARD( Bool, rt_sector_lights_outdoor, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: sector lights under the sky too" )

namespace
{

using namespace rtx;

constexpr uint64_t SectorLightBase  = 0x100000000ull;
constexpr uint64_t MapLightBase     = 0x200000000ull;
constexpr uint64_t SurfaceLightBase = 0x300000000ull;
constexpr uint64_t ActorLightBase   = 0x400000000ull;

RgFloat3D WorldToRT( double x, double y, double z )
{
    return { float( x ) * ONEGAMEUNIT_IN_METERS, float( -y ) * ONEGAMEUNIT_IN_METERS, float( -z ) * ONEGAMEUNIT_IN_METERS };
}

RgFloat3D PTToRT( const float p[ 3 ] )
{
    return { p[ 0 ] * ONEGAMEUNIT_IN_METERS, p[ 2 ] * ONEGAMEUNIT_IN_METERS, p[ 1 ] * ONEGAMEUNIT_IN_METERS };
}

RgColor4DPacked32 PackColor( const float c[ 3 ] )
{
    return rt.rgUtilPackColorFloat4D( std::clamp( c[ 0 ], 0.f, 1.f ),
                                      std::clamp( c[ 1 ], 0.f, 1.f ),
                                      std::clamp( c[ 2 ], 0.f, 1.f ),
                                      1.f );
}

uint64_t Hash64( uint64_t x )
{
    x = ( x ^ ( x >> 30 ) ) * 0xBF58476D1CE4E5B9ull;
    x = ( x ^ ( x >> 27 ) ) * 0x94D049BB133111EBull;
    return x ^ ( x >> 31 );
}

float Rand01( uint64_t seed )
{
    return float( Hash64( seed ) & 0xFFFFFF ) / float( 0xFFFFFF );
}

// Duke-RT lights fade out at 'radius' (game units) with a shaped falloff and
// 'intensity' at the centre; RTGL1's are physical (inverse square, lumens). This
// matches them at about a third of the radius.
float OverlayToLumens( float intensity, float radius )
{
    return std::max( 0.f, *rt_lightovr_scale * intensity * radius * radius * 5.0e-4f );
}

void UploadSphere( uint64_t id, const RgFloat3D& pos, RgColor4DPacked32 color, float lumens, float sourceRadiusM )
{
    if( lumens <= 0.01f )
    {
        return;
    }
    auto sph = RgLightSphericalEXT{
        .sType     = RG_STRUCTURE_TYPE_LIGHT_SPHERICAL_EXT,
        .pNext     = nullptr,
        .color     = color,
        .intensity = lumens,
        .position  = pos,
        .radius    = sourceRadiusM,
    };
    auto info = RgLightInfo{
        .sType        = RG_STRUCTURE_TYPE_LIGHT_INFO,
        .pNext        = &sph,
        .uniqueID     = id,
        .isExportable = false,
    };
    RG_CHECK( rt.rgUploadLight( &info ) );
}

float FlickerScale( uint64_t key, uint32_t frames, uint32_t time )
{
    if( frames <= 1 )
    {
        return 1.f;
    }
    // a new random level every 'frames' tics
    return 0.55f + 0.45f * Rand01( key * 31 + time / frames );
}

//
// sector lights
//

void UploadSectorLights()
{

    if( !*rt_sector_lights || sector.Size() == 0 )
    {
        return;
    }

    const float ns = float( std::max( numshades, 2 ) );

    // Build's flickering and strobing sectors change their shade every tic; following
    // that exactly makes the denoiser's history useless (splotchy shadows), so the
    // light follows it softly.
    static std::vector< float > s_smoothed;
    static const void*          s_level = nullptr;
    if( s_smoothed.size() != sector.Size() || s_level != currentLevel )
    {
        s_smoothed.assign( sector.Size(), -1.f );
        s_level = currentLevel;
    }

    for( unsigned i = 0; i < sector.Size(); i++ )
    {
        const sectortype& sec = sector[ i ];
        if( ( sec.ceilingstat & CSTAT_SECTOR_SKY ) && !*rt_sector_lights_outdoor )
        {
            continue;
        }
        if( sec.walls.Size() < 3 )
        {
            continue;
        }

        const int   shade = ( int( sec.ceilingshade ) + int( sec.floorshade ) ) / 2;
        const float level = std::clamp( 1.f - float( shade ) / ns, 0.f, 1.f );
        if( level < *rt_sector_light_min )
        {
            continue;
        }

        // centroid and area (shoelace) of the outline
        double area2 = 0, cx = 0, cy = 0;
        const unsigned n = sec.walls.Size();
        for( unsigned w = 0; w < n; w++ )
        {
            const auto& a = sec.walls[ w ].pos;
            const auto& b = sec.walls[ w ].point2Wall()->pos;
            const double cr = a.X * b.Y - b.X * a.Y;
            area2 += cr;
            cx += ( a.X + b.X ) * cr;
            cy += ( a.Y + b.Y ) * cr;
        }
        if( std::abs( area2 ) < 1.0 )
        {
            continue;
        }
        cx /= 3.0 * area2;
        cy /= 3.0 * area2;
        const double areaM2 = std::abs( area2 ) * 0.5 / ( 32.0 * 32.0 );

        const double height = sec.floorz - sec.ceilingz;
        if( height < 4.0 )
        {
            continue;
        }
        const double z = sec.ceilingz + height * std::clamp( float( *rt_sector_light_height ), 0.02f, 0.98f );

        // a switched room: off is about half light, which must still be dark
        const float sw     = RT_SectorSwitch( int( i ) );
        // switched on, a room is meant to light up: brighter than an ordinary one
        const float onness = sw < 0.f ? 1.f : 0.03f + ( float( *rt_switch_light_boost ) - 0.03f ) * sw;
        float lumens = *rt_sector_light_scale * level * level * onness *
                       float( std::clamp( std::sqrt( areaM2 ), 1.0, 25.0 ) );
        float& sm = s_smoothed[ i ];
        sm        = sm < 0.f ? lumens : sm + ( lumens - sm ) * std::clamp( float( *rt_sector_light_smooth ), 0.01f, 1.f );
        lumens    = sm;

        UploadSphere( SectorLightBase + i, WorldToRT( cx, cy, z ), RG_PACKED_COLOR_WHITE, lumens, 0.25f );
    }
}

//
// LIGHTOVR
//

// which actor rules apply to a class, worked out once per class
const std::vector< unsigned >& RulesForClass( const ResolvedLightOverlaySet& set, PClass* cls )
{
    static std::unordered_map< PClass*, std::vector< unsigned > > s_rulesByClass;
    static uint32_t                                                s_generation = ~0u;
    if( s_generation != set.resolvedGeneration )
    {
        s_rulesByClass.clear();
        s_generation = set.resolvedGeneration;
    }

    auto found = s_rulesByClass.find( cls );
    if( found == s_rulesByClass.end() )
    {
        std::vector< unsigned > list;
        for( unsigned r = 0; r < set.actorRules.Size(); r++ )
        {
            const auto& rule = set.actorRules[ r ];
            if( rule.actorClassResolved && rule.actorClass && ( cls == rule.actorClass || cls->IsDescendantOf( rule.actorClass ) ) )
            {
                list.push_back( r );
            }
        }
        found = s_rulesByClass.emplace( cls, std::move( list ) ).first;
    }
    return found->second;
}

int ActorRuleFlags( DCoreActor* actor )
{
    if( !*rt_lightovr || !currentLevel || !actor || !actor->GetClass() )
    {
        return 0;
    }
    const ResolvedLightOverlaySet& set   = GetResolvedLightOverlaySet();
    int                            flags = 0;
    for( unsigned r : RulesForClass( set, actor->GetClass() ) )
    {
        const auto& rule = set.actorRules[ r ];
        if( rule.hasShadowCast && !rule.shadowCast )
        {
            flags |= 1;
        }
        if( rule.hasFullbright && rule.fullbright )
        {
            flags |= 2;
        }
    }
    return flags;
}

struct InstallHook
{
    InstallHook() { RT_ActorRuleFlagsHook = &ActorRuleFlags; }
} g_installHook;


void UploadOverlayLights( uint32_t time )
{
    if( !*rt_lightovr || !currentLevel )
    {
        return;
    }

    const ResolvedLightOverlaySet& set = GetResolvedLightOverlaySet();

    // the map's directional light: RTGL1 allows one
    if( set.directionalRules.Size() > 0 )
    {
        const auto& d = set.directionalRules.Last();
        const float len = std::sqrt( d.direction[ 0 ] * d.direction[ 0 ] + d.direction[ 1 ] * d.direction[ 1 ] +
                                     d.direction[ 2 ] * d.direction[ 2 ] );
        const float intensity = ( d.hasIntensity ? d.intensity : 1.f ) * *rt_lightovr_sunscale;
        if( d.hasDirection && len > 1e-4f && intensity > 0.f )
        {
            // Duke-RT: towards the light, path tracing space; RTGL1: from the light
            const RgFloat3D toLight = { d.direction[ 0 ] / len, d.direction[ 2 ] / len, d.direction[ 1 ] / len };
            auto dir = RgLightDirectionalEXT{
                .sType                  = RG_STRUCTURE_TYPE_LIGHT_DIRECTIONAL_EXT,
                .pNext                  = nullptr,
                .color                  = PackColor( d.color ),
                .intensity              = intensity,
                .direction              = { -toLight.data[ 0 ], -toLight.data[ 1 ], -toLight.data[ 2 ] },
                .angularDiameterDegrees = d.hasAngularSize ? std::max( 0.05f, d.angularSize * 57.29578f ) : 0.5f,
            };
            auto info = RgLightInfo{
                .sType        = RG_STRUCTURE_TYPE_LIGHT_INFO,
                .pNext        = &dir,
                .uniqueID     = MapLightBase - 1,
                .isExportable = false,
            };
            RG_CHECK( rt.rgUploadLight( &info ) );
        }
    }

    // placed lights
    for( unsigned i = 0; i < set.mapLightRules.Size(); i++ )
    {
        const auto& r = set.mapLightRules[ i ];
        if( !r.hasIntensity || r.intensity <= 0.f || r.radius <= 0.f )
        {
            continue;
        }

        double x = 0, y = 0, z = 0;
        if( r.anchorType == LightOverlayAnchorType::Position && r.hasAnchorPosition )
        {
            x = r.anchorPosition[ 0 ];
            y = r.anchorPosition[ 1 ];
            z = r.anchorPosition[ 2 ];
        }
        else if( r.anchorType == LightOverlayAnchorType::Sector && r.anchorIndex >= 0 && unsigned( r.anchorIndex ) < sector.Size() )
        {
            const auto& sec = sector[ r.anchorIndex ];
            for( auto& w : sec.walls )
            {
                x += w.pos.X;
                y += w.pos.Y;
            }
            x /= std::max( 1u, sec.walls.Size() );
            y /= std::max( 1u, sec.walls.Size() );
            z = ( sec.floorz + sec.ceilingz ) * 0.5;
        }
        else if( r.anchorType == LightOverlayAnchorType::Wall && r.anchorIndex >= 0 && unsigned( r.anchorIndex ) < wall.Size() )
        {
            const auto& w = wall[ r.anchorIndex ];
            const auto& p2 = w.point2Wall()->pos;
            x = ( w.pos.X + p2.X ) * 0.5;
            y = ( w.pos.Y + p2.Y ) * 0.5;
            const auto sec = w.sectorp();
            z = ( sec->floorz + sec->ceilingz ) * 0.5;
        }
        else
        {
            continue;
        }
        if( r.hasOffset )
        {
            x += r.offset[ 0 ];
            y += r.offset[ 1 ];
            z += r.offset[ 2 ];
        }

        const uint64_t id     = MapLightBase + i;
        const float    lumens = OverlayToLumens( r.intensity, r.radius ) * FlickerScale( id, r.flickerFrames, time );
        UploadSphere( id, WorldToRT( x, y, z ), PackColor( r.color ), lumens, 0.05f );
    }

    // surface lights: a light just off an authored spot on a surface
    for( unsigned i = 0; i < set.surfaceLightRules.Size(); i++ )
    {
        const auto& r = set.surfaceLightRules[ i ];
        if( !r.hasPosition || !r.hasNormal || r.intensity <= 0.f || r.radius <= 0.f )
        {
            continue;
        }
        const float offset = r.hasOffset ? r.offset : 0.5f;
        const float p[ 3 ] = {
            r.position[ 0 ] + r.normal[ 0 ] * offset,
            r.position[ 1 ] + r.normal[ 1 ] * offset,
            r.position[ 2 ] + r.normal[ 2 ] * offset,
        };
        const float size = r.hasSize ? std::max( r.size[ 0 ], r.size[ 1 ] ) : 16.f;
        UploadSphere( SurfaceLightBase + i, PTToRT( p ), PackColor( r.color ), OverlayToLumens( r.intensity, r.radius ),
                      std::clamp( size * 0.5f * ONEGAMEUNIT_IN_METERS, 0.02f, 1.f ) );
    }

    // actor lights
    if( !*rt_lightovr_actors || set.actorRules.Size() == 0 )
    {
        return;
    }

    TSpriteIterator< DCoreActor > it;
    while( auto actor = it.Next() )
    {
        if( ( actor->spr.cstat & CSTAT_SPRITE_INVISIBLE ) || actor->spr.scale.X == 0 || actor->spr.scale.Y == 0 )
        {
            continue;
        }
        PClass* cls = actor->GetClass();
        if( !cls )
        {
            continue;
        }

        const auto& rules = RulesForClass( set, cls );
        if( rules.empty() )
        {
            continue;
        }

        // the sprite's middle
        double h = 0;
        if( auto tex = TexMan.GetGameTexture( actor->spr.spritetexture() ) )
        {
            h = tex->GetDisplayHeight() * actor->spr.scale.Y;
        }
        const bool   ycenter = ( actor->spr.cstat & CSTAT_SPRITE_YCENTER ) != 0;
        const double cz      = ycenter ? actor->spr.pos.Z : actor->spr.pos.Z - h * 0.5;

        const int tile = legacyTileNum( actor->spr.spritetexture() );

        for( unsigned r : rules )
        {
            const auto& rule = set.actorRules[ r ];
            if( rule.intensity <= 0.f || rule.radius <= 0.f )
            {
                continue;
            }
            if( rule.hasTileFilter && rule.tileFilter >= 0 && tile != rule.tileFilter )
            {
                continue;
            }

            const uint64_t id = ActorLightBase + ( uint64_t( actor->GetIndex() ) << 8 ) + r;

            // offset: Duke-RT path tracing space
            const RgFloat3D base = WorldToRT( actor->spr.pos.X, actor->spr.pos.Y, cz );
            const RgFloat3D off  = PTToRT( rule.offset );
            const RgFloat3D pos  = { base.data[ 0 ] + off.data[ 0 ], base.data[ 1 ] + off.data[ 1 ], base.data[ 2 ] + off.data[ 2 ] };

            float intensity = rule.intensity;
            if( rule.hasRandom )
            {
                const float t = Rand01( id * 7919 + time );
                intensity += rule.randomIntensityRange[ 0 ] + t * ( rule.randomIntensityRange[ 1 ] - rule.randomIntensityRange[ 0 ] );
            }
            intensity *= FlickerScale( id, rule.flickerFrames, time );

            UploadSphere( id, pos, PackColor( rule.color ), OverlayToLumens( intensity, rule.radius ), 0.05f );
        }
    }
}

} // namespace

uint32_t RT_LevelGeneration()
{
    static const void* s_data  = nullptr;
    static unsigned    s_count = 0;
    static const void* s_level = nullptr;
    static uint32_t    s_gen   = 0;
    if( s_data != sector.Data() || s_count != sector.Size() || s_level != currentLevel )
    {
        s_data  = sector.Data();
        s_count = sector.Size();
        s_level = currentLevel;
        s_gen++;
    }
    return s_gen;
}

void RT_UploadSceneLights( uint32_t time )
{
    UploadSectorLights();
    UploadOverlayLights( time );
}

CCMD( rt_lights_info )
{
    const ResolvedLightOverlaySet& set = GetResolvedLightOverlaySet();
    Printf( "LIGHTOVR for %s: %u actor rules, %u map lights, %u surface lights, %u directional\n",
            set.activeMapName.GetChars(),
            set.actorRules.Size(),
            set.mapLightRules.Size(),
            set.surfaceLightRules.Size(),
            set.directionalRules.Size() );
}
