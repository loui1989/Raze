#pragma once

// Raze RT: shared internals of the RTGL1 backend (not for the scene code; that
// only needs rt_state.h).

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>

#include "vectors.h"
#include "palentry.h"
#include "printf.h"
#include "c_cvars.h"
#include "hw_renderstate.h"

#ifdef _WIN32
    #define RG_USE_SURFACE_WIN32
#elif defined( __APPLE__ )
    #define RG_USE_SURFACE_METAL
#endif
#include <RTGL1/RTGL1.h>

#include "rt_state.h"

extern RgInterface rt;

namespace rtx
{

// Build map units are the same size as Doom's after Raze's conversion
// (x/16, z/256): a player is about 40 units to the eyes. 32 units = 1 metre.
constexpr float ONEGAMEUNIT_IN_METERS = 1.0f / 32.0f;

constexpr auto RG_PACKED_COLOR_WHITE = RgColor4DPacked32{ 0xFFFFFFFF };

#define RT_TRANSFORM_IDENTITY                                                  \
    RgTransform                                                                \
    {                                                                          \
        {                                                                      \
            { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }                     \
        }                                                                      \
    }

// Light ids, kept away from each other.
constexpr uint64_t FlashlightLightId   = 0xF0000000ull + 1;
constexpr uint64_t MuzzleFlashLightId  = 0xF0000000ull + 2;

void RT_Check( RgResult r, const char* what );
#define RG_CHECK( r ) ::rtx::RT_Check( ( r ), #r )

inline float applygamma( float x )
{
    return std::clamp( x * x, 0.f, 1.f );
}

inline uint8_t applygamma8( uint8_t x )
{
    return static_cast< uint8_t >( applygamma( float( x ) / 255.f ) * 255.f );
}

inline auto rtcolor( const PalEntry& e ) -> RgColor4DPacked32
{
    return rt.rgUtilPackColorByte4D( e.r, e.g, e.b, e.a );
}

inline auto rtcolor_bgr_alphagamma( const PalEntry& e ) -> RgColor4DPacked32
{
    return rt.rgUtilPackColorByte4D( e.b, e.g, e.r, applygamma8( e.a ) );
}

template< typename T >
RgFloat4D ApplyMat44ToVec4( const T* column_mat, const RgFloat4D& vs )
{
    const auto* v = vs.data;
    RgFloat4D   r;
    for( int i = 0; i < 4; i++ )
    {
        r.data[ i ] = column_mat[ 0 * 4 + i ] * T( v[ 0 ] ) + column_mat[ 1 * 4 + i ] * T( v[ 1 ] ) +
                      column_mat[ 2 * 4 + i ] * T( v[ 2 ] ) + column_mat[ 3 * 4 + i ] * T( v[ 3 ] );
    }
    return r;
}

// result = a * b, both column major 4x4
inline void MatMul44( const float* a, const float* b, float* result )
{
    for( int i = 0; i < 4; ++i )
    {
        for( int j = 0; j < 4; ++j )
        {
            float s = 0.f;
            for( int k = 0; k < 4; ++k )
            {
                s += a[ k * 4 + i ] * b[ j * 4 + k ];
            }
            result[ j * 4 + i ] = s;
        }
    }
}

// lights uploaded this frame, for rt_debug_stats
extern uint32_t g_rtLightsThisFrame;
extern uint32_t g_rtLightsFailed;
inline RgResult RT_UploadLight( const RgLightInfo* info )
{
    g_rtLightsThisFrame++;
    const RgResult r = rt.rgUploadLight( info );
    if( r != RG_RESULT_SUCCESS )
    {
        g_rtLightsFailed++;
    }
    return r;
}

} // namespace rtx

// cvars of the backend, defined in rt_main.cpp
EXTERN_CVAR( Bool, rt_flashlight )
EXTERN_CVAR( Float, rt_flashlight_intensity )
EXTERN_CVAR( Float, rt_classic )
EXTERN_CVAR( Bool, rt_world_white )
EXTERN_CVAR( Float, rt_fullbright_emis )
EXTERN_CVAR( Float, rt_additive_emis )
EXTERN_CVAR( Int, rt_debug_uniqueid )
EXTERN_CVAR( Float, rt_emis_mult )
EXTERN_CVAR( Bool, rt_emis_lights )
EXTERN_CVAR( Float, rt_emis_light_scale )
EXTERN_CVAR( Float, rt_emis_gi )
EXTERN_CVAR( Bool, rt_emis_follow_shade )
EXTERN_CVAR( Float, rt_ambient )
EXTERN_CVAR( Bool, rt_polish )
EXTERN_CVAR( Bool, rt_glass )
EXTERN_CVAR( Float, rt_decal_offset )
EXTERN_CVAR( Float, rt_flat_offset )
EXTERN_CVAR( Bool, rt_sprite_lights )
EXTERN_CVAR( Float, rt_sprite_light_scale )
EXTERN_CVAR( Bool, rt_world_motion )
EXTERN_CVAR( Float, rt_sector_light_scale )

void RT_UploadSceneLights( uint32_t time ); // rt_lights.cpp
uint32_t RT_LevelGeneration();              // rt_lights.cpp: changes when a map is (re)loaded
// rt_lights.cpp: is the point (RTGL1 space) inside the sector (true if the sector is unknown)
bool RT_PointInSector( int sectnum, float x, float y );
// Duke-RT's LIGHTOVR has actor rules for this game (else glowing sprites cast lights)
bool RT_HaveActorLightRules();
// the sector's floor (ceiling) is at the height of an overlapping sector's ceiling (floor)
bool RT_FlatTouchesOther( int sectnum, bool ceiling );
// 0..1: how much of a light at p (RT metres) in sector 'sect' is within the light reach
float RT_LightReachFade( const RgFloat3D& p, int sect );
// rt_lights.cpp: smoothed / current light level of a sector (a flickering sector's
// surfaces glow with the smoothed one)
float RT_SectorFlickerSmoothing( int sectnum );
// rt_lights.cpp: LIGHTOVR emissiveoverride for a glowing surface: returns the light's scale
// and sets glow (the switch response of its signal sector, if it has one)
float RT_EmissiveOverride( int sectnum, int tile, float& glow );
// rt_smoke.cpp: make, move and pack the smoke; returns the number of puffs
uint32_t RT_UpdateSmoke( const RgFloat3D& eye, const RgFloat3D& fwd, const RgFloat3D& up, const RgFloat3D& right,
                         float far, RgFloat4D* outPuffs, RgFloat4D* outAlbDen, RgFloat4D* outShape );
EXTERN_CVAR( Bool, rt_smoke_illum )
EXTERN_CVAR( Float, rt_smoke_maxlight )
EXTERN_CVAR( Float, rt_smoke_light_near )
EXTERN_CVAR( Float, rt_smoke_history )
// rt_draw.cpp: a quad with a tile's texture, glowing (a LIGHTOVR surface light's fixture)
void RT_UploadFixtureQuad( uint64_t id, const float corners[ 4 ][ 3 ], const float normal[ 3 ], const char* tileName, float glow );
