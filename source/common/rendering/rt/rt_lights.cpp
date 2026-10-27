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
#include <queue>
#include <array>
#include "palette.h"
#include "palettecontainer.h"
#include "bitmap.h"

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
CVARD( Float, rt_flicker_amount, 0.6f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: 0..1, how deep flickering LIGHTOVR lights flicker" )
CVARD( Bool, rt_sector_light_tint, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: sector lights take the colour of the sector's palette" )
CVARD( Bool, rt_lightovr_fixtures, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: draw the lamp panels of LIGHTOVR surface lights" )
CVARD( Bool, rt_sector_lights_outdoor, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: sector lights under the sky too" )
CVARD( Float, rt_moon, 8.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: a faint moonlight in games without Duke-RT's lights (Blood), lux; 0 = none" )
CVARD( Float, rt_moon_angle, 35.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: the moon's direction, degrees around" )
CVARD( Float, rt_skysun, 4.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: the sky's own moon or sun -- from the brightest part of the sky in view, tinted by it, as strong as the sky is bright; 0 = the fixed rt_moon instead" )
CVARD( Float, rt_skysun_tint, 0.5f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: 0..1, how much the sky's moon takes the sky's colour (0 white; 1 the sky's own: a red sky's red on red, no contrast)" )
CVARD( Float, rt_skysun_size, 1.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: the sky's moon's size, degrees (small: crisp shadows)" )
EXTERN_CVAR( Float, rt_sky )
CVARD( Int, rt_sky_portal_debug, 0, 0, "RT: sky portals as bright magenta lights (1) -- to see where they are and what they light" )
CVARD( Float, rt_sky_portals, 35.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: windows and openings to the sky light the room behind them with the sky's own light (sky portals), this strong; 0 = off" )
CVARD( Float, rt_lightning, 60.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: a lightning flash's light from the sky, lux (Blood's storms)" )
CVARD( Float, rt_game_light_scale, 1.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: brightness of the lights the game names (Blood: explosions, fire, flares)" )
CVARD( Float, rt_light_reach, 40.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: lights further than this (metres, walking through openings) from the view are left out; 0 = all lights" )

namespace
{

using namespace rtx;

constexpr uint64_t SectorLightBase  = 0x100000000ull;
constexpr uint64_t MapLightBase     = 0x200000000ull;
constexpr uint64_t SurfaceLightBase = 0x300000000ull;
constexpr uint64_t ActorLightBase   = 0x400000000ull;
constexpr uint64_t GameLightBase    = 0x500000000ull;
constexpr uint64_t SkyPortalBase    = 0x600000000ull;

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

//
// light reach: only the lights near the view, measured through the openings
// between sectors, go to RTGL1. Each pixel tries a handful of lights picked at
// random from all uploaded ones; with every lit sector of a big level uploaded
// (hundreds), most picks are lights far away and a pixel with no history (just
// turned into view) finds its own lamp only after a few frames: black, jagged
// patches that smear in while turning, worst under strong lights.
//

std::vector< float >     s_reachDist;  // path distance to the sector, metres; <0 = not reached
std::vector< FVector3 >  s_reachEntry; // where the path enters the sector (RT metres)
bool                     s_reachOn = false;

void ComputeReach()
{
    s_reachOn = false;
    const float reach = *rt_light_reach;
    if( reach <= 0.f || g_rtViewSector < 0 || unsigned( g_rtViewSector ) >= sector.Size() )
    {
        return;
    }
    s_reachDist.assign( sector.Size(), -1.f );
    s_reachEntry.resize( sector.Size() );

    const RgFloat3D cam = WorldToRT( g_rtViewPos[ 0 ], g_rtViewPos[ 1 ], g_rtViewPos[ 2 ] );
    using Item = std::pair< float, int >;
    std::priority_queue< Item, std::vector< Item >, std::greater< Item > > q;
    s_reachDist[ g_rtViewSector ]  = 0.f;
    s_reachEntry[ g_rtViewSector ] = { cam.data[ 0 ], cam.data[ 1 ], cam.data[ 2 ] };
    q.push( { 0.f, g_rtViewSector } );
    while( !q.empty() )
    {
        const auto [ d, si ] = q.top();
        q.pop();
        if( d > s_reachDist[ si ] )
        {
            continue;
        }
        const FVector3 from = s_reachEntry[ si ];
        for( auto& w : sector[ si ].walls )
        {
            if( !w.twoSided() )
            {
                continue;
            }
            const int ni = sectindex( w.nextSector() );
            if( ni < 0 || unsigned( ni ) >= sector.Size() )
            {
                continue;
            }
            const auto& p2 = w.point2Wall()->pos;
            const auto  ns = w.nextSector();
            // the middle of the opening, at the height the path arrives at, kept
            // between the floors and ceilings on both sides
            const double zlo  = std::max( sector[ si ].ceilingz, ns->ceilingz );
            const double zhi  = std::min( sector[ si ].floorz, ns->floorz );
            const double fz   = -double( from.Z ) / ONEGAMEUNIT_IN_METERS;
            const double z    = zlo <= zhi ? std::clamp( fz, zlo, zhi ) : ( zlo + zhi ) * 0.5;
            const RgFloat3D m = WorldToRT( ( w.pos.X + p2.X ) * 0.5, ( w.pos.Y + p2.Y ) * 0.5, z );
            const FVector3 mid{ m.data[ 0 ], m.data[ 1 ], m.data[ 2 ] };
            const float    nd = d + ( mid - from ).Length();
            if( nd > reach * 1.5f )
            {
                continue;
            }
            if( s_reachDist[ ni ] < 0.f || nd < s_reachDist[ ni ] )
            {
                s_reachDist[ ni ]  = nd;
                s_reachEntry[ ni ] = mid;
                q.push( { nd, ni } );
            }
        }
    }
    s_reachOn = true;
}

// the copy pass: the lights of the copied sectors (g_rtCopySector, Duke's world
// above the water) uploaded once more, moved like the copied geometry
bool s_copyPass  = false;
int  s_copyGroup = 0;

RgFloat3D CopyMoved( const RgFloat3D& p )
{
    if( !s_copyPass )
    {
        return p;
    }
    const double* ofs = RT_CopyOffset( s_copyGroup );
    return { p.data[ 0 ] + float( ofs[ 0 ] ) * ONEGAMEUNIT_IN_METERS,
             p.data[ 1 ] - float( ofs[ 1 ] ) * ONEGAMEUNIT_IN_METERS,
             p.data[ 2 ] - float( ofs[ 2 ] ) * ONEGAMEUNIT_IN_METERS };
}

uint64_t CopyId( uint64_t id )
{
    return s_copyPass ? id ^ ( 0x8000000000000000ull | ( uint64_t( s_copyGroup ) << 56 ) ) : id;
}

// 0..1: how much of a light at 'p' (RT metres) in sector 'sect' to keep
float ReachFade( const RgFloat3D& p, int sect )
{
    if( s_copyPass )
    {
        // only the copied sectors' lights, by plain distance from the view
        if( RT_CopySector( sect ) != s_copyGroup )
        {
            return 0.f;
        }
        const float     reach = *rt_light_reach > 0.f ? float( *rt_light_reach ) : 1e6f;
        const RgFloat3D q     = CopyMoved( p );
        const RgFloat3D cam   = WorldToRT( g_rtViewPos[ 0 ], g_rtViewPos[ 1 ], g_rtViewPos[ 2 ] );
        const float     d     = ( FVector3{ q.data[ 0 ], q.data[ 1 ], q.data[ 2 ] } -
                                FVector3{ cam.data[ 0 ], cam.data[ 1 ], cam.data[ 2 ] } )
                                .Length();
        const float t = std::clamp( ( reach - d ) / ( reach * 0.33f ), 0.f, 1.f );
        return t * t * ( 3.f - 2.f * t );
    }
    // a real sector left out for a room-over-room view drawn where it is
    if( RT_SectorHidden( sect ) )
    {
        return 0.f;
    }
    if( !s_reachOn )
    {
        return 1.f;
    }
    const float    reach = *rt_light_reach;
    const FVector3 pp{ p.data[ 0 ], p.data[ 1 ], p.data[ 2 ] };
    float          d;
    if( sect >= 0 && size_t( sect ) < s_reachDist.size() )
    {
        if( s_reachDist[ sect ] < 0.f )
        {
            return 0.f;
        }
        d = s_reachDist[ sect ] + ( pp - s_reachEntry[ sect ] ).Length();
    }
    else
    {
        const RgFloat3D cam = WorldToRT( g_rtViewPos[ 0 ], g_rtViewPos[ 1 ], g_rtViewPos[ 2 ] );
        d = ( pp - FVector3{ cam.data[ 0 ], cam.data[ 1 ], cam.data[ 2 ] } ).Length();
    }
    // fade out over the last third, so nothing pops
    const float t = std::clamp( ( reach - d ) / ( reach * 0.33f ), 0.f, 1.f );
    return t * t * ( 3.f - 2.f * t );
}

// the sector a fixed map position is in, found once per level
int CachedSector( std::vector< int >& cache, unsigned index, double x, double y )
{
    if( cache.size() <= index )
    {
        cache.resize( index + 1, -2 );
    }
    if( cache[ index ] == -2 )
    {
        sectortype* s = nullptr;
        updatesector( DVector2( x, y ), &s );
        cache[ index ] = s ? sectindex( s ) : -1;
    }
    return cache[ index ];
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
        .position  = CopyMoved( pos ),
        .radius    = sourceRadiusM,
    };
    auto info = RgLightInfo{
        .sType        = RG_STRUCTURE_TYPE_LIGHT_INFO,
        .pNext        = &sph,
        .uniqueID     = CopyId( id ),
        .isExportable = false,
    };
    RG_CHECK( RT_UploadLight( &info ) );
}

// Sky portals: an opening from a room into the open air (a window, a doorway, the edge of
// a porch -- a wall between a sector with a roof and one with the sky above it) is a
// soft light facing into the room, with the sky's colour and as bright as the sky seen
// through it. The sky lit rooms before only by rays that bounced out through the opening
// and happened to find it -- dim and grainy, the smaller the window the more so; this
// way the path tracer aims at the windows
// how much light, of what colour, comes through a window's picture (a masked wall in the
// opening): through its see-through texels all of it; through stained glass its
// colours, as the path tracer filters a view through it (darker texels -- the
// leading -- less); through the solid texels of bars and grates none
std::array< float, 3 > WindowTint( FTextureID texid )
{
    static std::unordered_map< int, std::array< float, 3 > > cache;
    static uint32_t                                          gen = ~0u;
    if( gen != RT_LevelGeneration() )
    {
        cache.clear();
        gen = RT_LevelGeneration();
    }
    if( auto it = cache.find( texid.GetIndex() ); it != cache.end() )
    {
        return it->second;
    }
    std::array< float, 3 > t = { 1.f, 1.f, 1.f };
    auto gt = TexMan.GetGameTexture( texid );
    if( gt && gt->GetTexture() )
    {
        const bool glass = RT_TableGlass( legacyTileNum( texid ) );
        FBitmap    bm    = gt->GetTexture()->GetBgraBitmap( nullptr );
        const uint8_t* px = bm.GetPixels();
        const int      n  = bm.GetWidth() * bm.GetHeight();
        if( px && n > 0 )
        {
            double sum[ 3 ] = {};
            for( int i = 0; i < n; i++ )
            {
                const uint8_t* c = &px[ size_t( i ) * 4 ];   // BGRA
                if( c[ 3 ] < 128 )
                {
                    sum[ 0 ] += 1; sum[ 1 ] += 1; sum[ 2 ] += 1;
                    continue;
                }
                if( !glass )
                {
                    continue;
                }
                const double r = std::pow( c[ 2 ] / 255., 2.2 ), g = std::pow( c[ 1 ] / 255., 2.2 ), b = std::pow( c[ 0 ] / 255., 2.2 );
                const double m = std::max( { r, g, b } );
                const double hue[ 3 ] = { r / std::max( m, 0.002 ), g / std::max( m, 0.002 ), b / std::max( m, 0.002 ) };
                const double s = std::clamp( ( m - 0.015 ) / ( 0.22 - 0.015 ), 0., 1. );
                const double through = s * s * ( 3 - 2 * s );
                for( int k = 0; k < 3; k++ ) sum[ k ] += ( 0.1 + 0.9 * hue[ k ] ) * through;
            }
            for( int k = 0; k < 3; k++ ) t[ k ] = float( sum[ k ] / n );
        }
    }
    cache[ texid.GetIndex() ] = t;
    return t;
}

// a shut door in the way of a sky opening: a solid sector (a door's panel -- Blood's
// rotating and sliding doors are a closed sector turning or sliding in the doorway) next
// to the room or to the sky's sector, one of whose walls crosses the way from 'from' to 'to'
static bool SkyPortalBlocked( const sectortype& room, const sectortype& sky, const DVector2& from, const DVector2& to )
{
    auto crosses = []( const DVector2& a, const DVector2& b, const DVector2& c, const DVector2& d ) {
        auto side = []( const DVector2& p, const DVector2& q, const DVector2& r ) {
            return ( q.X - p.X ) * ( r.Y - p.Y ) - ( q.Y - p.Y ) * ( r.X - p.X );
        };
        const double d1 = side( c, d, a ), d2 = side( c, d, b ), d3 = side( a, b, c ), d4 = side( a, b, d );
        return ( ( d1 > 0 ) != ( d2 > 0 ) ) && ( ( d3 > 0 ) != ( d4 > 0 ) );
    };
    for( const sectortype* side : { &room, &sky } )
    {
        for( auto& w : side->walls )
        {
            const sectortype* t = w.nextSector();
            if( !t || t == &room || t == &sky || t->floorz - t->ceilingz > 4 )
            {
                continue;
            }
            for( auto& tw : t->walls )
            {
                if( crosses( from, to, tw.pos, tw.point2Wall()->pos ) )
                {
                    return true;
                }
            }
        }
    }
    return false;
}

void UploadSkyPortals()
{
    if( !s_copyPass )
    {
        g_rtSkyPortalStats = {};
    }
    if( *rt_sky_portals <= 0.f || !g_rtSkyLight.valid || s_copyPass )
    {
        return;
    }
    const float* a   = g_rtSkyLight.average;
    const float  lum = 0.2126f * a[ 0 ] + 0.7152f * a[ 1 ] + 0.0722f * a[ 2 ];
    const float  mx  = std::max( { a[ 0 ], a[ 1 ], a[ 2 ] } );
    if( lum <= 1e-5f || mx <= 0.f )
    {
        return;
    }
    const float             c[ 3 ]   = { a[ 0 ] / mx, a[ 1 ] / mx, a[ 2 ] / mx };
    const float             radiance = lum * std::max( 0.f, float( *rt_sky ) );
    // the light outside, on the ground: the sky's (half of it seen through an opening)
    // and the sky's moon (rt_skysun) -- a window lets in what lights the yard
    const float outside = 3.14159265f * radiance * 0.5f +
                          std::max( 0.f, float( *rt_skysun ) ) * g_rtSkyLight.irradiance * std::max( 0.f, float( *rt_sky ) );
    const RgFloat3D cam = WorldToRT( g_rtViewPos[ 0 ], g_rtViewPos[ 1 ], g_rtViewPos[ 2 ] );
    constexpr double        Piece    = 80.;   // 2.5 m: a wide opening is several lights
    for( unsigned si = 0; si < sector.Size(); si++ )
    {
        auto& s = sector[ si ];
        if( s.ceilingstat & CSTAT_SECTOR_SKY )
        {
            continue;
        }
        for( auto& w : s.walls )
        {
            if( !w.twoSided() )
            {
                continue;
            }
            auto o = w.nextSector();
            if( !o || !( o->ceilingstat & CSTAT_SECTOR_SKY ) )
            {
                continue;
            }
            // from the lower ceiling down to the higher floor (an open sky's 'ceiling' is high
            // and closes nothing -- but a door under the sky comes down to its floor: shut,
            // its light glowed on in the room, red on the door's inside)
            const double top = std::max( s.ceilingz, o->ceilingz ), bottom = std::min( s.floorz, o->floorz );
            const double h   = bottom - top;
            const DVector2 p1 = w.pos, p2 = w.point2Wall()->pos;
            const double   width = ( p2 - p1 ).Length();
            if( h < 8 || width < 8 )
            {
                continue;
            }
            DVector2 n = DVector2( -( p2 - p1 ).Y, ( p2 - p1 ).X ).Unit();
            const DVector2 mid = ( p1 + p2 ) * 0.5;
            if( inside( mid.X + n.X * 2, mid.Y + n.Y * 2, &s ) != 1 )
            {
                n = -n;
            }
            // a window's picture in it: its colour (stained glass), less through bars
            std::array< float, 3 > tint = { 1.f, 1.f, 1.f };
            {
                const walltype* mw = ( w.cstat & CSTAT_WALL_MASKED ) ? &w : nullptr;
                if( !mw && w.nextWall() && ( w.nextWall()->cstat & CSTAT_WALL_MASKED ) ) mw = w.nextWall();
                if( mw && mw->overtexture.isValid() ) tint = WindowTint( mw->overtexture );
            }
            const float tmax = std::max( { tint[ 0 ], tint[ 1 ], tint[ 2 ] } );
            if( tmax <= 0.01f )
            {
                continue;
            }
            const float             lc[ 3 ] = { c[ 0 ] * tint[ 0 ], c[ 1 ] * tint[ 1 ], c[ 2 ] * tint[ 2 ] };
            const float             lm      = std::max( { lc[ 0 ], lc[ 1 ], lc[ 2 ], 1e-4f } );
            const float             ln[ 3 ] = { lc[ 0 ] / lm, lc[ 1 ] / lm, lc[ 2 ] / lm };
            const RgColor4DPacked32 wcolor  = PackColor( ln );
            // (the light's strength: the window's share of the sky's, by the brightness of its tint)
            const float through = 0.2126f * lc[ 0 ] + 0.7152f * lc[ 1 ] + 0.0722f * lc[ 2 ];
            const float through0 = std::max( 1e-4f, 0.2126f * c[ 0 ] + 0.7152f * c[ 1 ] + 0.0722f * c[ 2 ] );
            // (a recess's inner face: a wall of it, into another roofed room, parallel to the opening)
            const walltype* inner = nullptr;
            for( auto& w2 : s.walls )
            {
                auto r = w2.nextSector();
                if( &w2 == &w || !r || ( r->ceilingstat & CSTAT_SECTOR_SKY ) ) continue;
                const DVector2 d2 = ( w2.point2Wall()->pos - w2.pos ).Unit();
                const DVector2 d1 = ( p2 - p1 ).Unit();
                if( std::abs( d1.X * d2.X + d1.Y * d2.Y ) < 0.95 ) continue;
                const DVector2 m2 = ( w2.pos + w2.point2Wall()->pos ) * 0.5;
                const double   depth = ( m2 - mid ) | n;
                if( depth > 2. && depth < 64. ) { inner = &w2; break; }   // (up to 2 m deep)
            }
            const int    pieces = std::clamp( int( width / Piece ) + 1, 1, 8 );
            const double pw     = width / pieces;
            for( int k = 0; k < pieces; k++ )
            {
                // (a little way into the room: right at the opening, it lit the wall round it)
                DVector2 pc = p1 + ( p2 - p1 ) * ( ( k + 0.5 ) / pieces ) + n * 12.;
                // a window in a recess (a thin roofed sector, the glass on its outer face): the
                // light at its inner face, in the room -- in the recess it lit mostly the recess
                if( inner )
                {
                    const DVector2 q1 = inner->pos, q2 = inner->point2Wall()->pos, qd = q2 - q1;
                    const double   ql = qd.LengthSquared();
                    if( ql > 1e-6 )
                    {
                        const double t = std::clamp( ( ( pc - q1 ) | qd ) / ql, 0., 1. );
                        pc = q1 + qd * t + n * 12.;
                    }
                }
                // (shut: its door in the way, from the light out to a metre and a half outside)
                {
                    const DVector2 onOpening = p1 + ( p2 - p1 ) * ( ( k + 0.5 ) / pieces );
                    if( SkyPortalBlocked( s, *o, pc, onOpening - n * 48. ) )
                    {
                        continue;
                    }
                }
                const RgFloat3D pos = WorldToRT( pc.X, pc.Y, ( top + bottom ) * 0.5 );
                const float     fade = ReachFade( pos, int( si ) );
                if( fade <= 0.f )
                {
                    continue;
                }
                // the light outside through its area (rt_sky_portals: more, Blood's skies are dark)
                const float area   = float( pw * h ) * ONEGAMEUNIT_IN_METERS * ONEGAMEUNIT_IN_METERS;
                const float lumens = *rt_sky_portals * outside * area * fade * ( through / through0 );
                if( lumens <= 0.05f )
                {
                    continue;
                }
                const float size = float( std::min( pw, h ) ) * ONEGAMEUNIT_IN_METERS;
                const int   dbg  = *rt_sky_portal_debug;
                static const float magenta[ 3 ] = { 1.f, 0.f, 1.f };
                // a small ball (RTGL1's spot lights lit almost nothing here -- a ball of the
                // same strength lit the room: rt_sky_portal_debug 1 and 2 showed it)
                UploadSphere( SkyPortalBase + ( uint64_t( wallindex( &w ) ) << 3 ) + uint64_t( k ), pos,
                              dbg ? PackColor( magenta ) : wcolor, dbg ? 5000.f : lumens, std::clamp( size * 0.1f, 0.05f, 0.2f ) );
                g_rtSkyPortalStats.count++;
                g_rtSkyPortalStats.maxLumens = std::max( g_rtSkyPortalStats.maxLumens, lumens );
                const float dd = std::sqrt( ( pos.data[ 0 ] - cam.data[ 0 ] ) * ( pos.data[ 0 ] - cam.data[ 0 ] ) +
                                            ( pos.data[ 1 ] - cam.data[ 1 ] ) * ( pos.data[ 1 ] - cam.data[ 1 ] ) +
                                            ( pos.data[ 2 ] - cam.data[ 2 ] ) * ( pos.data[ 2 ] - cam.data[ 2 ] ) );
                g_rtSkyPortalStats.nearest = std::min( g_rtSkyPortalStats.nearest, dd );
            }
        }
    }
}

float FlickerScale( uint64_t key, uint32_t frames, uint32_t time )
{
    if( frames <= 1 )
    {
        return 1.f;
    }
    // a new random level every 'frames' tics -- reached smoothly: a level that jumps
    // throws away the denoiser's history every time (crawling splotches)
    const uint32_t k = time / frames;
    float          t = float( time % frames ) / float( frames );
    t                = t * t * ( 3.f - 2.f * t );
    const float a    = Rand01( key * 31 + k );
    const float b    = Rand01( key * 31 + k + 1 );
    const float r    = a + ( b - a ) * t;
    const float amp  = 0.45f * std::clamp( float( *rt_flicker_amount ), 0.f, 1.f );
    return 1.f - amp + amp * r;
}

//
// sector lights
//

std::vector< float > g_levelSmoothed;
uint32_t             g_levelGen = ~0u;

// the colour a sector palette (Duke's pal 1 blue, 2 red, ...) gives the light
FVector3 PaletteTint( int pal )
{
    static std::array< FVector3, MAXPALOOKUPS > s_tint;
    static std::array< bool, MAXPALOOKUPS >     s_done{};
    static uint32_t                             s_gen = ~0u;
    if( s_gen != RT_LevelGeneration() )
    {
        s_done.fill( false );
        s_gen = RT_LevelGeneration();
    }
    if( pal <= 0 || pal >= MAXPALOOKUPS || !lookups.checkTable( pal ) )
    {
        return { 1, 1, 1 };
    }
    if( !s_done[ pal ] )
    {
        const uint8_t* t = lookups.getTable( pal );
        double         orig[ 3 ] = {}, mapped[ 3 ] = {};
        for( int i = 0; i < 255; i++ )
        {
            const PalEntry a = GPalette.BaseColors[ i ];
            const PalEntry b = GPalette.BaseColors[ t[ i ] ];
            orig[ 0 ] += a.r, orig[ 1 ] += a.g, orig[ 2 ] += a.b;
            mapped[ 0 ] += b.r, mapped[ 1 ] += b.g, mapped[ 2 ] += b.b;
        }
        FVector3 c{ float( mapped[ 0 ] / std::max( orig[ 0 ], 1.0 ) ),
                    float( mapped[ 1 ] / std::max( orig[ 1 ], 1.0 ) ),
                    float( mapped[ 2 ] / std::max( orig[ 2 ], 1.0 ) ) };
        const float mx = std::max( { c.X, c.Y, c.Z, 1e-3f } );
        c /= mx;
        // a palette that only darkens or is near grey stays white
        if( std::min( { c.X, c.Y, c.Z } ) > 0.8f )
        {
            c = { 1, 1, 1 };
        }
        s_tint[ pal ] = c;
        s_done[ pal ] = true;
    }
    return s_tint[ pal ];
}

// LIGHTOVR "signal sector" response: a switch's state, or 1 without one
float SignalResponse( bool enabled, int signalSector )
{
    if( !enabled || signalSector < 0 )
    {
        return 1.f;
    }
    const float sw = RT_SectorSwitch( signalSector );
    return sw < 0.f ? 1.f : 0.03f + 0.97f * sw;
}

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

    // every sector's light level, followed softly (for the surfaces' faint glow)
    if( g_levelSmoothed.size() != sector.Size() || g_levelGen != RT_LevelGeneration() )
    {
        g_levelSmoothed.assign( sector.Size(), -1.f );
        g_levelGen = RT_LevelGeneration();
    }
    for( unsigned i = 0; i < sector.Size() && !s_copyPass; i++ )
    {
        const int   sh  = ( int( sector[ i ].ceilingshade ) + int( sector[ i ].floorshade ) ) / 2;
        const float lv  = std::clamp( 1.f - float( sh ) / ns, 0.f, 1.f );
        float&      sm  = g_levelSmoothed[ i ];
        sm              = sm < 0.f ? lv : sm + ( lv - sm ) * std::clamp( float( *rt_sector_light_smooth ), 0.01f, 1.f );
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
        if( !s_copyPass || sm < 0.f )
        {
            sm = sm < 0.f ? lumens : sm + ( lumens - sm ) * std::clamp( float( *rt_sector_light_smooth ), 0.01f, 1.f );
        }
        lumens    = sm;

        lumens *= ReachFade( WorldToRT( cx, cy, z ), int( i ) );
        if( lumens <= 0.01f )
        {
            continue;
        }

        // the sector's palette tints its light (the club's red and blue flashes)
        const int   pal  = sec.floorpal ? sec.floorpal : sec.ceilingpal;
        const auto  tint = *rt_sector_light_tint ? PaletteTint( pal ) : FVector3{ 1, 1, 1 };
        UploadSphere( SectorLightBase + i, WorldToRT( cx, cy, z ),
                      rt.rgUtilPackColorFloat4D( tint.X, tint.Y, tint.Z, 1.f ), lumens, 0.25f );
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

    static std::vector< int > s_mapLightSector, s_surfLightSector;
    static uint32_t           s_cacheGen = ~0u;
    if( s_cacheGen != RT_LevelGeneration() )
    {
        s_mapLightSector.clear();
        s_surfLightSector.clear();
        s_cacheGen = RT_LevelGeneration();
    }

    // games without Duke-RT's lights (Blood): a faint moon, so the outdoors has a
    // light with a direction -- shadows, and the bump maps show (the sky's own light
    // comes from everywhere: flat)
    // (a lightning flash is this light too, many times brighter: RTGL1 has one sun)
    const float flash = std::clamp( g_rtSkyFlash, 0.f, 1.f ) * std::max( 0.f, float( *rt_lightning ) );
    // the sky's own light (any game, unless the map has its own sun): from where the sky
    // in view is brightest -- Blood's moon, a glowing stretch of clouds -- in its colour
    // (a red sky's is red), as strong as the sky is bright; a level's own (it keeps it
    // indoors, where the sky is not drawn)
    static uint32_t s_skyGen = ~0u;
    if( s_skyGen != RT_LevelGeneration() )
    {
        s_skyGen            = RT_LevelGeneration();
        g_rtSkyLight.valid  = false;
    }
    if( set.directionalRules.Size() == 0 && !s_copyPass && *rt_skysun > 0.f && g_rtSkyLight.valid &&
        g_rtSkyLight.irradiance > 0.f )
    {
        const float sun = *rt_skysun * g_rtSkyLight.irradiance * std::max( 0.f, float( *rt_sky ) );
        const float mf  = flash / ( flash + sun + 1e-3f );
        const float flashc[ 3 ] = { 0.92f, 0.95f, 1.f };
        float c[ 3 ];
        const float tint = std::clamp( float( *rt_skysun_tint ), 0.f, 1.f );
        for( int i = 0; i < 3; i++ )
        {
            // (lighter than the sky: the lit side stands apart from the sky-lit shadows)
            const float sc = 1.f - tint + tint * g_rtSkyLight.color[ i ];
            c[ i ] = sc * ( 1.f - mf ) + flashc[ i ] * mf;
        }
        const float* d = g_rtSkyLight.dir;
        auto dir = RgLightDirectionalEXT{
            .sType                  = RG_STRUCTURE_TYPE_LIGHT_DIRECTIONAL_EXT,
            .pNext                  = nullptr,
            .color                  = PackColor( c ),
            .intensity              = sun + flash,
            .direction              = { -d[ 0 ], -d[ 1 ], -d[ 2 ] },
            .angularDiameterDegrees = std::clamp( float( *rt_skysun_size ), 0.1f, 10.f ),
        };
        auto info = RgLightInfo{
            .sType        = RG_STRUCTURE_TYPE_LIGHT_INFO,
            .pNext        = &dir,
            .uniqueID     = MapLightBase - 2,
            .isExportable = false,
        };
        RG_CHECK( RT_UploadLight( &info ) );
    }
    else if( set.directionalRules.Size() == 0 && set.actorRules.Size() == 0 && !s_copyPass && *rt_moon + flash > 0.f )
    {
        const float  az = float( *rt_moon_angle ) * 3.14159265f / 180.f, el = 50.f * 3.14159265f / 180.f;
        const RgFloat3D toLight = { std::cos( el ) * std::cos( az ), std::cos( el ) * std::sin( az ), std::sin( el ) };
        const float  mf     = flash / ( flash + std::max( 0.f, float( *rt_moon ) ) + 1e-3f );
        const float  c[ 3 ] = { 0.72f + 0.2f * mf, 0.78f + 0.17f * mf, 1.0f };
        auto dir = RgLightDirectionalEXT{
            .sType                  = RG_STRUCTURE_TYPE_LIGHT_DIRECTIONAL_EXT,
            .pNext                  = nullptr,
            .color                  = PackColor( c ),
            .intensity              = std::max( 0.f, float( *rt_moon ) ) + flash,
            .direction              = { -toLight.data[ 0 ], -toLight.data[ 1 ], -toLight.data[ 2 ] },
            .angularDiameterDegrees = 1.0f,
        };
        auto info = RgLightInfo{
            .sType        = RG_STRUCTURE_TYPE_LIGHT_INFO,
            .pNext        = &dir,
            .uniqueID     = MapLightBase - 2,
            .isExportable = false,
        };
        RG_CHECK( RT_UploadLight( &info ) );
    }

    // the map's directional light: RTGL1 allows one
    if( set.directionalRules.Size() > 0 && !s_copyPass )
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
            RG_CHECK( RT_UploadLight( &info ) );
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

        const uint64_t  id     = MapLightBase + i;
        const RgFloat3D lp     = WorldToRT( x, y, z );
        const int       lsec   = r.anchorType == LightOverlayAnchorType::Sector ? r.anchorIndex
                                                                                 : CachedSector( s_mapLightSector, i, x, y );
        const float     lumens = OverlayToLumens( r.intensity, r.radius ) * FlickerScale( id, r.flickerFrames, time ) *
                             ReachFade( lp, lsec );
        UploadSphere( id, lp, PackColor( r.color ), lumens, 0.05f );
    }

    // surface lights: a light just off an authored spot on a surface
    for( unsigned i = 0; i < set.surfaceLightRules.Size(); i++ )
    {
        const auto& r = set.surfaceLightRules[ i ];
        if( !r.hasPosition || !r.hasNormal || r.intensity <= 0.f || r.radius <= 0.f )
        {
            continue;
        }
        const float offset   = r.hasOffset ? r.offset : 0.5f;
        const float response = SignalResponse( r.sectorResponse, r.hasSignalSector ? r.signalSector : -1 );
        const float sw       = r.hasSize ? r.size[ 0 ] : 32.f;
        const float sh       = r.hasSize ? r.size[ 1 ] : 32.f;

        // the light: a little in front of its fixture, small enough to stay off the surface
        const float lightOut = offset + 4.f;
        const float p[ 3 ] = {
            r.position[ 0 ] + r.normal[ 0 ] * lightOut,
            r.position[ 1 ] + r.normal[ 1 ] * lightOut,
            r.position[ 2 ] + r.normal[ 2 ] * lightOut,
        };
        // path tracing space: x, -z, -y of the world
        const float fade = ReachFade( PTToRT( p ), CachedSector( s_surfLightSector, i, p[ 0 ], -p[ 2 ] ) );
        UploadSphere( SurfaceLightBase + i, PTToRT( p ), PackColor( r.color ),
                      OverlayToLumens( r.intensity, r.radius ) * response * fade,
                      std::clamp( std::min( sw, sh ) * 0.5f * ONEGAMEUNIT_IN_METERS, 0.02f, 0.1f ) );

        // the fixture: the author's lamp panel, drawn on the surface
        if( *rt_lightovr_fixtures )
        {
            const RgFloat3D c  = PTToRT( r.position );
            const float     pn[ 3 ] = { r.normal[ 0 ], r.normal[ 1 ], r.normal[ 2 ] };
            const RgFloat3D nn = PTToRT( pn );
            FVector3        n{ nn.data[ 0 ], nn.data[ 1 ], nn.data[ 2 ] };
            if( n.LengthSquared() < 1e-8f )
            {
                continue;
            }
            n.MakeUnit(); // PTToRT scaled it; only the direction is used
            FVector3 u, v;
            if( std::abs( n.Z ) > 0.9f )
            {
                u = { 1, 0, 0 };
            }
            else
            {
                u = FVector3{ 0, 0, 1 } ^ n;
            }
            u.MakeUnit();
            v = n ^ u;
            if( r.hasRotation && r.rotation != 0.f )
            {
                const float a  = r.rotation * float( 3.14159265358979 / 180.0 );
                const FVector3 ru = u * std::cos( a ) + v * std::sin( a );
                const FVector3 rv = v * std::cos( a ) - u * std::sin( a );
                u = ru, v = rv;
            }
            const float    hw = sw * 0.5f * ONEGAMEUNIT_IN_METERS, hh = sh * 0.5f * ONEGAMEUNIT_IN_METERS;
            const FVector3 ctr = FVector3{ c.data[ 0 ], c.data[ 1 ], c.data[ 2 ] } + n * ( offset * ONEGAMEUNIT_IN_METERS );
            const FVector3 q[ 4 ] = { ctr - u * hw - v * hh, ctr + u * hw - v * hh, ctr + u * hw + v * hh, ctr - u * hw + v * hh };
            float corners[ 4 ][ 3 ];
            for( int k = 0; k < 4; k++ )
            {
                corners[ k ][ 0 ] = q[ k ].X, corners[ k ][ 1 ] = q[ k ].Y, corners[ k ][ 2 ] = q[ k ].Z;
            }
            const float nrm[ 3 ] = { n.X, n.Y, n.Z };
            FString     tex      = r.hasFixtureTexture && r.fixtureTexture.IsNotEmpty() ? r.fixtureTexture : FString( "#00707" );
            if( s_copyPass )
            {
                if( fade <= 0.f )
                {
                    continue;
                }
                for( int k = 0; k < 4; k++ )
                {
                    const RgFloat3D m = CopyMoved( { corners[ k ][ 0 ], corners[ k ][ 1 ], corners[ k ][ 2 ] } );
                    corners[ k ][ 0 ] = m.data[ 0 ], corners[ k ][ 1 ] = m.data[ 1 ], corners[ k ][ 2 ] = m.data[ 2 ];
                }
            }
            RT_UploadFixtureQuad( CopyId( SurfaceLightBase + i ), corners, nrm, tex.GetChars(), response );
        }
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
            RgFloat3D       pos  = { base.data[ 0 ] + off.data[ 0 ], base.data[ 1 ] + off.data[ 1 ], base.data[ 2 ] + off.data[ 2 ] };
            // ...in the actor's room: an offset made for a big fire (Duke-RT's DukeFire is a
            // dumpster's, 48 up) put the light of a small one in a low alcove above its
            // ceiling -- the alcove dark, a bright spot on the wall over it
            if( auto sec = actor->sector() )
            {
                const double cz = getceilzofslopeptr( sec, actor->spr.pos.X, actor->spr.pos.Y );
                const double fz = getflorzofslopeptr( sec, actor->spr.pos.X, actor->spr.pos.Y );
                const float  ct = float( -cz ) * ONEGAMEUNIT_IN_METERS - 0.08f;
                const float  fb = float( -fz ) * ONEGAMEUNIT_IN_METERS + 0.05f;
                if( ct > fb )
                {
                    pos.data[ 2 ] = std::clamp( pos.data[ 2 ], fb, ct );
                }
            }

            float intensity = rule.intensity;
            if( rule.hasRandom )
            {
                // (a fire's): a new random level every few tics, reached smoothly, around the
                // middle of its range and as deep as rt_flicker_amount. A new level every tic
                // was a strobe the denoiser could not follow: the light near fires boiled.
                constexpr uint32_t period = 6;
                const uint32_t     k      = time / period;
                float              t      = float( time % period ) / float( period );
                t                         = t * t * ( 3.f - 2.f * t );
                const float a   = Rand01( id * 7919 + k ), b = Rand01( id * 7919 + k + 1 );
                const float r   = a + ( b - a ) * t;
                const float lo  = rule.randomIntensityRange[ 0 ], hi = rule.randomIntensityRange[ 1 ];
                const float mid = 0.5f * ( lo + hi );
                intensity += mid + ( lo + r * ( hi - lo ) - mid ) * std::clamp( float( *rt_flicker_amount ), 0.f, 1.f );
            }
            intensity *= FlickerScale( id, rule.flickerFrames, time );

            const float fade = ReachFade( pos, actor->sector() ? sectindex( actor->sector() ) : -1 );
            UploadSphere( id, pos, PackColor( rule.color ), OverlayToLumens( intensity, rule.radius ) * fade, 0.05f );
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

float RT_SectorFlickerSmoothing( int sectnum )
{
    if( sectnum < 0 || size_t( sectnum ) >= g_levelSmoothed.size() || unsigned( sectnum ) >= sector.Size() ||
        g_levelSmoothed[ size_t( sectnum ) ] < 0.f )
    {
        return 1.f;
    }
    const float ns  = float( std::max( numshades, 2 ) );
    const int   sh  = ( int( sector[ sectnum ].ceilingshade ) + int( sector[ sectnum ].floorshade ) ) / 2;
    const float lv  = std::clamp( 1.f - float( sh ) / ns, 0.02f, 1.f );
    return std::clamp( g_levelSmoothed[ size_t( sectnum ) ] / lv, 0.25f, 4.f );
}

bool RT_PointInSector( int sectnum, float x, float y )
{
    if( sectnum < 0 || unsigned( sectnum ) >= sector.Size() )
    {
        return true;
    }
    double wx = double( x ) / ONEGAMEUNIT_IN_METERS, wy = double( -y ) / ONEGAMEUNIT_IN_METERS;
    // a copy is somewhere else than its sector
    if( rtstate.is< RtPrim::CopyMove >() || rtstate.is< RtPrim::CopyId >() )
    {
        const double* ofs = rtstate.get_copyoffset();
        wx -= ofs[ 0 ];
        wy -= ofs[ 1 ];
    }
    return inside( wx, wy, &sector[ sectnum ] ) == 1;
}

float RT_CeilingRecessDepth( int sectnum )
{
    if( sectnum < 0 || unsigned( sectnum ) >= sector.Size() )
    {
        return 0.f;
    }
    const sectortype& sec  = sector[ sectnum ];
    double            drop = -1;
    for( auto& wal : sec.walls )
    {
        if( !wal.twoSided() )
        {
            continue;
        }
        const double d = wal.nextSector()->ceilingz - sec.ceilingz; // (z down: lower is more)
        if( d > 0.5 && ( drop < 0 || d < drop ) )
        {
            drop = d;
        }
    }
    return drop > 0 ? std::min( float( drop ) * ONEGAMEUNIT_IN_METERS, 2.f ) : 0.f;
}

float RT_EmissiveOverride( int sectnum, int tile, float& glow )
{
    const ResolvedLightOverlaySet& set = GetResolvedLightOverlaySet();
    float scale = 1.f;
    for( unsigned i = 0; i < set.emissiveOverrideRules.Size(); i++ )
    {
        const auto& r = set.emissiveOverrideRules[ i ];
        if( !r.hasSectorFilter && !r.hasTileFilter )
        {
            continue;
        }
        if( ( r.hasSectorFilter && r.sectorFilter != sectnum ) || ( r.hasTileFilter && r.tileFilter != tile ) )
        {
            continue;
        }
        scale = std::clamp( r.intensityScale * r.reachScale, 0.f, 8.f );
        if( r.sectorResponse && r.hasSignalSector )
        {
            const float sw = RT_SectorSwitch( r.signalSector );
            if( sw >= 0.f )
            {
                glow = sw;
            }
        }
        break;
    }
    return scale;
}

// Rooms over rooms (overlapping sectors): a ceiling at exactly the height of the
// floor of a sector over it. Rays hit both at random there; those two planes are
// moved apart (rt_flat_offset). Found once per level: per sector, bit 0 = its floor,
// bit 1 = its ceiling.
static std::vector< uint8_t > s_flatTouch;
static uint32_t               s_flatTouchGen = ~0u;

static void FindTouchingFlats()
{
    const unsigned n = sector.Size();
    s_flatTouch.assign( n, 0 );
    std::vector< DVector4 > bb( n ); // min x, min y, max x, max y
    std::vector< std::vector< DVector2 > > pts( n );
    for( unsigned i = 0; i < n; i++ )
    {
        const auto& sec = sector[ i ];
        DVector4    b{ 1e30, 1e30, -1e30, -1e30 };
        DVector2    c{ 0, 0 };
        for( auto& w : sec.walls )
        {
            b.X = std::min( b.X, w.pos.X ), b.Y = std::min( b.Y, w.pos.Y );
            b.Z = std::max( b.Z, w.pos.X ), b.W = std::max( b.W, w.pos.Y );
            c += w.pos;
        }
        bb[ i ] = b;
        if( sec.walls.Size() == 0 )
        {
            continue;
        }
        c /= double( sec.walls.Size() );
        // points inside the sector: its middle and its corners pulled in a little
        if( inside( c.X, c.Y, &sec ) == 1 )
        {
            pts[ i ].push_back( c );
        }
        for( auto& w : sec.walls )
        {
            for( double t : { 0.1, 0.4 } )
            {
                const DVector2 p = w.pos * ( 1.0 - t ) + c * t;
                if( inside( p.X, p.Y, &sec ) == 1 )
                {
                    pts[ i ].push_back( p );
                }
            }
        }
    }
    auto overlap = [ & ]( unsigned a, unsigned b ) {
        for( auto& p : pts[ a ] )
        {
            if( inside( p.X, p.Y, &sector[ b ] ) == 1 )
            {
                return true;
            }
        }
        for( auto& p : pts[ b ] )
        {
            if( inside( p.X, p.Y, &sector[ a ] ) == 1 )
            {
                return true;
            }
        }
        return false;
    };
    for( unsigned a = 0; a < n; a++ )
    {
        const auto& sa = sector[ a ];
        for( unsigned b = a + 1; b < n; b++ )
        {
            if( bb[ a ].X >= bb[ b ].Z || bb[ b ].X >= bb[ a ].Z || bb[ a ].Y >= bb[ b ].W || bb[ b ].Y >= bb[ a ].W )
            {
                continue;
            }
            const auto& sb = sector[ b ];
            // a's ceiling on b's floor, or b's ceiling on a's floor (flat planes)
            const bool flatA = !( sa.ceilingstat & CSTAT_SECTOR_SLOPE ) && !( sa.floorstat & CSTAT_SECTOR_SLOPE );
            const bool flatB = !( sb.ceilingstat & CSTAT_SECTOR_SLOPE ) && !( sb.floorstat & CSTAT_SECTOR_SLOPE );
            if( !flatA || !flatB )
            {
                continue;
            }
            const bool ab = std::abs( sa.ceilingz - sb.floorz ) < 0.01;
            const bool ba = std::abs( sb.ceilingz - sa.floorz ) < 0.01;
            if( ( ab || ba ) && overlap( a, b ) )
            {
                if( ab )
                {
                    s_flatTouch[ a ] |= 2, s_flatTouch[ b ] |= 1;
                }
                if( ba )
                {
                    s_flatTouch[ b ] |= 2, s_flatTouch[ a ] |= 1;
                }
            }
        }
    }
}

bool RT_FlatTouchesOther( int sectnum, bool ceiling )
{
    if( s_flatTouchGen != RT_LevelGeneration() )
    {
        FindTouchingFlats();
        s_flatTouchGen = RT_LevelGeneration();
    }
    return sectnum >= 0 && size_t( sectnum ) < s_flatTouch.size() && ( s_flatTouch[ sectnum ] & ( ceiling ? 2 : 1 ) );
}

bool RT_HaveActorLightRules()
{
    return *rt_lightovr && currentLevel && GetResolvedLightOverlaySet().actorRules.Size() > 0;
}

float RT_LightReachFade( const RgFloat3D& p, int sect )
{
    return ReachFade( p, sect );
}

void RT_UploadSceneLights( uint32_t time )
{
    ComputeReach();
    UploadSectorLights();
    UploadOverlayLights( time );
    UploadSkyPortals();
    // the game's own lights
    for( const RtGameLight& l : g_rtGameLights )
    {
        const RgFloat3D p = WorldToRT( l.pos[ 0 ], l.pos[ 1 ], l.pos[ 2 ] );
        UploadSphere( GameLightBase + ( l.id & 0xFFFFFFFFull ),
                      p,
                      PackColor( l.color ),
                      l.intensity * float( *rt_game_light_scale ) * ReachFade( p, l.sector ),
                      std::clamp( l.radius, 0.02f, 0.5f ) );
    }
    if( g_rtCopySector.size() == sector.Size() )
    {
        uint32_t used = 0;
        for( uint8_t c : g_rtCopySector )
        {
            used |= c >= 1 && c <= RT_MAX_COPIES ? 1u << ( c - 1 ) : 0u;
        }
        s_copyPass = true;
        for( s_copyGroup = 1; s_copyGroup <= RT_MAX_COPIES; s_copyGroup++ )
        {
            if( used & ( 1u << ( s_copyGroup - 1 ) ) )
            {
                UploadSectorLights();
                UploadOverlayLights( time );
            }
        }
        s_copyPass  = false;
        s_copyGroup = 0;
    }
}

// rt_lookat: the openings near the view -- what made a sky portal of each, or not
CCMD( rt_sky_portal_info )
{
    const DVector2 cam( g_rtViewPos[ 0 ], g_rtViewPos[ 1 ] );
    int            shown = 0;
    for( unsigned si = 0; si < sector.Size() && shown < 24; si++ )
    {
        auto& s = sector[ si ];
        for( auto& w : s.walls )
        {
            if( !w.twoSided() || shown >= 24 )
            {
                continue;
            }
            auto o = w.nextSector();
            if( !o )
            {
                continue;
            }
            const DVector2 p1 = w.pos, p2 = w.point2Wall()->pos, mid = ( p1 + p2 ) * 0.5;
            const double   d  = ( mid - cam ).Length() / 32.;
            if( d > 12. )
            {
                continue;
            }
            const bool inSky = ( s.ceilingstat & CSTAT_SECTOR_SKY ) != 0, outSky = ( o->ceilingstat & CSTAT_SECTOR_SKY ) != 0;
            if( inSky == outSky )
            {
                continue;   // (both roofed or both open: no opening to the sky)
            }
            if( inSky )
            {
                continue;   // (listed from the roofed side)
            }
            const double h  = std::min( s.floorz, o->floorz ) - s.ceilingz;
            const double wd = ( p2 - p1 ).Length();
            const walltype* mw = ( w.cstat & CSTAT_WALL_MASKED ) ? &w : ( w.nextWall() && ( w.nextWall()->cstat & CSTAT_WALL_MASKED ) ) ? w.nextWall() : nullptr;
            FString t;
            if( mw && mw->overtexture.isValid() )
            {
                auto tint = WindowTint( mw->overtexture );
                t.Format( ", window tile %d%s, lets through %.2f %.2f %.2f", legacyTileNum( mw->overtexture ),
                          RT_TableGlass( legacyTileNum( mw->overtexture ) ) ? " (glass)" : "", tint[ 0 ], tint[ 1 ], tint[ 2 ] );
            }
            Printf( "    opening: wall %d, sector %u (roofed) to %d (sky), %.1f m away, %.1f m wide, %.1f m high%s%s\n", wallindex( &w ), si,
                    sectindex( o ), d, wd / 32., h / 32., t.GetChars(), h < 8 || wd < 8 ? " -- too small, no light" : "" );
            shown++;
        }
    }
    if( !shown )
    {
        Printf( "    no openings to the sky within 12 m\n" );
    }
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
