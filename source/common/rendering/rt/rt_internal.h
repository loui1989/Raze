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
constexpr uint64_t FlashlightLightId = 0xF0000000ull + 1;

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

void RT_UploadSceneLights( uint32_t time ); // rt_lights.cpp
