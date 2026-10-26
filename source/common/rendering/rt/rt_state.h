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

// Set by the game: the camera is under water (the picture takes the water's tint)
extern bool g_rtCameraUnderwater;

// Set by the game when the player fires: the counter goes up, the event names the
// LIGHTOVR smoke event rule ("duke.pistol.primary"...), or is null
extern uint32_t    g_rtShotCounter;
extern const char* g_rtShotEvent;

// The player's own actor: drawn only where the player cannot see it himself
// (mirrors, reflections, shadows)
extern const void* g_rtViewerActor;
// Set by the game: sectors copied to another place this frame -- per sector the
// copy it belongs to (1..RT_MAX_COPIES, 0 = none; or empty), each copy moved by its
// g_rtCopyOffset (game units, z down). Duke's water: the world above a surface lives
// elsewhere on the map; under water it is copied over the room
constexpr int RT_MAX_COPIES = 8;
extern std::vector< uint8_t > g_rtCopySector;
extern double                 g_rtCopyOffset[ RT_MAX_COPIES ][ 3 ];
inline int RT_CopySector( int sectnum )
{
    return sectnum >= 0 && size_t( sectnum ) < g_rtCopySector.size() ? g_rtCopySector[ size_t( sectnum ) ] : 0;
}
// per copy: the linked side's above-water floors (Duke's lotag 1) are the water
// surface itself, already drawn by the room under it: left out of the copy
extern bool g_rtCopySkipWater[ RT_MAX_COPIES ];

// What a game links this frame, for the copies (set before it renders the view; the
// renderer turns them into g_rtCopySector and empties the list). An opening ("hole")
// on this side shows the place at hole + delta: Duke's water surfaces. Raze's sector
// portals (Blood's and Redneck Rampage's rooms over rooms) are added by the renderer.
struct RtCopyLink
{
    DVector3           delta;                   // hole side + delta = linked side (game units)
    int                start           = -1;    // a sector of the linked side
    std::vector< int > holes;                   // the openings' sectors (this side)
    bool               skipWaterFloors = false; // see g_rtCopySkipWater
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

    // the copy (see g_rtCopySector) the primitive belongs to, 0 = none
    [[nodiscard]] auto push_copygroup( int copy )
    {
        const int prev = m_copyGroup;
        m_copyGroup    = copy;
        return rtdetail::AutoPop{
            prev,
            []( FRtState& fthis, int p ) { fthis.m_copyGroup = p; },
            *this,
        };
    }
    int get_copygroup() const { return m_copyGroup; }

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
    int      m_copyGroup{ 0 };

    std::unordered_map< uint64_t, uint32_t > m_nextPrimIndex{};
};

extern FRtState rtstate;

// Pushes a flag only while RT is active, and nothing otherwise; for the scene hooks.
#define RT_PUSH_TYPE( var, prim ) auto var = rtstate.push_type( RT_IsActive() ? ( prim ) : RtPrim::Identity )
#define RT_PUSH_ID( var, obj, kind ) auto var = rtstate.push_uniqueid( ( obj ), ( kind ) )
