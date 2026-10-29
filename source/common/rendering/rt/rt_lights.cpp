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
#include "serializer.h"

#include <unordered_map>
#include <queue>
#include <array>
#include "palette.h"
#include "palettecontainer.h"
#include "bitmap.h"

CVARD( Bool, rt_lightovr, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: lights from Duke-RT's LIGHTOVR" )
CVARD( Float, rt_lightovr_scale, 10.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: brightness of LIGHTOVR point lights" )
CVARD( Float, rt_lightovr_spot_scale, 1.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: brightness of LIGHTOVR spot lights (a cinema projector)" )
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
int RT_SectorWhyCode( int s );	// hw_bunchdrawer.cpp: why a sector was or was not given (5: on screen)
EXTERN_CVAR( Bool, rt_glass_shadows )
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
    // a light in a room on screen: as far as can be seen, straight (a torch across a yard,
    // a long walk round, was out -- its fire burned and nothing round it was lit)
    if( RT_SectorWhyCode( sect ) == 5 )
    {
        const RgFloat3D cam  = WorldToRT( g_rtViewPos[ 0 ], g_rtViewPos[ 1 ], g_rtViewPos[ 2 ] );
        const float     far_ = reach * 2.5f;
        const float     ds   = ( pp - FVector3{ cam.data[ 0 ], cam.data[ 1 ], cam.data[ 2 ] } ).Length();
        const float     t    = std::clamp( ( far_ - ds ) / ( far_ * 0.33f ), 0.f, 1.f );
        return t * t * ( 3.f - 2.f * t );
    }
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

// a spot light (since rtgl1-lib 0012 as bright as asked): a stained glass window's
// light, outside the glass, shining in through it
static void UploadSpotLight( uint64_t id, const RgFloat3D& pos, const RgFloat3D& dir, RgColor4DPacked32 color, float lumens,
                             float outerDeg, float innerDeg )
{
    if( lumens <= 0.01f )
    {
        return;
    }
    auto spot = RgLightSpotEXT{
        .sType      = RG_STRUCTURE_TYPE_LIGHT_SPOT_EXT,
        .pNext      = nullptr,
        .color      = color,
        .intensity  = lumens,
        .position   = CopyMoved( pos ),
        .direction  = dir,
        .radius     = 0.05f,
        .angleOuter = outerDeg * 3.14159265f / 180.f,
        .angleInner = innerDeg * 3.14159265f / 180.f,
    };
    auto info = RgLightInfo{
        .sType        = RG_STRUCTURE_TYPE_LIGHT_INFO,
        .pNext        = &spot,
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
// per column of the picture: what comes through it (summed over the column), so a part
// of a window -- one of a wide window's lights -- gets the colours of its own panes
struct WindowCols
{
    std::vector< std::array< float, 3 > > col;	// per texel column, the mean over its height
    float                                 tileWidth = 1.f;	// the picture's width in tile texels (panning, repeats)
};
static const WindowCols& WindowColumns( FTextureID texid )
{
    static std::unordered_map< int, WindowCols > cache;
    static uint32_t                              gen = ~0u;
    if( gen != RT_LevelGeneration() )
    {
        cache.clear();
        gen = RT_LevelGeneration();
    }
    if( auto it = cache.find( texid.GetIndex() ); it != cache.end() )
    {
        return it->second;
    }
    WindowCols wc;
    auto gt = TexMan.GetGameTexture( texid );
    if( gt && gt->GetTexture() )
    {
        wc.tileWidth     = std::max( 1.f, float( gt->GetDisplayWidth() ) );
        const bool glass = RT_TableGlass( legacyTileNum( texid ) );
        FBitmap    bm    = gt->GetTexture()->GetBgraBitmap( nullptr );
        const uint8_t* px = bm.GetPixels();
        const int      w  = bm.GetWidth(), h = bm.GetHeight();
        if( px && w > 0 && h > 0 )
        {
            wc.col.assign( size_t( w ), { 0.f, 0.f, 0.f } );
            for( int x = 0; x < w; x++ )
            {
                double sum[ 3 ] = {};
                for( int y = 0; y < h; y++ )
                {
                    const uint8_t* c = &px[ ( size_t( y ) * size_t( w ) + size_t( x ) ) * 4 ];   // BGRA
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
                for( int k = 0; k < 3; k++ ) wc.col[ size_t( x ) ][ k ] = float( sum[ k ] / h );
            }
        }
    }
    return cache[ texid.GetIndex() ] = std::move( wc );
}

// through the columns from u0 to u1 (tile texels; wrapping), or the whole picture
static std::array< float, 3 > WindowTintSpan( FTextureID texid, double u0, double u1 )
{
    const WindowCols& wc = WindowColumns( texid );
    std::array< float, 3 > t = { 1.f, 1.f, 1.f };
    const int w = int( wc.col.size() );
    if( w == 0 )
    {
        return t;
    }
    if( u1 < u0 ) std::swap( u0, u1 );
    const double scale = w / double( wc.tileWidth );
    int x0 = int( std::floor( u0 * scale ) ), x1 = int( std::ceil( u1 * scale ) );
    if( x1 - x0 >= w || x1 <= x0 ) { x0 = 0; x1 = w; }
    double sum[ 3 ] = {};
    for( int x = x0; x < x1; x++ )
    {
        const auto& c = wc.col[ size_t( ( ( x % w ) + w ) % w ) ];
        for( int k = 0; k < 3; k++ ) sum[ k ] += c[ k ];
    }
    for( int k = 0; k < 3; k++ ) t[ k ] = float( sum[ k ] / ( x1 - x0 ) );
    return t;
}

std::array< float, 3 > WindowTint( FTextureID texid )
{
    return WindowTintSpan( texid, 0., 1e9 );
}

// a shut door in the way of a sky opening: a sector next to the room, to the sky's sector
// or to the room the light was moved into (a recess's inner side) that is mostly solid
// over the opening's height -- a door's panel (Blood's rotating and sliding doors are a
// sector turning or sliding in the doorway, its floor up near its ceiling; a door that
// comes down shuts its own sector) -- one of whose walls crosses the way from 'from' to 'to'
static bool SkyPortalBlocked( std::initializer_list< const sectortype* > around, double top, double bottom,
                              const DVector2& from, const DVector2& to )
{
    auto crosses = []( const DVector2& a, const DVector2& b, const DVector2& c, const DVector2& d ) {
        auto side = []( const DVector2& p, const DVector2& q, const DVector2& r ) {
            return ( q.X - p.X ) * ( r.Y - p.Y ) - ( q.Y - p.Y ) * ( r.X - p.X );
        };
        const double d1 = side( c, d, a ), d2 = side( c, d, b ), d3 = side( a, b, c ), d4 = side( a, b, d );
        return ( ( d1 > 0 ) != ( d2 > 0 ) ) && ( ( d3 > 0 ) != ( d4 > 0 ) );
    };
    const double h = std::max( 1., bottom - top );
    for( const sectortype* side : around )
    {
        if( !side )
        {
            continue;
        }
        for( auto& w : side->walls )
        {
            const sectortype* t = w.nextSector();
            if( !t || std::find( around.begin(), around.end(), t ) != around.end() )
            {
                continue;
            }
            // (its open part, over the opening's height: under a third of it is shut)
            const double open = std::min( t->floorz, bottom ) - std::max( t->ceilingz, top );
            if( open > h * 0.35 )
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

// open air, for the window lights: a sector with the sky for a ceiling -- or one roofed
// very high (over 12 m) next to one: a street the map closed with a ceiling far up and a
// scrap of sky beside it (Duke E1L1's street in front of the cinema, 54 m up), outdoors
// for anyone in it
static bool OpenAir( const sectortype* s )
{
    if( !s )
    {
        return false;
    }
    if( s->ceilingstat & CSTAT_SECTOR_SKY )
    {
        return true;
    }
    if( s->floorz - s->ceilingz < 12. * 32. )
    {
        return false;
    }
    for( auto& w : s->walls )
    {
        if( auto n = w.nextSector(); n && ( n->ceilingstat & CSTAT_SECTOR_SKY ) )
        {
            return true;
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
        if( OpenAir( &s ) )
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
            if( !o || !OpenAir( o ) )
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
            // (a recess's inner face: a wall of it, into another roofed room, parallel to the
            // opening -- and on through recesses behind it: Blood E4M1's stained glass is the
            // wall between two thin sectors, one in from the opening, the room one further)
            const walltype*              inner = nullptr;
            std::vector< const walltype* > layers;	// the walls the light comes in through
            {
                const sectortype* cur   = &s;
                double            depth0 = 0.;
                const DVector2    d1     = ( p2 - p1 ).Unit();
                for( int level = 0; level < 3 && cur; level++ )
                {
                    // (on only through a recess: a thin sector, not the room itself)
                    if( level > 0 )
                    {
                        double lo = 1e30, hi = -1e30;
                        for( auto& w3 : cur->walls )
                        {
                            const double dd = ( w3.pos - mid ) | n;
                            lo = std::min( lo, dd );
                            hi = std::max( hi, dd );
                        }
                        if( hi - lo > 64. ) break;
                    }
                    const walltype* found = nullptr;
                    double          fd    = 0.;
                    for( auto& w2 : cur->walls )
                    {
                        auto r = w2.nextSector();
                        if( &w2 == &w || !r || r == &s || OpenAir( r ) ) continue;
                        if( inner && w2.nextWall() == inner ) continue;
                        const DVector2 d2 = ( w2.point2Wall()->pos - w2.pos ).Unit();
                        if( std::abs( d1.X * d2.X + d1.Y * d2.Y ) < 0.95 ) continue;
                        const DVector2 m2    = ( w2.pos + w2.point2Wall()->pos ) * 0.5;
                        const double   depth = ( m2 - mid ) | n;
                        if( depth > depth0 + 2. && depth < depth0 + 64. ) { found = &w2; fd = depth; break; }   // (up to 2 m deep)
                    }
                    if( !found ) break;
                    inner  = found;
                    depth0 = fd;
                    layers.push_back( found );
                    cur = found->nextSector();
                }
            }
            // a window's picture in it, or in a recess's wall behind it: its colour (stained
            // glass), less through bars -- per light, the panes in front of it
            std::vector< const walltype* > glassWalls;
            {
                auto add = [ & ]( const walltype& lw ) {
                    const walltype* mw = ( lw.cstat & CSTAT_WALL_MASKED ) ? &lw : nullptr;
                    if( !mw && lw.nextWall() && ( lw.nextWall()->cstat & CSTAT_WALL_MASKED ) ) mw = lw.nextWall();
                    if( mw && mw->overtexture.isValid() ) glassWalls.push_back( mw );
                };
                add( w );
                for( const walltype* lw : layers ) add( *lw );
            }
            // through the part of a picture over the stretch of the opening from a to b
            auto tintOver = [ & ]( const DVector2& a, const DVector2& b ) {
                std::array< float, 3 > t = { 1.f, 1.f, 1.f };
                for( const walltype* mw : glassWalls )
                {
                    const DVector2 q1 = mw->pos, qd = mw->point2Wall()->pos - q1;
                    const double   ql = qd.LengthSquared();
                    double         u0 = 0., u1 = 1e9;
                    if( ql > 1e-6 )
                    {
                        // Build's wall mapping: the repeat is 8 texels a unit, from the panning
                        const double ta = ( ( a - q1 ) | qd ) / ql, tb = ( ( b - q1 ) | qd ) / ql;
                        u0 = mw->xpan_ + std::clamp( ta, 0., 1. ) * mw->xrepeat * 8.;
                        u1 = mw->xpan_ + std::clamp( tb, 0., 1. ) * mw->xrepeat * 8.;
                    }
                    const auto wt = WindowTintSpan( mw->overtexture, u0, u1 );
                    for( int i = 0; i < 3; i++ ) t[ i ] *= wt[ i ];
                }
                return t;
            };
            const float through0 = std::max( 1e-4f, 0.2126f * c[ 0 ] + 0.7152f * c[ 1 ] + 0.0722f * c[ 2 ] );
            {
                const auto  tw   = tintOver( p1, p2 );
                const float tmax = std::max( { tw[ 0 ], tw[ 1 ], tw[ 2 ] } );
                if( tmax <= 0.01f )
                {
                    continue;
                }
            }
            // (stained glass: smaller lights, each with the colours of its own panes)
            // stained glass with rt_glass_shadows: the light outside the glass, shining in
            // through it -- the path tracer colours it texel by texel, the window's pattern
            // on the floor (a light in the room, its colour the panes' mean, threw none);
            // without: smaller lights, each with the colours of its own panes
            // (bars and grates too: their shadows on the floor -- a picture with holes casts
            // shadows; glass only colours its light with rt_glass_shadows)
            bool glassLit = !glassWalls.empty();
            // (not where the outside sees a picture in the opening -- a one-way wall: Blood
            // E3M1's windows are glass from inside and a painted window from the street; lit
            // from outside, the street saw the light on the front round them, red)
            const bool oneWay = ( w.cstat & CSTAT_WALL_1WAY ) || ( w.nextWall() && ( w.nextWall()->cstat & CSTAT_WALL_1WAY ) );
            if( oneWay ) glassLit = false;
            // (bars that are a sprite standing in the opening -- a wall sprite flat in it, as
            // Blood's mortuary windows have: lit from outside too, for their shadows)
            if( !glassLit && !oneWay )
            {
                const DVector2 ud = ( p2 - p1 ).Unit();
                for( sectortype* ss : { &s, o } )
                {
                    CoreSectIterator it( ss );
                    while( auto ac = it.Next() )
                    {
                        const auto& sp = ac->spr;
                        if( ( sp.cstat & CSTAT_SPRITE_ALIGNMENT_MASK ) != CSTAT_SPRITE_ALIGNMENT_WALL || ( sp.cstat & CSTAT_SPRITE_INVISIBLE ) ) continue;
                        const DVector2 q     = sp.pos.XY();
                        const double   dn    = ( q - mid ) | n;
                        const double   along = ( q - p1 ) | ud;
                        if( dn < -24. || dn > 24. || along < -8. || along > width + 8. ) continue;
                        const DVector2 face = sp.Angles.Yaw.ToVector();
                        if( std::abs( face | n ) < 0.9 ) continue;	// (flat in the opening)
                        glassLit = true;
                        break;
                    }
                    if( glassLit ) break;
                }
            }
            for( const walltype* mw : glassWalls )
                if( RT_TableGlass( legacyTileNum( mw->overtexture ) ) && !*rt_glass_shadows ) glassLit = false;
            const int    pieces = std::clamp( int( width / ( glassWalls.empty() || glassLit ? Piece : Piece * 0.5 ) ) + 1, 1, 8 );
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
                // (a door in the way: its share of the opening still open -- seven parallel
                // ways across it, from a metre and a half into the room (past the recess, if
                // any) to as far outside: a door that starts to open lets the light in as it
                // opens, not all at once)
                float open = 1.f;
                {
                    constexpr int Ways = 7;
                    int           free = 0;
                    for( int j = 0; j < Ways; j++ )
                    {
                        const double   t         = ( k + 0.08 + 0.84 * j / ( Ways - 1 ) ) / pieces;
                        const DVector2 onOpening = p1 + ( p2 - p1 ) * t;
                        DVector2       from      = onOpening;
                        if( inner )
                        {
                            const DVector2 q1 = inner->pos, qd = inner->point2Wall()->pos - q1;
                            const double   ql = qd.LengthSquared();
                            if( ql > 1e-6 )
                            {
                                from = q1 + qd * std::clamp( ( ( onOpening - q1 ) | qd ) / ql, 0., 1. );
                            }
                        }
                        if( !SkyPortalBlocked( { &s, o, inner ? inner->nextSector() : nullptr }, top, bottom,
                                               from + n * 48., onOpening - n * 48. ) )
                        {
                            free++;
                        }
                    }
                    if( free == 0 )
                    {
                        continue;
                    }
                    // (squared: a crack lets in little -- the light is in front of the door)
                    open = float( free ) / Ways;
                    open *= open;
                }
                const RgFloat3D pos = WorldToRT( pc.X, pc.Y, ( top + bottom ) * 0.5 );
                const float     fade = ReachFade( pos, int( si ) );
                if( fade <= 0.f )
                {
                    continue;
                }
                // the light outside through its area (rt_sky_portals: more, Blood's skies are dark)
                const float area   = float( pw * h ) * ONEGAMEUNIT_IN_METERS * ONEGAMEUNIT_IN_METERS;
                // (its colour and share: the panes in front of this light)
                const auto              tint    = tintOver( p1 + ( p2 - p1 ) * ( double( k ) / pieces ), p1 + ( p2 - p1 ) * ( double( k + 1 ) / pieces ) );
                const float             lc[ 3 ] = { c[ 0 ] * tint[ 0 ], c[ 1 ] * tint[ 1 ], c[ 2 ] * tint[ 2 ] };
                const float             lm      = std::max( { lc[ 0 ], lc[ 1 ], lc[ 2 ], 1e-4f } );
                const float             ln[ 3 ] = { lc[ 0 ] / lm, lc[ 1 ] / lm, lc[ 2 ] / lm };
                const RgColor4DPacked32 wcolor  = PackColor( ln );
                const float             through = 0.2126f * lc[ 0 ] + 0.7152f * lc[ 1 ] + 0.0722f * lc[ 2 ];
                const float lumens = *rt_sky_portals * outside * area * fade * open * ( through / through0 );
                if( lumens <= 0.05f )
                {
                    continue;
                }
                const float size = float( std::min( pw, h ) ) * ONEGAMEUNIT_IN_METERS;
                const int   dbg  = *rt_sky_portal_debug;
                static const float magenta[ 3 ] = { 1.f, 0.f, 1.f };
                // a small ball (RTGL1's spot lights lit almost nothing here -- a ball of the
                // same strength lit the room: rt_sky_portal_debug 1 and 2 showed it)
                if( glassLit )
                {
                    // three quarters of a metre outside the opening, in the middle of its piece,
                    // a little above the middle (the light comes in from the sky: down onto the
                    // floor), facing into the room; its colour the sky's (the glass gives its own)
                    // (not past the sky's sector: a window whose 'outside' is a narrow gap behind a
                    // building's front -- Blood E3M1's -- put it out in the street, lighting the
                    // front round the window red, and none of it came in)
                    const DVector2 onOpen = p1 + ( p2 - p1 ) * ( ( k + 0.5 ) / pieces );
                    double         out    = 3.;
                    for( double tryOut : { 24., 16., 10., 6. } )
                        if( inside( onOpen.X - n.X * tryOut, onOpen.Y - n.Y * tryOut, o ) == 1 ) { out = tryOut; break; }
                    const DVector2  onp   = onOpen - n * out;
                    const RgFloat3D spos  = WorldToRT( onp.X, onp.Y, top + ( bottom - top ) * 0.35 );
                    // (its cone through the opening, a little wider: not the wall round it)
                    const float coneOut = std::clamp( float( std::atan2( std::max( pw, h ) * 0.5, out ) * 180. / 3.14159265 ) + 8.f, 35.f, 80.f );
                    const FVector3  down  = FVector3( float( n.X ), float( -n.Y ), -0.35f ).Unit();
                    const float     sky   = *rt_sky_portals * outside * area * fade * open;
                    if( dbg )
                    {
                        // (rt_sky_portal_debug: a magenta ball where the light outside is)
                        UploadSphere( SkyPortalBase + ( uint64_t( wallindex( &w ) ) << 3 ) + uint64_t( k ), spos, PackColor( magenta ), 5000.f, 0.05f );
                    }
                    else
                    {
                        UploadSpotLight( SkyPortalBase + ( uint64_t( wallindex( &w ) ) << 3 ) + uint64_t( k ), spos, { down.X, down.Y, down.Z },
                                         PackColor( c ), sky * 0.6f, coneOut, coneOut - 25.f );
                    }
                }
                else
                {
                    UploadSphere( SkyPortalBase + ( uint64_t( wallindex( &w ) ) << 3 ) + uint64_t( k ), pos,
                                  dbg ? PackColor( magenta ) : wcolor, dbg ? 5000.f : lumens, std::clamp( size * 0.1f, 0.05f, 0.2f ) );
                }
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


// rt_lights_info: what each placed light of the level got the last frame
struct MapLightDiag
{
    int   sector = -1;
    float fade = 0.f, sw = -2.f, lumens = 0.f;
};
static std::vector< MapLightDiag > g_mapLightDiag;

// a LIGHTOVR surface light whose surface was shot out (rt_shoot_lights): the floor,
// the ceiling or a wall of its sector it sits on (its position in path tracing space)
static bool SurfaceLightShot( const float pos[ 3 ], int sec )
{
    if( sec < 0 || unsigned( sec ) >= sector.Size() )
    {
        return false;
    }
    const double x = pos[ 0 ], y = -pos[ 2 ], z = -pos[ 1 ];
    auto         s = &sector[ sec ];
    if( RT_LightShot( -1, sec, 1 ) && fabs( z - getflorzofslopeptr( s, x, y ) ) < 8. ) return true;
    if( RT_LightShot( -1, sec, 2 ) && fabs( z - getceilzofslopeptr( s, x, y ) ) < 8. ) return true;
    for( auto& w : s->walls )
    {
        if( !RT_LightShot( wallindex( &w ), -1, 0 ) ) continue;
        const DVector2 a = w.pos, b = w.point2Wall()->pos, q( x, y );
        const DVector2 ab = b - a;
        const double   len2 = ab.LengthSquared();
        const double   t = len2 > 0 ? std::clamp( ( ( q - a ) | ab ) / len2, 0., 1. ) : 0.;
        if( ( a + ab * t - q ).Length() < 4. ) return true;
    }
    return false;
}

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
        const float     fade   = ReachFade( lp, lsec );
        float           lumens = OverlayToLumens( r.intensity, r.radius ) * FlickerScale( id, r.flickerFrames, time ) * fade;
        if( g_mapLightDiag.size() <= i )
        {
            g_mapLightDiag.resize( i + 1 );
        }
        g_mapLightDiag[ i ] = { lsec, fade, -2.f, 0.f };
        // with a light switch: on with it, or (inverse) on while it is off
        if( r.sectorResponse != 0 && r.hasSignalSector )
        {
            const float sw = RT_SectorSwitch( r.signalSector );
            const float on = sw < 0.f ? 1.f : sw;
            lumens *= r.sectorResponse == 2 ? 1.f - on : 0.03f + 0.97f * on;
            g_mapLightDiag[ i ].sw = sw;
        }
        g_mapLightDiag[ i ].lumens = lumens;
        // a spot light (a cinema's projector): aimed at its target, or along its direction
        if( r.lightType.CompareNoCase( "spot" ) == 0 && ( r.hasTarget || r.hasDirection ) )
        {
            RgFloat3D d;
            if( r.hasTarget )
            {
                const RgFloat3D t = WorldToRT( r.target[ 0 ], r.target[ 1 ], r.target[ 2 ] );
                d = { t.data[ 0 ] - lp.data[ 0 ], t.data[ 1 ] - lp.data[ 1 ], t.data[ 2 ] - lp.data[ 2 ] };
            }
            else
            {
                d = WorldToRT( r.direction[ 0 ], r.direction[ 1 ], r.direction[ 2 ] );
            }
            const float len = std::sqrt( d.data[ 0 ] * d.data[ 0 ] + d.data[ 1 ] * d.data[ 1 ] + d.data[ 2 ] * d.data[ 2 ] );
            if( len > 1e-6f )
            {
                d = { d.data[ 0 ] / len, d.data[ 1 ] / len, d.data[ 2 ] / len };
                UploadSpotLight( id, lp, d, PackColor( r.color ), lumens * std::max( 0.f, float( *rt_lightovr_spot_scale ) ),
                                 r.hasCone ? r.coneOuter : 30.f, r.hasCone ? r.coneInner : 20.f );
            }
            continue;
        }
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
        const int lsec = CachedSector( s_surfLightSector, i, p[ 0 ], -p[ 2 ] );
        // its fixture shot out (rt_shoot_lights): the surface it is on was hit -- no light,
        // and its panel there but dark
        const bool  shot = SurfaceLightShot( r.position, lsec );
        const float fade = ReachFade( PTToRT( p ), lsec );
        if( !shot )
        {
            UploadSphere( SurfaceLightBase + i, PTToRT( p ), PackColor( r.color ),
                          OverlayToLumens( r.intensity, r.radius ) * response * fade,
                          std::clamp( std::min( sw, sh ) * 0.5f * ONEGAMEUNIT_IN_METERS, 0.02f, 0.1f ) );
        }

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
            RT_UploadFixtureQuad( CopyId( SurfaceLightBase + i ), corners, nrm, tex.GetChars(), shot ? 0.f : response );
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
            RT_NoSoftenScope fire; // (an actor's light: at its fire, a lamp's bulb)
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

// glowing surfaces shot out (rt_state.h)
CVARD( Bool, rt_shoot_lights, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: glowing surfaces (lamps, panels, screens, signs) go out when shot" )
static std::vector< uint8_t > s_shotWall, s_shotPlane;
static uint32_t               s_shotGen = ~0u;
// from a savegame being loaded: taken on at the loaded level's first frame
static TArray< int >          s_loadedWalls, s_loadedPlanes, s_loadedSprites;
static std::vector< uint8_t > s_shotSprite; // by actor index: a lamp that is a wall or floor sprite
static bool                   s_loadedPending = false;

static void ShotReset()
{
    const uint32_t gen = RT_LevelGeneration();
    if( gen != s_shotGen || s_shotWall.size() != wall.Size() || s_shotPlane.size() != sector.Size() )
    {
        s_shotGen = gen;
        s_shotWall.assign( wall.Size(), 0 );
        s_shotPlane.assign( sector.Size(), 0 );
        s_shotSprite.clear();
        // (a level loaded from a savegame: the lights shot out in it; a level started or
        // restarted: none)
        if( s_loadedPending )
        {
            s_loadedPending = false;
            for( int w : s_loadedWalls )
            {
                if( w >= 0 && size_t( w ) < s_shotWall.size() ) s_shotWall[ size_t( w ) ] = 1;
            }
            for( int e : s_loadedPlanes )
            {
                const int s = e >> 2;
                if( s >= 0 && size_t( s ) < s_shotPlane.size() ) s_shotPlane[ size_t( s ) ] |= uint8_t( e & 3 );
            }
            for( int a : s_loadedSprites )
            {
                if( a >= 0 && a < ( 1 << 20 ) )
                {
                    if( size_t( a ) >= s_shotSprite.size() ) s_shotSprite.resize( size_t( a ) + 1, 0 );
                    s_shotSprite[ size_t( a ) ] = 1;
                }
            }
        }
    }
}

// the lights shot out, in a savegame (SerializeMap)
void RT_SerializeShotLights( FSerializer& arc )
{
    TArray< int > walls, planes, sprites;
    if( arc.isWriting() && s_shotGen == RT_LevelGeneration() )
    {
        for( size_t i = 0; i < s_shotWall.size(); i++ )
        {
            if( s_shotWall[ i ] ) walls.Push( int( i ) );
        }
        for( size_t i = 0; i < s_shotPlane.size(); i++ )
        {
            if( s_shotPlane[ i ] ) planes.Push( int( i ) * 4 + s_shotPlane[ i ] );
        }
        for( size_t i = 0; i < s_shotSprite.size(); i++ )
        {
            if( s_shotSprite[ i ] ) sprites.Push( int( i ) );
        }
    }
    arc( "rt_shotwalls", walls );
    arc( "rt_shotplanes", planes );
    arc( "rt_shotsprites", sprites );
    if( arc.isReading() )
    {
        s_loadedWalls   = std::move( walls );
        s_loadedPlanes  = std::move( planes );
        s_loadedSprites = std::move( sprites );
        s_loadedPending = true;
        s_shotGen       = ~0u; // (taken on at the next look, the same level or not)
    }
}

void RT_ShootLight( int wallnum, int sectnum, int plane )
{
    if( !*rt_shoot_lights )
    {
        return;
    }
    ShotReset();
    if( plane == 0 && wallnum >= 0 && size_t( wallnum ) < s_shotWall.size() )
    {
        s_shotWall[ size_t( wallnum ) ] = 1;
    }
    else if( ( plane == 1 || plane == 2 ) && sectnum >= 0 && size_t( sectnum ) < s_shotPlane.size() )
    {
        s_shotPlane[ size_t( sectnum ) ] |= uint8_t( plane );
    }
}

bool RT_LightShot( int wallnum, int sectnum, int plane )
{
    ShotReset(); // (first: a loaded savegame's list is the loaded level's, not a later one's)
    if( !*rt_shoot_lights )
    {
        return false;
    }
    if( plane == 0 )
    {
        return wallnum >= 0 && size_t( wallnum ) < s_shotWall.size() && s_shotWall[ size_t( wallnum ) ];
    }
    return sectnum >= 0 && size_t( sectnum ) < s_shotPlane.size() && ( s_shotPlane[ size_t( sectnum ) ] & plane );
}

void RT_ShootSprite( int actorIndex )
{
    if( !*rt_shoot_lights || actorIndex < 0 || actorIndex >= ( 1 << 20 ) )
    {
        return;
    }
    ShotReset();
    if( size_t( actorIndex ) >= s_shotSprite.size() )
    {
        s_shotSprite.resize( size_t( actorIndex ) + 256, 0 );
    }
    s_shotSprite[ size_t( actorIndex ) ] = 1;
}

bool RT_SpriteShot( int actorIndex )
{
    ShotReset();
    return *rt_shoot_lights && actorIndex >= 0 && size_t( actorIndex ) < s_shotSprite.size() && s_shotSprite[ size_t( actorIndex ) ];
}

// A shot that hit a wall, a ceiling or a floor where a lamp is a sprite laid on it (Blood's
// fluorescent panels on the ceilings: flat sprites the bullets go through): the sprites
// it hit too
void RT_ShootSpritesAt( const DVector3& hit, int sectnum )
{
    if( !*rt_shoot_lights || sectnum < 0 || unsigned( sectnum ) >= sector.Size() )
    {
        return;
    }
    TArray< int > secs;
    secs.Push( sectnum );
    for( auto& w : sector[ sectnum ].walls )
    {
        if( w.twoSided() && w.nextSector() && secs.Find( sectindex( w.nextSector() ) ) == secs.Size() ) secs.Push( sectindex( w.nextSector() ) );
    }
    constexpr double slack = 2.;
    for( int si : secs )
    {
        TSectIterator< DCoreActor > it( &sector[ si ] );
        while( auto a = it.Next() )
        {
            const int align = a->spr.cstat & CSTAT_SPRITE_ALIGNMENT_MASK;
            if( align != CSTAT_SPRITE_ALIGNMENT_WALL && align != CSTAT_SPRITE_ALIGNMENT_FLOOR && align != CSTAT_SPRITE_ALIGNMENT_SLOPE ) continue;
            auto tex = TexMan.GetGameTexture( a->spr.spritetexture() );
            if( !tex ) continue;
            const double   w  = tex->GetDisplayWidth() * a->spr.scale.X, h = tex->GetDisplayHeight() * a->spr.scale.Y;
            const DVector3 d  = hit - a->spr.pos;
            const DVector2 ax = a->spr.Angles.Yaw.ToVector();
            const double   along = d.X * ax.X + d.Y * ax.Y, across = -d.X * ax.Y + d.Y * ax.X;
            bool hitIt;
            if( align == CSTAT_SPRITE_ALIGNMENT_WALL )
            {
                // (its plane: across its facing; up from its foot, or round its middle)
                hitIt = fabs( along ) <= 4. && fabs( across ) <= w * 0.5 + slack && d.Z <= h * 0.5 + slack && d.Z >= -h - slack;
            }
            else
            {
                hitIt = fabs( d.Z ) <= 6. && ( ( fabs( along ) <= h * 0.5 + slack && fabs( across ) <= w * 0.5 + slack ) ||
                                               ( fabs( along ) <= w * 0.5 + slack && fabs( across ) <= h * 0.5 + slack ) );
            }
            if( hitIt ) RT_ShootSprite( a->GetIndex() );
        }
    }
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
    // the game's own lights (at their fires, torches, flares: not softened)
    for( const RtGameLight& l : g_rtGameLights )
    {
        RT_NoSoftenScope fire;
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
    // (for any game -- Duke has no rt_lookat: what the window lights go by)
    Printf( "  sky light: %s (rt_sky %g, rt_sky_portals %g); %d window lights last frame\n",
            g_rtSkyLight.valid ? FStringf( "from the sky's picture, colour %.2f %.2f %.2f", g_rtSkyLight.average[ 0 ], g_rtSkyLight.average[ 1 ],
                                           g_rtSkyLight.average[ 2 ] ).GetChars()
                               : "none yet (no sky drawn on this level since it started: no window lights)",
            float( *rt_sky ), float( *rt_sky_portals ), g_rtSkyPortalStats.count );
    if( g_rtViewSector >= 0 && unsigned( g_rtViewSector ) < sector.Size() )
    {
        auto& vs = sector[ g_rtViewSector ];
        Printf( "  view in sector %d: %s\n", g_rtViewSector, ( vs.ceilingstat & CSTAT_SECTOR_SKY ) ? "open to the sky" : OpenAir( &vs ) ? "roofed far up, by the sky: open air" : "roofed" );
    }
    // the windows near: walls with a picture between two sectors, and what is on each side
    for( unsigned si = 0; si < sector.Size(); si++ )
    {
        for( auto& w : sector[ si ].walls )
        {
            if( !w.twoSided() || !( w.cstat & ( CSTAT_WALL_MASKED | CSTAT_WALL_1WAY ) ) || w.nextsector < int( si ) ) continue;
            const DVector2 mid = ( w.pos + w.point2Wall()->pos ) * 0.5;
            if( ( mid - cam ).Length() / 32. > 6. ) continue;
            auto o = w.nextSector();
            Printf( "  window (wall %d, tile %d): sector %u (%s) | sector %d (%s), %.1f m away\n", wallindex( &w ), legacyTileNum( w.overtexture ), si,
                    OpenAir( &sector[ si ] ) ? "open air" : "roofed", sectindex( o ), OpenAir( o ) ? "open air" : "roofed",
                    ( mid - cam ).Length() / 32. );
        }
    }
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
            const bool inSky = OpenAir( &s ), outSky = OpenAir( o );
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
            // (the pictures in its sector's other walls: bars or glass a recess in)
            for( auto& w2 : s.walls )
            {
                if( &w2 == &w ) continue;
                const walltype* m2 = ( w2.cstat & CSTAT_WALL_MASKED ) ? &w2 : ( w2.nextWall() && ( w2.nextWall()->cstat & CSTAT_WALL_MASKED ) ) ? w2.nextWall() : nullptr;
                if( m2 && m2->overtexture.isValid() )
                    Printf( "      its sector's wall %d has a picture: tile %d%s, to sector %d (cstat %x)\n", wallindex( &w2 ), legacyTileNum( m2->overtexture ),
                            RT_TableGlass( legacyTileNum( m2->overtexture ) ) ? " (glass)" : "", w2.nextsector, unsigned( m2->cstat ) );
            }
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
    // the placed lights: where, and how much of each the path tracer got the last frame
    const DVector3 view( g_rtViewPos[ 0 ], g_rtViewPos[ 1 ], g_rtViewPos[ 2 ] );
    for( unsigned i = 0; i < set.mapLightRules.Size(); i++ )
    {
        const auto& r = set.mapLightRules[ i ];
        const MapLightDiag d = i < g_mapLightDiag.size() ? g_mapLightDiag[ i ] : MapLightDiag{};
        const DVector3 p( r.anchorPosition[ 0 ], r.anchorPosition[ 1 ], r.anchorPosition[ 2 ] );
        Printf( "  light \"%s\" (%s%s): sector %d, %.1f m away, reach %.2f, %s, %.0f lumens last frame%s\n", r.id.GetChars(),
                r.lightType.IsEmpty() ? "point" : r.lightType.GetChars(), r.hasTarget ? ", aimed" : "", d.sector,
                r.anchorType == LightOverlayAnchorType::Position ? ( p - view ).Length() / 32. : -1., d.fade,
                !r.hasSignalSector || !r.sectorResponse ? FString( "no switch" ).GetChars()
                    : FStringf( "switch of sector %d: %.2f (%s)", r.signalSector, d.sw, r.sectorResponse == 2 ? "on while it is off" : "on with it" ).GetChars(),
                d.lumens, d.lumens > 0.01f ? "" : " -- not lit" );
    }
}

// THE NEAR FALLOFF (rt_light_soft). A light is a small ball (or disk): its light falls
// off with the square of the distance from it, but no faster than its size allows --
// right next to it, a surface sees it as a ball of its size and is lit by how bright
// its surface is. A strong light given a small ball (the lava's, a big glowing floor's,
// a blast's) is a tiny, hugely bright one, and the wall beside it burns out to a white
// patch. So a light's ball is made at least so big that it is no brighter, right next
// to it, than a set amount -- its radius at least 0.005 m x sqrt(its intensity) x
// rt_light_soft: a torch (600) 0.12 m, a lava pool's light (20 000) 0.7 m. Further off
// a light is exactly as bright as before; its shadows are a little softer.
CVARD( Float, rt_light_soft, 1.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: how much a strong light's glare right next to it is softened (a wall by lava, a lamp's ceiling) -- 0 = not at all; further off a light is as bright as ever" )

namespace rtx
{
int g_rtNoSoften = 0;
const RgLightInfo* RT_SoftenLight( const RgLightInfo* info )
{
    const float soft = std::clamp( float( *rt_light_soft ), 0.f, 4.f );
    if( !info || soft <= 0.f || !info->pNext || g_rtNoSoften > 0 )
    {
        return info;
    }
    static thread_local RgLightInfo        s_info;
    static thread_local RgLightSphericalEXT s_sphere;
    static thread_local RgLightSpotEXT      s_spot;
    const auto* hdr = static_cast< const RgLightSphericalEXT* >( info->pNext ); // (sType first in each)
    auto minRadius  = [ & ]( float intensity ) { return std::min( 3.f, 0.005f * soft * std::sqrt( std::max( intensity, 0.f ) ) ); };
    if( hdr->sType == RG_STRUCTURE_TYPE_LIGHT_SPHERICAL_EXT )
    {
        const auto* l = static_cast< const RgLightSphericalEXT* >( info->pNext );
        const float r = minRadius( l->intensity );
        if( l->radius >= r ) return info;
        s_sphere        = *l;
        s_sphere.radius = r;
        s_info          = *info;
        s_info.pNext    = &s_sphere;
        return &s_info;
    }
    if( hdr->sType == RG_STRUCTURE_TYPE_LIGHT_SPOT_EXT )
    {
        const auto* l = static_cast< const RgLightSpotEXT* >( info->pNext );
        const float r = minRadius( l->intensity );
        if( l->radius >= r ) return info;
        s_spot        = *l;
        s_spot.radius = r;
        s_info        = *info;
        s_info.pNext  = &s_spot;
        return &s_info;
    }
    return info;
}
} // namespace rtx
