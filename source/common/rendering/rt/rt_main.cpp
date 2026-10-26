// Raze RT: the RTGL1 path tracer as a Raze hardware renderer.
//
// RTGL1 (the native Metal build from Doom 64 RT for macOS) is loaded at run time
// from <rt folder>/bin/libRTGL1.dylib. The rt folder is $RAZE_RT_DIR, or "rt"
// in the working directory.
//
// The frame: BeginFrame -> rgStartFrame; the scene code uploads the camera
// (RT_SetMainCamera) and every primitive through RTRenderState; Update draws the
// 2D and calls rgDrawFrame.
//
// Adapted from gzdoom-rt's rt_main.cpp (Doom 64 RT).

#include "rt_backend.h"

#include "c_dispatch.h"
#include "m_argv.h"
#include "hw_lightbuffer.h"
#include "hw_bonebuffer.h"
#include "hw_cvars.h"
#include "v_draw.h"

#include <chrono>
#include <cstdlib>
#include <string>

RgInterface rt = {};

//
// cvars
//

// 0 = native resolution (no upscaler); 1..4 = MetalFX quality, balanced, performance,
// ultra performance; 5 = MetalFX at rt_renderscale
CVARD( Int, rt_upscale, 3, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: MetalFX upscaling (0 off, 1 quality, 2 balanced, 3 performance, 4 ultra performance, 5 custom rt_renderscale)" )
CVARD( Float, rt_renderscale, 0.5f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: render resolution scale, when rt_upscale is 0 or 5" )
CVARD( Bool, rt_metalfx_denoiser, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: MetalFX denoising upscaler instead of RTGL1's denoiser (needs rt_upscale 1..5)" )
CVARD( Int, rt_sharpen, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: 0 auto, 1 naive, 2 AMD CAS, 3 none" )
CVARD( Bool, rt_vsync, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: vertical sync" )
CVARD( Float, rt_classic, 0.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: 0..1, how much of the screen uses the classic (not path traced) look" )
CVARD( Bool, rt_world_white, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: do not bake the game's light colour into surfaces" )
CVARD( Float, rt_fullbright_emis, 1.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: emission of surfaces the game draws fullbright" )
CVARD( Float, rt_additive_emis, 1.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: emission of additive sprites (explosions, fire)" )
CVARD( Float, rt_sky, 25.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: sky brightness (its light on the scene)" )
CVARD( Float, rt_emis_mult, 1.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: how much glowing surfaces light their surroundings" )
CVARD( Bool, rt_emis_lights, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: glowing surfaces cast real light" )
CVARD( Float, rt_emis_light_scale, 600.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: brightness of the light glowing surfaces cast" )
CVARD( Bool, rt_emis_follow_shade, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: glowing textures go out with their room's light switch" )
CVARD( Float, rt_emis_gi, 0.25f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: with rt_emis_lights, how much bounce light a glowing texture still gives itself" )
CVARD( Float, rt_ambient, 0.04f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: faint glow of every surface by its sector's light level, so unlit places are dark rather than black" )
CVARD( Bool, rt_polish, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: textures with smooth texels (roughness maps) mirror the room" )
CVARD( Float, rt_world_pbr, 1.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: 0..1, how much of the walls' and floors' roughness and metallic maps to use" )
CVARD( Float, rt_sprite_pbr, 0.35f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: 0..1, the same for sprites" )
CVARD( Float, rt_metallic_max, 0.75f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: ceiling on metalness (a metal has no diffuse light of its own)" )
CVARD( Float, rt_decal_offset, 0.03f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: metres a wall or floor sprite is moved off the surface it is on" )
// how fast the denoiser drops its history when the lighting changes: lower = calmer
// under flickering lights and explosions, but lighting changes trail a little
CVARD( Float, rt_denoise_sens_direct, 0.5f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: 0..1, denoiser reaction to direct light changes (lower = fewer splotches, more lag)" )
CVARD( Float, rt_denoise_sens_indirect, 0.2f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: 0..1, the same for bounce light" )
CVARD( Float, rt_min_roughness, 0.7f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: 0..1, no surface is glossier than this (lower = sharper highlights, more noise)" )
CVARD( Float, rt_denoise_sens_spec, 0.3f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: 0..1, the same for reflections" )
CVARD( Bool, rt_muzzleflash, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: shooting lights the room" )
CVARD( Float, rt_muzzleflash_intensity, 250.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: muzzle flash brightness" )
CVARD( Color, rt_muzzleflash_color, 0xffa860, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: muzzle flash colour" )
CVARD( Float, rt_emis_boost, 50.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: RTGL1's emission map boost (bounce light from glowing surfaces)" )
CVARD( Float, rt_emis_screen, 3.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: on-screen brightness of glowing surfaces" )
CVARD( Int, rt_texture_filter, 1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: 0 nearest (pixelated), 1 linear" )
CVARD( Float, rt_ev100_min, 2.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: eye adaptation, darkest exposure (lower = dark scenes brighter)" )
CVARD( Float, rt_ev100_max, 7.7f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: eye adaptation, brightest exposure" )
CVARD( Float, rt_white_point, 3.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: tonemapping white point" )
CVARD( Float, rt_contrast, 0.1f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: tonemapping contrast" )
CVARD( Float, rt_sky_saturation, 1.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: sky saturation" )
CVARD( Int, rt_bounces, 2, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: indirect light bounces" )
CVARD( Bool, rt_volumetrics, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: volumetric light (haze, light shafts) and smoke" )
CVARD( Float, rt_volume_far, 60.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: metres the volume reaches (the reach of haze and smoke)" )
CVARD( Float, rt_volume_scatter, 1.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: how thick the air's haze is (per metre)" )
CVARD( Float, rt_volume_ambient, 0.2f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: the haze's own faint light" )
CVARD( Float, rt_volume_lintensity, 1.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: how strongly lights show in the haze" )
CVARD( Bool, rt_volume_all_lights, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: every light shows in the haze, not only the moon (costly)" )
CVARD( Float, rt_znear, 0.05f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: camera near plane, metres" )
CVARD( Float, rt_zfar, 2048.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: camera far plane, metres" )
CVARD( Bool, rt_flashlight, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: a light at the camera" )
CVARD( Float, rt_flashlight_intensity, 156.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: flashlight brightness" )
CVARD( Float, rt_flashlight_angle, 46.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: flashlight cone, degrees" )
CVARD( Float, rt_flashlight_radius, 0.02f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: flashlight source size, metres" )
CVARD( Color, rt_flashlight_color, 0xffbe82, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: flashlight colour" )
CVARD( Bool, rt_precache, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: give RTGL1 the level's textures when it loads, not when first seen" )
CVARD( Int, rt_precache_budget, 2000, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: at most this many textures precached per session (RTGL1 holds 4096)" )
CVARD( Int, rt_config_version, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: settings version (old defaults are updated once)" )
CVARD( Int, rt_debug_uniqueid, 0, 0, "RT: print draws the scene code did not give an id" )
CVARD( Bool, rt_debug_stats, false, 0, "RT: print upload counts every few seconds" )

namespace rtx
{

void RT_Check( RgResult r, const char* what )
{
    if( r != RG_RESULT_SUCCESS )
    {
        static int printed = 0;
        if( printed < 50 )
        {
            printed++;
            const char* desc = rt.rgUtilGetResultDescription ? rt.rgUtilGetResultDescription( r ) : "";
            Printf( TEXTCOLOR_RED "RT: %s failed: %d %s\n", what, int( r ), desc ? desc : "" );
        }
    }
}

} // namespace rtx

using namespace rtx;

namespace
{

void RT_Print( const char* pMessage, RgMessageSeverityFlags flags, void* pUserData )
{
    if( !pMessage )
    {
        return;
    }
    if( flags & RG_MESSAGE_SEVERITY_ERROR )
    {
        Printf( TEXTCOLOR_RED "RTGL1 error: %s\n", pMessage );
    }
    else if( flags & RG_MESSAGE_SEVERITY_WARNING )
    {
        Printf( TEXTCOLOR_ORANGE "RTGL1: %s\n", pMessage );
    }
    else
    {
        Printf( "RTGL1: %s\n", pMessage );
    }
}

std::string RT_OverrideFolder()
{
    if( const char* env = getenv( "RAZE_RT_DIR" ); env && env[ 0 ] )
    {
        std::string s = env;
        while( s.size() > 1 && ( s.back() == '/' || s.back() == '\\' ) )
        {
            s.pop_back();
        }
        return s;
    }
    return "rt";
}

std::string g_overrideFolder;

uint32_t g_frameCounter = 0;

auto RT_GetCurrentTime() -> double
{
    using namespace std::chrono;
    static const auto start = steady_clock::now();
    return duration< double >( steady_clock::now() - start ).count();
}

auto RT_MakeUpRightForwardVectors( float yawDeg, float pitchDeg, float rollDeg )
    -> std::tuple< RgFloat3D, RgFloat3D, RgFloat3D >
{
    auto view = VSMatrix{ 1 };
    view.rotate( yawDeg, 0, 0, 1 );   // around up
    view.rotate( pitchDeg, 1, 0, 0 ); // around right
    view.rotate( rollDeg, 0, 1, 0 );  // around forward
    const float* v = view.get();

    auto right   = RgFloat3D{ -v[ 0 ], -v[ 1 ], -v[ 2 ] };
    auto forward = RgFloat3D{ -v[ 4 ], -v[ 5 ], -v[ 6 ] };
    auto up      = RgFloat3D{ v[ 8 ], v[ 9 ], v[ 10 ] };
    return { up, right, forward };
}

static RgExtent2D g_lastOutputSize{};
static int        g_lastUpscaleMode = 0;

void RT_FillResolution( RgStartFrameRenderResolutionParams& dst, RgExtent2D winsize )
{
    g_lastOutputSize = winsize;
    dst.frameGeneration   = RG_FRAME_GENERATION_MODE_OFF;
    dst.preferDxgiPresent = false;
    dst.pixelizedRenderSizeEnable = false;
    dst.rayReconstruction = 0;
    dst.denoiser          = RG_DENOISER_DEFAULT;

    const double scale = std::clamp( double( *rt_renderscale ), 0.2, 1.0 );
    dst.customRenderSize = { std::max( 1u, uint32_t( winsize.width * scale ) ),
                             std::max( 1u, uint32_t( winsize.height * scale ) ) };

    const bool metalfx = rt.rgUtilIsUpscaleTechniqueAvailable &&
                         rt.rgUtilIsUpscaleTechniqueAvailable(
                             RG_RENDER_UPSCALE_TECHNIQUE_AMD_FSR2, RG_FRAME_GENERATION_MODE_OFF, nullptr );

    int mode = metalfx ? int( *rt_upscale ) : 0;
    switch( mode )
    {
        case 1: dst.resolutionMode = RG_RENDER_RESOLUTION_MODE_QUALITY; break;
        case 2: dst.resolutionMode = RG_RENDER_RESOLUTION_MODE_BALANCED; break;
        case 3: dst.resolutionMode = RG_RENDER_RESOLUTION_MODE_PERFORMANCE; break;
        case 4: dst.resolutionMode = RG_RENDER_RESOLUTION_MODE_ULTRA_PERFORMANCE; break;
        case 5: dst.resolutionMode = RG_RENDER_RESOLUTION_MODE_CUSTOM; break;
        default: mode = 0; break;
    }
    g_lastUpscaleMode = mode;

    if( mode == 0 )
    {
        // no upscaler: render at rt_renderscale (1 = native) and stretch
        dst.upscaleTechnique = RG_RENDER_UPSCALE_TECHNIQUE_LINEAR;
        dst.resolutionMode   = RG_RENDER_RESOLUTION_MODE_CUSTOM;
    }
    else
    {
        // on Metal, the AMD FSR2 slot is the MetalFX temporal scaler
        dst.upscaleTechnique = RG_RENDER_UPSCALE_TECHNIQUE_AMD_FSR2;
        if( *rt_metalfx_denoiser && ( rt.rgUtilGetSupportedFeatures() & RG_FEATURE_METALFX_DENOISER ) )
        {
            dst.denoiser          = RG_DENOISER_METALFX;
            dst.rayReconstruction = 1;
        }
    }

    switch( int( *rt_sharpen ) )
    {
        case 1: dst.sharpenTechnique = RG_RENDER_SHARPEN_TECHNIQUE_NAIVE; break;
        case 2: dst.sharpenTechnique = RG_RENDER_SHARPEN_TECHNIQUE_AMD_CAS; break;
        case 3: dst.sharpenTechnique = RG_RENDER_SHARPEN_TECHNIQUE_NONE; break;
        default:
            dst.sharpenTechnique = mode != 0 ? RG_RENDER_SHARPEN_TECHNIQUE_AMD_CAS
                                             : RG_RENDER_SHARPEN_TECHNIQUE_NONE;
            break;
    }
}

} // namespace

//
// Instance
//

void RT_CreateRendererInstance( void* pMetalLayer )
{
    rt = RgInterface{};

    g_overrideFolder = RT_OverrideFolder();

#if defined( __APPLE__ )
    auto metalInfo = RgMetalSurfaceCreateInfo{
        .pLayer = static_cast< const CAMetalLayer* >( pMetalLayer ),
    };
#endif

    const bool verbose = Args->CheckParm( "-rtdebug" ) != 0;

    auto info = RgInstanceCreateInfo{
        .sType             = RG_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pNext             = nullptr,
        .version           = RG_RTGL_VERSION_API,
        .sizeOfRgInterface = sizeof( RgInterface ),
        .pAppName          = "Raze",
        .pAppGUID          = "5b0b2a4e-6f7e-4b8e-9d3a-2a0c6f1e8d71",
#if defined( __APPLE__ )
        .pMetalSurfaceCreateInfo = &metalInfo,
#endif
        .pOverrideFolderPath = g_overrideFolder.c_str(),
        .pfnPrint            = RT_Print,
        .pUserPrintData      = nullptr,
        .allowedMessages     = verbose ? RgMessageSeverityFlags{ RG_MESSAGE_SEVERITY_VERBOSE | RG_MESSAGE_SEVERITY_INFO |
                                                                 RG_MESSAGE_SEVERITY_WARNING | RG_MESSAGE_SEVERITY_ERROR }
                                       : RgMessageSeverityFlags{ RG_MESSAGE_SEVERITY_WARNING | RG_MESSAGE_SEVERITY_ERROR },

        .primaryRaysMaxAlbedoLayers          = 1,
        .indirectIlluminationMaxAlbedoLayers = 1,

        .replacementsMaxVertexCount = 4 * 1024 * 1024,
        .dynamicMaxVertexCount      = 2 * 1024 * 1024,

        .rayCullBackFacingTriangles = 0,
        .allowTexCoordLayer1        = false,
        .allowTexCoordLayer2        = false,
        .allowTexCoordLayer3        = false,
        .lightmapTexCoordLayerIndex = 0,

        .rasterizedMaxVertexCount   = 1 << 20,
        .rasterizedMaxIndexCount    = 1 << 21,
        .rasterizedVertexColorGamma = true,

        .rasterizedSkyCubemapSize = 256,

        .textureSamplerForceMinificationFilterLinear = true,
        .textureSamplerForceNormalMapFilterLinear    = true,

        .pbrTextureSwizzling = RG_TEXTURE_SWIZZLING_NULL_ROUGHNESS_METALLIC,

        .effectWipeIsUsed = false,

        .worldUp      = { 0, 0, 1 },
        .worldForward = { 0, 1, 0 },
        .worldScale   = 1.0f,

        .importedLightIntensityScaleDirectional = 1.0f / 50,
        .importedLightIntensityScaleSphere      = 1.0f / 500,
        .importedLightIntensityScaleSpot        = 1.0f / 500,
    };

    Printf( "RT: loading the renderer from %s/bin\n", g_overrideFolder.c_str() );

    RgResult r = rgLoadLibraryAndCreate( &info, false, nullptr, &rt, nullptr );
    if( r != RG_RESULT_SUCCESS )
    {
        std::string msg;
        switch( r )
        {
            case RG_RESULT_CANT_FIND_DYNAMIC_LIBRARY:
            {
                const char* err = dlerror();
                msg = "Can't load '" + g_overrideFolder + "/bin/libRTGL1.dylib': " + ( err ? err : "unknown error" );
                break;
            }
            case RG_RESULT_CANT_FIND_ENTRY_FUNCTION_IN_DYNAMIC_LIBRARY:
                msg = "rgCreateInstance not found in libRTGL1";
                break;
            case RG_RESULT_WRONG_FUNCTION_ARGUMENT:
                msg = "RTGL1 API version mismatch (expected " RG_RTGL_VERSION_API ")";
                break;
            default: msg = "RTGL1 failed to start, RgResult " + std::to_string( int( r ) ); break;
        }
        rt = RgInterface{};
        I_FatalError( "Ray tracing renderer: %s\n(set vid_raytracing 0 in the ini to use OpenGL)", msg.c_str() );
    }

    g_rtActive = true;
    Printf( "RT: RTGL1 started\n" );

    // settings saved by earlier versions with defaults that have changed since
    if( *rt_config_version < 2 )
    {
        rt_flashlight      = false;
        rt_sky             = 25.f;
        rt_fullbright_emis = 1.f;
        rt_emis_boost      = 50.f;
        rt_config_version  = 2;
    }
    if( *rt_config_version < 3 )
    {
        rt_sector_light_scale->ResetToDefault();
        rt_emis_light_scale->ResetToDefault();
        rt_config_version = 3;
    }
    if( *rt_config_version < 4 )
    {
        rt_metallic_max->ResetToDefault();
        rt_config_version = 4;
    }
    if( *rt_config_version < 5 )
    {
        rt_emis_light_scale->ResetToDefault();
        rt_config_version = 5;
    }
    if( *rt_config_version < 6 )
    {
        rt_denoise_sens_direct->ResetToDefault();
        rt_denoise_sens_indirect->ResetToDefault();
        rt_min_roughness->ResetToDefault();
        rt_volumetrics->ResetToDefault();
        rt_config_version = 6;
    }
}

void RT_DestroyRendererInstance()
{
    g_rtActive = false;
    if( !rt.rgDestroyInstance )
    {
        return;
    }
    rt.rgDestroyInstance();
    rt = RgInterface{};
}

SystemBaseFrameBuffer* RT_CreateFrameBuffer( void* hMonitor, bool fullscreen )
{
    return new RTFrameBuffer{ hMonitor, fullscreen };
}

//
// Frame buffer
//

RTFrameBuffer::RTFrameBuffer( void* hMonitor, bool fullscreen )
    : SystemBaseFrameBuffer( hMonitor, fullscreen )
{
}

RTFrameBuffer::~RTFrameBuffer()
{
    delete mVertexData;
    delete mSkyData;
    delete mViewpoints;
    delete mLights;
    delete mBones;
    mVertexData = nullptr;
    mSkyData    = nullptr;
    mViewpoints = nullptr;
    mLights     = nullptr;
    mBones      = nullptr;
    delete m_state;
}

void RTFrameBuffer::InitializeState()
{
    delete m_state;
    m_state      = new RTRenderState{ this };
    vendorstring = "RTGL1";
    hwcaps       = 0;
    glslversion  = 4.5f;
    uniformblockalignment = 256;
    maxuniformblock       = 65536;

    mVertexData = new FFlatVertexBuffer( GetWidth(), GetHeight(), mPipelineNbr );
    mSkyData    = new FSkyVertexBuffer;
    mViewpoints = new HWViewpointBuffer( mPipelineNbr );
    mLights     = new FLightBuffer( mPipelineNbr );
    mBones      = new BoneBuffer( mPipelineNbr );
}

FRenderState* RTFrameBuffer::RenderState()
{
    return m_state;
}
IVertexBuffer* RTFrameBuffer::CreateVertexBuffer()
{
    return new RTVertexBuffer{};
}
IIndexBuffer* RTFrameBuffer::CreateIndexBuffer()
{
    return new RTIndexBuffer{};
}
IDataBuffer* RTFrameBuffer::CreateDataBuffer( int bindingpoint, bool ssbo, bool needsresize )
{
    return new RTDataBuffer{};
}
IHardwareTexture* RTFrameBuffer::CreateHardwareTexture( int numchannels )
{
    return new RTHardwareTexture{};
}

void RTFrameBuffer::PrecacheMaterial( FMaterial* mat, int translation )
{
    static int precached = 0;
    if( !mat || !*rt_precache || precached >= *rt_precache_budget )
    {
        return;
    }
    FGameTexture* gametex = mat->Source();
    if( !gametex || gametex->GetUseType() == ETextureType::SWCanvas )
    {
        return;
    }
    FTexture* base = gametex->GetTexture();
    if( !base )
    {
        return;
    }
    if( auto hwtex = static_cast< RTHardwareTexture* >( base->GetHardwareTexture( translation, mat->GetScaleFlags() ) ) )
    {
        if( !hwtex->GetRTName() )
        {
            precached++;
        }
        hwtex->CreateIfWasnt( *gametex, translation, mat->GetScaleFlags(), LegacyRenderStyles[ STYLE_Normal ] );
    }
}

TArray< uint8_t > RTFrameBuffer::GetScreenshotBuffer( int& pitch, ESSType& color_type, float& gamma )
{
    // RTGL1 presents straight to the window's layer and has no read back
    pitch      = 0;
    color_type = SS_RGB;
    gamma      = 1.0f;
    return {};
}

void RTFrameBuffer::Draw2D()
{
    ::Draw2D( twod, *m_state );
}

void RTFrameBuffer::BeginFrame()
{
    SetViewportRects( nullptr );
    if( mViewpoints )
    {
        mViewpoints->Clear();
    }
    // Raze calls this twice per frame (once more right before the 3D view);
    // the RTGL1 frame starts on the first call
    if( !m_frameStarted )
    {
        RT_StartFrame();
    }
    Super::BeginFrame();
}

void RTFrameBuffer::Update()
{
    if( !m_frameStarted )
    {
        // frames that never called BeginFrame (screen wipes, start screens)
        SetViewportRects( nullptr );
        RT_StartFrame();
    }

    Draw2D();
    twod->Clear();

    RT_DrawFrame();
    Super::Update();
}

void RTFrameBuffer::RT_StartFrame()
{
    m_state->RT_BeginFrame();
    m_wassky         = false;
    m_cameraUploaded = false;

    auto resolution = RgStartFrameRenderResolutionParams{
        .sType = RG_STRUCTURE_TYPE_START_FRAME_RENDER_RESOLUTION_PARAMS,
        .pNext = nullptr,
    };
    RT_FillResolution( resolution, RgExtent2D{ uint32_t( std::max( 1, GetClientWidth() ) ),
                                               uint32_t( std::max( 1, GetClientHeight() ) ) } );

    auto info = RgStartFrameInfo{
        .sType                    = RG_STRUCTURE_TYPE_START_FRAME_INFO,
        .pNext                    = &resolution,
        .pMapName                 = nullptr,
        .ignoreExternalGeometry   = true,
        .vsync                    = *rt_vsync ? 1u : 0u,
        .hdr                      = false,
        .allowMapAutoExport       = false,
        .lightmapScreenCoverage   = std::clamp( float( *rt_classic ), 0.f, 1.f ),
        .lightstyleValuesCount    = 0,
        .pLightstyleValues8       = nullptr,
        .pResultStaticSceneStatus = nullptr,
        .staticSceneAnimationTime = 0,
    };

    RgResult r = rt.rgStartFrame( &info );
    RG_CHECK( r );
    m_frameStarted = ( r == RG_RESULT_SUCCESS );
}

void RTFrameBuffer::RT_SetMainCamera( const DVector3& pos, const FRotator& hwangles, float fovDeg, float ratio, float fovratio )
{
    if( !m_frameStarted )
    {
        return;
    }

    // Raze's HW yaw is 270 - (Doom-style yaw); gzdoom-rt rotates by Doom yaw - 270
    const auto [ up, right, forward ] = RT_MakeUpRightForwardVectors(
        float( -hwangles.Yaw.Degrees() ), float( hwangles.Pitch.Degrees() ), float( hwangles.Roll.Degrees() ) );

    const float fovy = float( 2.0 * std::atan( std::tan( double( fovDeg ) * M_PI / 360.0 ) / double( fovratio ) ) );

    auto readback = RgCameraInfoReadbackEXT{
        .sType = RG_STRUCTURE_TYPE_CAMERA_INFO_READ_BACK_EXT,
        .pNext = nullptr,
    };

    auto info = RgCameraInfo{
        .sType       = RG_STRUCTURE_TYPE_CAMERA_INFO,
        .pNext       = &readback,
        .flags       = 0,
        .position    = { float( pos.X ) * ONEGAMEUNIT_IN_METERS,
                         float( pos.Y ) * ONEGAMEUNIT_IN_METERS,
                         float( pos.Z ) * ONEGAMEUNIT_IN_METERS },
        .up          = up,
        .right       = right,
        .fovYRadians = fovy,
        .aspect      = ratio,
        .cameraNear  = *rt_znear,
        .cameraFar   = *rt_zfar,
        .pView       = nullptr,
    };

    RgResult r = rt.rgUploadCamera( &info );
    RG_CHECK( r );
    if( r != RG_RESULT_SUCCESS )
    {
        return;
    }

    m_state->RT_SetMainCameraInverse( readback.viewInverse, readback.projectionInverse );

    m_cameraUploaded = true;
    m_camPos         = info.position;
    m_camUp          = up;
    m_camRight       = right;
    m_camForward     = forward;
}

void RTFrameBuffer::RT_UploadLights()
{
    if( !m_cameraUploaded )
    {
        return;
    }

    RT_UploadSceneLights( g_frameCounter++ );

    if( *rt_flashlight && *rt_flashlight_intensity > 0.f )
    {
        // slightly below and to the right of the eyes, pointing where they look
        const float down = 0.25f, side = 0.15f;
        RgFloat3D   p    = {
            m_camPos.data[ 0 ] - m_camUp.data[ 0 ] * down + m_camRight.data[ 0 ] * side,
            m_camPos.data[ 1 ] - m_camUp.data[ 1 ] * down + m_camRight.data[ 1 ] * side,
            m_camPos.data[ 2 ] - m_camUp.data[ 2 ] * down + m_camRight.data[ 2 ] * side,
        };

        const uint32_t c = *rt_flashlight_color;

        auto spot = RgLightSpotEXT{
            .sType      = RG_STRUCTURE_TYPE_LIGHT_SPOT_EXT,
            .pNext      = nullptr,
            .color      = rt.rgUtilPackColorByte4D( RPART( c ), GPART( c ), BPART( c ), 255 ),
            .intensity  = *rt_flashlight_intensity,
            .position   = p,
            .direction  = m_camForward,
            .radius     = *rt_flashlight_radius,
            .angleOuter = float( double( *rt_flashlight_angle ) * M_PI / 180.0 ),
            .angleInner = 0,
        };

        auto light = RgLightInfo{
            .sType        = RG_STRUCTURE_TYPE_LIGHT_INFO,
            .pNext        = &spot,
            .uniqueID     = FlashlightLightId,
            .isExportable = false,
        };

        RG_CHECK( rt.rgUploadLight( &light ) );
    }

    // the game says how bright its weapon flash is (Duke: the player's visibility dip)
    if( *rt_muzzleflash && g_rtMuzzleFlash > 0.01f && *rt_muzzleflash_intensity > 0.f )
    {
        // ahead of the gun, a little low; near enough not to end up behind a wall
        const float fwd = 0.45f, down = 0.15f;
        RgFloat3D   p   = {
            m_camPos.data[ 0 ] + m_camForward.data[ 0 ] * fwd - m_camUp.data[ 0 ] * down,
            m_camPos.data[ 1 ] + m_camForward.data[ 1 ] * fwd - m_camUp.data[ 1 ] * down,
            m_camPos.data[ 2 ] + m_camForward.data[ 2 ] * fwd - m_camUp.data[ 2 ] * down,
        };
        const uint32_t c = *rt_muzzleflash_color;

        auto sphere = RgLightSphericalEXT{
            .sType     = RG_STRUCTURE_TYPE_LIGHT_SPHERICAL_EXT,
            .pNext     = nullptr,
            .color     = rt.rgUtilPackColorByte4D( RPART( c ), GPART( c ), BPART( c ), 255 ),
            .intensity = *rt_muzzleflash_intensity * std::min( g_rtMuzzleFlash, 1.f ),
            .position  = p,
            .radius    = 0.05f,
        };
        auto light = RgLightInfo{
            .sType        = RG_STRUCTURE_TYPE_LIGHT_INFO,
            .pNext        = &sphere,
            .uniqueID     = MuzzleFlashLightId,
            .isExportable = false,
        };
        RG_CHECK( rt.rgUploadLight( &light ) );
    }
}

void RTFrameBuffer::RT_DrawFrame()
{
    if( !m_frameStarted )
    {
        return;
    }

    RT_UploadLights();

    // RTGL1 uses its defaults only for a struct that is not linked at all, so the ones
    // linked here repeat RTGL1's defaults (DrawFrameInfo.h) and change only what is ours
    auto illum = RgDrawFrameIlluminationParams{
        .sType                                       = RG_STRUCTURE_TYPE_DRAW_FRAME_ILLUMINATION_PARAMS,
        .pNext                                       = nullptr,
        .maxBounceShadows                            = 2,
        .indirectBounces                             = uint32_t( std::clamp( int( *rt_bounces ), 1, 4 ) ),
        .indirectLegacyBounceWeight                  = true,
        .cellWorldSize                               = 1.0f,
        .directDiffuseSensitivityToChange            = std::clamp( float( *rt_denoise_sens_direct ), 0.f, 1.f ),
        .indirectDiffuseSensitivityToChange          = std::clamp( float( *rt_denoise_sens_indirect ), 0.f, 1.f ),
        .specularSensitivityToChange                 = std::clamp( float( *rt_denoise_sens_spec ), 0.f, 1.f ),
        .polygonalLightSpotlightFactor               = 2.0f,
        // the player's own body (seen in mirrors) does not shadow his muzzle flash
        .lightUniqueIdIgnoreFirstPersonViewerShadows = &MuzzleFlashLightId,
        .enableRrTemporalPrefilter                   = false,
        .rrFireflyThreshold                          = 0.0f,
        .rrFireflyMinLum                             = 0.01f,
        .restirBlueNoise                             = true,
        .shadowSamples                               = 1,
        .debugRestirM                                = 0,
        .debugVisibility                             = 0,
        .restirTemporalJitter                        = 2.0f,
        .rrSpecularHitDistance                       = true,
        .directSamples                               = 1,
        .indirectSamples                             = 1,
        .restirInitialSamples                        = 8,
        .restirSpatialSamples                        = 8,
        .restirSpatialRadius                         = 30.0f,
        .restirTemporalMCap                          = 20,
        .rrGuideMin                                  = 0.01f,
        .rrGuideMode                                 = 1,
        .restirIndirAntilag                          = 1,
        .rrPreExposure                               = true,
        .rrPreExposureDebug                          = false,
    };

    auto sky = RgDrawFrameSkyParams{
        .sType                       = RG_STRUCTURE_TYPE_DRAW_FRAME_SKY_PARAMS,
        .pNext                       = &illum,
        .skyType                     = m_wassky ? RG_SKY_TYPE_RASTERIZED_GEOMETRY : RG_SKY_TYPE_COLOR,
        .skyColorDefault             = { 0.02f, 0.02f, 0.05f },
        .skyColorMultiplier          = *rt_sky,
        .skyColorSaturation          = *rt_sky_saturation,
        .skyViewerPosition           = { 0, 0, 0 },
        .pSkyCubemapTextureName      = nullptr,
        .skyCubemapRotationTransform = {},
        .sunRequireSky               = 0.0f,
        .sunLeakDebug                = 0.0f,
        .sunLeakDebugMul             = 1.0f,
        .sunSkyProbeMaxDist          = 10000.0f,
        .sunSplit                    = 0.0f,
    };

    auto textures = RgDrawFrameTexturesParams{
        .sType                  = RG_STRUCTURE_TYPE_DRAW_FRAME_TEXTURES_PARAMS,
        .pNext                  = &sky,
        .dynamicSamplerFilter   = *rt_texture_filter ? RG_SAMPLER_FILTER_LINEAR : RG_SAMPLER_FILTER_NEAREST,
        .mipLodBiasOffset       = 0.0f,
        .normalMapStrength      = 1.0f,
        .emissionMapBoost       = std::max( 0.f, float( *rt_emis_boost ) ),
        .emissionMaxScreenColor = std::max( 0.f, float( *rt_emis_screen ) ),
        // a floor under every surface's roughness: Duke-RT's maps are glossy almost
        // everywhere (0.3 on average), and a glossy surface's reflection is one noisy
        // sample per pixel -- the crawling on shiny ceilings and floors. The mirrors of
        // polished surfaces are separate (masked by the map, before this floor)
        .minRoughness           = std::clamp( float( *rt_min_roughness ), 0.f, 1.f ),
        .heightMapDepth         = 0.02f,
        // the material pass: RTGL1's own defaults for these leave every surface a
        // plain matte dielectric (worldPbr 0), so they must be given
        .forceNonMetallic       = false,
        .metallicMax            = std::clamp( float( *rt_metallic_max ), 0.f, 1.f ),
        .metallicRoughCut       = 0.7f,
        .metallicRoughBand      = 0.3f,
        .spritePbr              = std::clamp( float( *rt_sprite_pbr ), 0.f, 1.f ),
        .spriteMetallicMax      = 0.9f,
        .spriteRoughMin         = 0.0f,
        .spriteNormalStrength   = 1.0f,
        .worldPbr               = std::clamp( float( *rt_world_pbr ), 0.f, 1.f ),
        .albedoFilterMode       = 0,
    };

    auto tonemap = RgDrawFrameTonemappingParams{
        .sType                = RG_STRUCTURE_TYPE_DRAW_FRAME_TONEMAPPING_PARAMS,
        .pNext                = &textures,
        .disableEyeAdaptation = false,
        .ev100Min             = *rt_ev100_min,
        .ev100Max             = std::max( float( *rt_ev100_min ) + 0.1f, float( *rt_ev100_max ) ),
        .luminanceWhitePoint  = std::max( 0.1f, float( *rt_white_point ) ),
        .saturation           = { 0, 0, 0 },
        .crosstalk            = { 1, 1, 1 },
        .contrast             = *rt_contrast,
        .hdrBrightness        = 1.0f,
        .hdrContrast          = 0.1f,
        .hdrSaturation        = { 0.25f, 0.25f, 0.25f },
    };

    void* chain = &tonemap;

    // smoke: the puffs, packed for the froxel volume (rt_smoke.cpp)
    const float volFar = std::clamp( float( *rt_volume_far ), 5.f, 500.f );
    static RgFloat4D s_puffs[ RG_MAX_SMOKE_PUFFS ], s_albden[ RG_MAX_SMOKE_PUFFS ], s_shape[ RG_MAX_SMOKE_PUFFS ];
    const uint32_t   puffCount = ( *rt_volumetrics && m_cameraUploaded )
                                     ? RT_UpdateSmoke( m_camPos, m_camForward, m_camUp, m_camRight, volFar, s_puffs, s_albden, s_shape )
                                     : 0;

    auto smoke = RgDrawFrameSmokeParams{
        .sType          = RG_STRUCTURE_TYPE_DRAW_FRAME_SMOKE_PARAMS,
        .pNext          = chain,
        .puffCount      = puffCount,
        .pPuffs         = s_puffs,
        .pAlbedoDensity = s_albden,
        .pShape         = s_shape,
        // Doom 64 RT's values
        .lightNearFade  = 0.5f,
        .illumBlend     = 0.45f,
        .allLights      = bool( *rt_smoke_illum ),
        .lightFarFade   = 12.f,
        .samplesPerCell = 8,
        .maxLight       = 6.f,
        .debugMode      = 0,
        .stylize        = 0.f,
        .stylizeSteps   = 3,
        .stylizeGrid    = 0.f,
        .selfAmbient    = 0.10f,
        .tintBias       = 0.8f,
        .absorb         = 1.f,
    };
    chain = &smoke;

    // The haze: Doom 64 RT's volume. Its density is given per metre (scaled by the
    // reach), so a longer reach does not thin it. The moon (the one directional
    // light) is what draws shafts through it; rt_volume_all_lights makes every
    // light do so.
    const float scatter = std::max( 0.f, float( *rt_volume_scatter ) ) * ( volFar / 30.f );
    auto volumetric = RgDrawFrameVolumetricParams{
        .sType                   = RG_STRUCTURE_TYPE_DRAW_FRAME_VOLUMETRIC_PARAMS,
        .pNext                   = chain,
        .enable                  = *rt_volumetrics,
        .maxHistoryLength        = puffCount > 0 ? 20.f : 8.f,
        .useSimpleDepthBased     = false,
        .volumetricFar           = volFar,
        .ambientColor            = { float( *rt_volume_ambient ), float( *rt_volume_ambient ), float( *rt_volume_ambient ) },
        .scaterring              = scatter,
        .assymetry               = 0.5f,
        .useIlluminationVolume   = false,
        .fallbackSourceColor     = { 0, 0, 0 },
        .fallbackSourceDirection = { 0, -1, 0 },
        .lightMultiplier         = std::max( 0.f, float( *rt_volume_lintensity ) ),
        .allowTintUnderwater     = false,
        .underwaterColor         = { 0, 0, 0 },
        .illuminateFromAllLights = bool( *rt_volume_all_lights ),
        .mediaColor              = { 1.0f, 1.0f, 1.0f },
        .mediaColorFar           = { 1.0f, 1.0f, 1.0f },
        .farScattering           = scatter,
        .densityCurve            = 1.0f,
        .occludeEmission         = true,
        .ditherRadius            = 5.0f,
        .ditherRadiusZ           = 1.0f,
        .spatialBlur             = 1.0f,
        .lightNearFade           = 0.0f,
        .depthGate               = 1.0f,
        .depthGateBias           = 0.0f,
        .depthGateFeather        = 1.0f,
        .depthGateTaps           = 5,
    };
    chain = &volumetric;

    auto info = RgDrawFrameInfo{
        .sType                    = RG_STRUCTURE_TYPE_DRAW_FRAME_INFO,
        .pNext                    = chain,
        .rayLength                = std::clamp( float( *rt_zfar ), 10.f, 10000.f ),
        .disableRayTracedGeometry = !m_cameraUploaded,
        .disableRasterization     = false,
        .presentPrevFrame         = false,
        .resetHistory             = false,
        .currentTime              = RT_GetCurrentTime(),
    };

    RG_CHECK( rt.rgDrawFrame( &info ) );
    m_frameStarted = false;

    if( *rt_debug_stats )
    {
        static double last = 0;
        const double  now  = RT_GetCurrentTime();
        if( now - last > 3.0 )
        {
            last = now;
            Printf( "RT: %u primitives uploaded, %u dropped, sky %d, camera %d\n",
                    m_state->m_drawsThisFrame,
                    m_state->m_droppedThisFrame,
                    int( m_wassky ),
                    int( m_cameraUploaded ) );
        }
    }
}

//
// Viewpoint matrices: the hardware renderer binds a range of the viewpoint buffer
// before drawing; that is how the 2D projection and the sky's view reach us.
//

void RTDataBuffer::BindRange( FRenderState* state, size_t start, size_t length )
{
    auto hwstate = static_cast< RTRenderState* >( state );
    if( hwstate && hwstate->m_fb && screen && screen->mViewpoints && this == screen->mViewpoints->DataBuffer() )
    {
        const HWViewpointUniforms& vp = screen->mViewpoints->FetchViewpoint( start );
        hwstate->RT_SetMatrices( vp.mViewMatrix, vp.mProjectionMatrix );
    }
}

//
// Called by the scene code
//

void RT_SetMainCamera( const DVector3& pos, const FRotator& hwangles, float fovDeg, float ratio, float fovratio )
{
    if( g_rtActive && screen )
    {
        static_cast< RTFrameBuffer* >( screen )->RT_SetMainCamera( pos, hwangles, fovDeg, ratio, fovratio );
    }
}

// The size the frame is made at (MetalFX upscales to it; macOS scales it to the
// screen). Fullscreen only, as in the Doom 64 port: vid_rt_fullscreen_height.
EXTERN_CVAR( Int, vid_rt_fullscreen_height )
CCMD( rt_resolution )
{
    if( argv.argc() > 1 )
    {
        vid_rt_fullscreen_height = std::max( 0, atoi( argv[ 1 ] ) );
    }
    static const double kTraced[] = { 0, 1 / 1.5, 1 / 1.7, 1 / 2.0, 1 / 3.0 };
    const double        f         = g_lastUpscaleMode >= 1 && g_lastUpscaleMode <= 4 ? kTraced[ g_lastUpscaleMode ]
                                                                                     : std::clamp( double( *rt_renderscale ), 0.2, 1.0 );
    Printf( "Output %ux%u (vid_rt_fullscreen_height %d: %s), traced about %ux%u (rt_upscale %d)\n",
            g_lastOutputSize.width,
            g_lastOutputSize.height,
            *vid_rt_fullscreen_height,
            *vid_rt_fullscreen_height > 0 ? "lower than the screen" : "the screen's own",
            unsigned( g_lastOutputSize.width * f ),
            unsigned( g_lastOutputSize.height * f ),
            g_lastUpscaleMode );
    if( argv.argc() <= 1 )
    {
        Printf( "rt_resolution <height>: make the frame that tall in fullscreen (e.g. 1440, 1080); 0 = the screen's\n" );
    }
    else
    {
        Printf( "(applies from the next frame, in fullscreen)\n" );
    }
}

CCMD( rt_info )
{
    if( !g_rtActive )
    {
        Printf( "RT is not active\n" );
        return;
    }
    const RgUtilMemoryUsage vram = rt.rgUtilRequestMemoryUsage();
    Printf( "RT: renderer folder %s\n", g_overrideFolder.c_str() );
    Printf( "RT: VRAM %d / %d MB\n",
            int( vram.vramUsed / ( 1024 * 1024 ) ),
            int( vram.vramTotal / ( 1024 * 1024 ) ) );
    Printf( "RT: MetalFX upscaler %s, denoiser %s\n",
            rt.rgUtilIsUpscaleTechniqueAvailable( RG_RENDER_UPSCALE_TECHNIQUE_AMD_FSR2, RG_FRAME_GENERATION_MODE_OFF, nullptr ) ? "available" : "not available",
            ( rt.rgUtilGetSupportedFeatures() & RG_FEATURE_METALFX_DENOISER ) ? "available" : "not available" );
}

const std::string& RT_GetOverrideFolder()
{
    return g_overrideFolder;
}
