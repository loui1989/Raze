#pragma once

// Raze RT: what the scene code tells the RTGL1 backend about the primitive it is
// about to draw. The scene code (core/rendering/scene) pushes these around its
// draw calls; RTRenderState::InternalDraw reads them.
//
// Adapted from gzdoom-rt's rt_state.h (Doom 64 RT). The main difference is that
// every unique id keeps its own primitive counter for the frame, so a scene
// object that draws more than once per frame (a wall split in parts, a sprite in
// two passes) can never produce a duplicate (id, primitive index) pair, which
// RTGL1 would silently drop.

#include <cassert>
#include <cstdint>
#include <unordered_map>
#include <vector>
#include <type_traits>

#include "vectors.h"
#include "palentry.h"

// True while the ray tracing frame buffer is the active renderer. Scene hooks use
// this so that the OpenGL path is untouched when RT is off.
extern bool g_rtActive;

// 0..1: how bright the player's weapon flash is this frame, set by the game before
// it renders the view (the path tracer adds a light for it)
extern float g_rtMuzzleFlash;
// a lightning flash now (0..1): the game's storm brightening the sky sectors (Blood)
extern float g_rtSkyFlash;

// Set by the game: the camera is under water (the picture takes the water's tint)
extern bool g_rtCameraUnderwater;

// Set by the game when the player fires: the counter goes up, the event names the
// LIGHTOVR smoke event rule ("duke.pistol.primary"...), or is null
extern uint32_t    g_rtShotCounter;
extern const char* g_rtShotEvent;

// The player's own actor: drawn only where the player cannot see it himself
// (mirrors, reflections, shadows)
extern const void* g_rtViewerActor;
// what of it was drawn the last frame (rt_lookat): how many times, its tile, as a model
extern int  g_rtViewerDrawn, g_rtViewerDrawnTile;
extern bool g_rtViewerDrawnModel;
// per sprite (its actor's index), the frame it last got to each stage of drawing
// (rt_lookat): 0 processed (the game gave it), 1 placed (a picture or model made), 2 drawn
extern uint32_t g_rtFrameNo;
// The light of the sky in view (a Build sky's picture): from its brightest part, its
// colour. The sky's draw gives the direction in the dome's own axes (pending); the
// backend turns it into the world's (dir: towards the light, z up) as it draws the sky
struct RtSkyLight
{
    bool  pending = false, valid = false;
    float local[ 3 ]{};
    float dir[ 3 ]{ 0, 0, 1 };
    float color[ 3 ]{ 1, 1, 1 };
    float irradiance = 0.f;   // per rt_sky: pi * its average radiance, more for a bright moon
    float diameter   = 2.f;   // degrees: a moon's small, a bright stretch of clouds' wide
};
extern RtSkyLight g_rtSkyLight;
extern std::vector< uint32_t > g_rtSpriteStage[ 3 ];
inline void RT_MarkSprite( int index, int stage )
{
    if( index < 0 || index > 1 << 20 ) return;
    auto& v = g_rtSpriteStage[ stage ];
    if( size_t( index ) >= v.size() ) v.resize( size_t( index ) + 256, 0 );
    v[ size_t( index ) ] = g_rtFrameNo;
}
inline bool RT_SpriteAt( int index, int stage )
{
    const auto& v = g_rtSpriteStage[ stage ];
    return index >= 0 && size_t( index ) < v.size() && v[ size_t( index ) ] == g_rtFrameNo && g_rtFrameNo != 0;
}
// Set by the game: sectors copied to another place this frame -- per sector the
// copy it belongs to (1..RT_MAX_COPIES, 0 = none; or empty), each copy moved by its
// g_rtCopyOffset (game units, z down). Duke's water: the world above a surface lives
// elsewhere on the map; under water it is copied over the room
constexpr int RT_MAX_COPIES = 32;
extern int g_rtCopyUsed; // copies (slots of g_rtCopyOffset) handed out this frame
// While a room-over-room portal's view is drawn: the sectors it may show (per sector,
// or empty = all). The rasterizer's stencil keeps the rest of that view out; the path
// tracer has no stencil, so only the linked place near the opening is drawn.
extern std::vector< char > g_rtPortalFilter;
inline bool RT_PortalFiltered( int sectnum )
{
    return !g_rtPortalFilter.empty() && ( sectnum < 0 || size_t( sectnum ) >= g_rtPortalFilter.size() || !g_rtPortalFilter[ size_t( sectnum ) ] );
}
// Real sectors left out this frame: a room-over-room view is drawn where they are
// (the linked place stacked right over the room, or the crypt over the coffin), and
// the viewer cannot reach them from here. Their lights are out too.
extern std::vector< char > g_rtHiddenSector;
inline bool RT_SectorHidden( int sectnum )
{
    return sectnum >= 0 && size_t( sectnum ) < g_rtHiddenSector.size() && g_rtHiddenSector[ size_t( sectnum ) ];
}
// what is not drawn: outside the room-over-room view being drawn, or (the real map)
// left out for one
inline bool RT_SkipSector( int sectnum )
{
    return g_rtPortalFilter.empty() ? RT_SectorHidden( sectnum ) : RT_PortalFiltered( sectnum );
}
extern std::vector< uint8_t > g_rtCopySector;
extern double                 g_rtCopyOffset[ RT_MAX_COPIES ][ 3 ];
inline int RT_CopySector( int sectnum )
{
    return sectnum >= 0 && size_t( sectnum ) < g_rtCopySector.size() ? g_rtCopySector[ size_t( sectnum ) ] : 0;
}
// per copy, a water copy: 1 = the world above a surface (seen from under it): its
// above-water floors (Duke's lotag 1) are the surface itself, already drawn by the
// room under it, so they are left out of the copy; 2 = the room under a surface
// (seen from above it): its ceilings that are the surface are left out. 0 = neither.
extern uint8_t g_rtCopyWater[ RT_MAX_COPIES ];
// Which side of a water surface each sector is on, for the water copies: 1 = above
// (its floor is the surface), 2 = under water, 0 = neither. Set by a game whose
// sector lotags mean something else (Blood: its water links) each frame, or empty:
// Duke's lotags 1 and 2.
extern std::vector< uint8_t > g_rtWaterSide;
inline int RT_WaterSide( int sectnum, int lotag )
{
    if( !g_rtWaterSide.empty() )
        return sectnum >= 0 && size_t( sectnum ) < g_rtWaterSide.size() ? g_rtWaterSide[ size_t( sectnum ) ] : 0;
    return lotag == 1 ? 1 : lotag == 2 ? 2 : 0;
}

// What a game links this frame, for the copies (set before it renders the view; the
// renderer turns them into g_rtCopySector and empties the list). An opening ("hole")
// on this side shows the place at hole + delta: Duke's water surfaces. Raze's sector
// portals (Blood's and Redneck Rampage's rooms over rooms) are added by the renderer.
struct RtCopyLink
{
    DVector3           delta;                   // hole side + delta = linked side (game units)
    int                start           = -1;    // a sector of the linked side
    std::vector< int > holes;                   // the openings' sectors (this side)
    uint8_t            water           = 0;     // see g_rtCopyWater
};
extern std::vector< RtCopyLink > g_rtCopyLinks;

inline const double* RT_CopyOffset( int copy )
{
    static const double none[ 3 ] = {};
    return copy >= 1 && copy <= RT_MAX_COPIES ? g_rtCopyOffset[ copy - 1 ] : none;
}
// Lights the game itself names this frame (Blood: explosions, burning things,
// flares...), set before it renders the view. Position in game units (z down);
// colour 0..1; intensity in lumens; radius of the source in metres; id stable while
// the light lives; sector (-1 unknown) for the light reach.
struct RtGameLight
{
    double   pos[ 3 ];
    float    color[ 3 ];
    float    intensity;
    float    radius;
    uint64_t id;
    int      sector;
};
extern std::vector< RtGameLight > g_rtGameLights;

// Smoke the game itself makes this frame (Blood -- Duke's comes from Duke-RT's
// LIGHTOVR rules), set before it renders the view. A burst (an explosion, a shot)
// is made once per id; a fire or a trail smokes for as long as it is listed.
// Position in game units (z down); a shot's is the view's own.
enum class RtSmokeKind : uint8_t
{
    Explosion,
    Fire,
    Trail,
    Shot,
};
struct RtSmokeSource
{
    double      pos[ 3 ];
    uint64_t    id;
    RtSmokeKind kind;
    float       scale; // 1 = as usual
};
extern std::vector< RtSmokeSource > g_rtSmokeSources;

// where the main view is rendered from (game units, z down) and its sector; -1 = unknown
extern int    g_rtViewSector;
extern double g_rtViewPos[ 3 ];

// Per sector, set by the game before it renders the view: -1 for a sector without a
// light switch, else how far its lights are on (0 off .. 1 on). A switched room sits
// at about half light when off, which the path tracer cannot tell from a dim room.
extern std::vector< float > g_rtSectorSwitch;
inline float RT_SectorSwitch( int sectnum )
{
    return sectnum >= 0 && size_t( sectnum ) < g_rtSectorSwitch.size() ? g_rtSectorSwitch[ size_t( sectnum ) ] : -1.f;
}
// the material table's flame class (<rt>/materials.txt: a candle, a torch whose flame is
// in its picture): how brightly the tile's flame glows, 0 if not one; and its colours
float RT_TableFlame( int tile );
// the table's "light" lines: a decoration with a light of its own (a lantern), how strong,
// its colour (negative: the sprite's brightest colour)
bool  RT_TableLight( int tile, float& strength, float rgb[ 3 ] );
bool  RT_FlameColor( uint8_t r, uint8_t g, uint8_t b );

inline bool RT_IsActive()
{
    return g_rtActive;
}

struct FRtState;

namespace rtdetail
{
template< typename Payload, typename DestroyFunc >
struct AutoPop
{
    explicit AutoPop( Payload pl, DestroyFunc&& func, FRtState& st )
        : m_st{ st }, m_payload{ pl }, m_func{ func }
    {
    }

    ~AutoPop() { m_func( m_st, m_payload ); }

    AutoPop( const AutoPop& )                = delete;
    AutoPop( AutoPop&& ) noexcept            = delete;
    AutoPop& operator=( const AutoPop& )     = delete;
    AutoPop& operator=( AutoPop&& ) noexcept = delete;

private:
    FRtState&   m_st;
    Payload     m_payload;
    DestroyFunc m_func;
};
} // namespace rtdetail

enum class RtPrim : uint32_t
{
    Identity       = 0,
    Ignored        = 1 << 0,
    Sky            = 1 << 1, // sky dome / sky box contents
    SkyVisibility  = 1 << 2, // geometry through which the sky is seen
    Mirror         = 1 << 3,
    Glass          = 1 << 4,
    Sprite         = 1 << 5, // camera facing billboard
    Water          = 1 << 6,
    Lava           = 1 << 7,
    Slime          = 1 << 8,
    Fullbright     = 1 << 9, // drawn at full brightness by the game (shade at or below the fullbright cutoff)
    NoShadow       = 1 << 10, // casts no shadow (a fire sprite around its own light)
    Decal          = 1 << 11, // a wall or floor aligned sprite, usually flush with a surface
    Viewer         = 1 << 12, // the player's own body: only in reflections and shadows
    CopyMove       = 1 << 13, // a copy of geometry elsewhere: moved by g_rtCopyOffset, its own ids
    CopyId         = 1 << 14, // a copy already in place (copied sprites): its own ids
    FloorPlane     = 1 << 15, // a sector's floor
    CeilingPlane   = 1 << 16, // a sector's ceiling
    BloodPool      = 1 << 17, // a pool of blood (Blood): the stylized liquid, red
    OneSided       = 1 << 18, // a solid wall part, a floor or ceiling: seen only from its sector's side
};

// Per-actor policy from Duke-RT's LIGHTOVR actor rules, set by the backend when it
// is active: bit 0 = no shadow, bit 1 = fullbright.
class DCoreActor;
extern int ( *RT_ActorRuleFlagsHook )( DCoreActor* actor );
inline int RT_ActorRuleFlags( DCoreActor* actor )
{
    return RT_ActorRuleFlagsHook && actor ? RT_ActorRuleFlagsHook( actor ) : 0;
}

// Unique id salts: the same game object can be drawn as different kinds of
// primitive, they must not share ids.
enum class RtIdKind : uint64_t
{
    Wall        = 1,
    WallSprite  = 2,
    FloorFlat   = 3,
    CeilingFlat = 4,
    FlatSprite  = 5,
    Sprite      = 6,
    Model       = 7,
    Portal      = 8,
    Sky         = 9,
    UI          = 10,
    Other       = 11,
    Copy        = 12,
};

struct FRtState
{
    FRtState()                                       = default;
    ~FRtState()                                      = default;
    FRtState( const FRtState& other )                = delete;
    FRtState( FRtState&& other ) noexcept            = delete;
    FRtState& operator=( const FRtState& other )     = delete;
    FRtState& operator=( FRtState&& other ) noexcept = delete;

    void reset() { m_nextPrimIndex.clear(); }

    [[nodiscard]] auto push_type( RtPrim s )
    {
        // the bits that were not set before are the ones to clear on pop
        const uint32_t added = uint32_t( s ) & ~m_state;
        m_state |= uint32_t( s );

        return rtdetail::AutoPop{
            added,
            []( FRtState& fthis, uint32_t a ) { fthis.m_state &= ~a; },
            *this,
        };
    }

    // the opposite: these types off for a while (set again on pop, if they were)
    [[nodiscard]] auto push_without( RtPrim s )
    {
        const uint32_t removed = uint32_t( s ) & m_state;
        m_state &= ~uint32_t( s );

        return rtdetail::AutoPop{
            removed,
            []( FRtState& fthis, uint32_t r ) { fthis.m_state |= r; },
            *this,
        };
    }

    template< RtPrim Value >
    [[nodiscard]] bool is() const
    {
        static_assert( Value != RtPrim::Identity );
        return m_state & uint32_t( Value );
    }

    // 'id' must identify the game object between frames; 'kind' separates the
    // different primitive kinds one object can produce.
    [[nodiscard]] auto push_uniqueid( uint64_t id, RtIdKind kind )
    {
        const uint64_t prev = m_curUniqueID;
        m_curUniqueID       = MixId( id, kind );

        return rtdetail::AutoPop{
            prev,
            []( FRtState& fthis, uint64_t p ) { fthis.m_curUniqueID = p; },
            *this,
        };
    }

    template< typename T >
    [[nodiscard]] auto push_uniqueid( const T* obj, RtIdKind kind )
    {
        return push_uniqueid( reinterpret_cast< uint64_t >( obj ), kind );
    }

    // the sector the primitive belongs to (-1: none)
    [[nodiscard]] auto push_sector( int sectnum )
    {
        const int prev = m_sector;
        m_sector       = sectnum;
        return rtdetail::AutoPop{
            prev,
            []( FRtState& fthis, int p ) { fthis.m_sector = p; },
            *this,
        };
    }
    int get_sector() const { return m_sector; }

    // the copy (see g_rtCopySector) the primitive belongs to, 0 = none (keeps the
    // copy around it: a portal's view), and the offset that moves it into place.
    // 'add': on top of the copy around it (a portal seen through a portal)
    struct CopyState
    {
        int    group;
        double ofs[ 3 ];
    };
    [[nodiscard]] auto push_copygroup( int copy, const double* ofs = nullptr, bool add = false )
    {
        const CopyState prev = m_copy;
        if( copy != 0 )
        {
            m_copy.group = copy;
            const double* o = ofs ? ofs : RT_CopyOffset( copy );
            for( int i = 0; i < 3; i++ )
            {
                m_copy.ofs[ i ] = ( add ? prev.ofs[ i ] : 0.0 ) + o[ i ];
            }
        }
        return rtdetail::AutoPop{
            prev,
            []( FRtState& fthis, CopyState p ) { fthis.m_copy = p; },
            *this,
        };
    }
    int           get_copygroup() const { return m_copy.group; }
    const double* get_copyoffset() const { return m_copy.ofs; }

    bool has_uniqueid() const { return m_curUniqueID != 0; }
    uint64_t get_uniqueid() const { return m_curUniqueID; }

    // the next primitive index for 'id' this frame
    uint32_t next_primitiveindex( uint64_t id ) { return m_nextPrimIndex[ id ]++; }

    static uint64_t MixId( uint64_t id, RtIdKind kind )
    {
        // splitmix64-style mixing; never 0
        uint64_t x = id + 0x9E3779B97F4A7C15ull * ( uint64_t( kind ) + 1 );
        x          = ( x ^ ( x >> 30 ) ) * 0xBF58476D1CE4E5B9ull;
        x          = ( x ^ ( x >> 27 ) ) * 0x94D049BB133111EBull;
        x          = x ^ ( x >> 31 );
        return x == 0 ? 1 : x;
    }

private:
    uint32_t m_state{ 0 };       // RtPrim flags
    uint64_t m_curUniqueID{ 0 }; // to identify an object between frames
    int      m_sector{ -1 };
    CopyState m_copy{ 0, { 0, 0, 0 } };

    std::unordered_map< uint64_t, uint32_t > m_nextPrimIndex{};
};

extern FRtState rtstate;

// Pushes a flag only while RT is active, and nothing otherwise; for the scene hooks.
#define RT_PUSH_TYPE( var, prim ) auto var = rtstate.push_type( RT_IsActive() ? ( prim ) : RtPrim::Identity )
#define RT_PUSH_ID( var, obj, kind ) auto var = rtstate.push_uniqueid( ( obj ), ( kind ) )
