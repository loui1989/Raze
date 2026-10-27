// Raze RT: every primitive the hardware renderer draws goes through
// RTRenderState::Draw / DrawIndexed and ends up in InternalDraw, which
// classifies it, resolves its texture and hands it to RTGL1.
//
// Adapted from gzdoom-rt's rt_draw.cpp / rt_buffers.h (Doom 64 RT), without the
// Doom 64 specific parts.

#include "rt_backend.h"

#include <chrono>
#include "texturemanager.h"
#include "tiletexture.h"

int RT_HightileTileOf( FGameTexture* tex, int* palnum ); // core/textures/hightile.cpp (global)


namespace rtx
{

//
// Vertex formats -> RgPrimitiveVertex
//

auto RTVertexBuffer::AccessFormatted( uint32_t first, uint32_t count ) -> std::span< const RgPrimitiveVertex >
{
    const size_t stride = std::visit(
        []< typename T >( const T& ) -> size_t {
            if constexpr( std::is_same_v< T, std::monostate > )
            {
                return 0;
            }
            else
            {
                return sizeof( T );
            }
        },
        m_vertextype );

    if( stride == 0 )
    {
        return {};
    }

    const auto   src       = AccessBuffer();
    const size_t available = src.size_bytes() / stride;
    if( size_t( first ) + size_t( count ) > available )
    {
        static bool warned = false;
        if( !warned )
        {
            warned = true;
            Printf( "RT: vertex range [%u, +%u) exceeds the buffer (%zu), draw skipped\n",
                    first,
                    count,
                    available );
        }
        return {};
    }

    if( size_t( first ) + count > m_formatted.size() )
    {
        const size_t target = size_t( first ) + count;
        m_formatted.reserve( target );

        static const RgNormalPacked32 fallbackNormal = rt.rgUtilPackNormal( 0, 0, 1 );

        auto unpack = []( uint32_t packed, int shift ) {
            // FModelVertex::SetNormal: 10 bit two's complement per component
            int v = int( ( packed >> shift ) & 1023 );
            if( v >= 512 )
            {
                v -= 1024;
            }
            return float( v ) / 512.0f;
        };

        std::visit(
            [ & ]< typename T >( const T& ) {
                if constexpr( !std::is_same_v< T, std::monostate > )
                {
                    for( size_t i = m_formatted.size(); i < target; i++ )
                    {
                        const auto* s = reinterpret_cast< const T* >( &src[ i * sizeof( T ) ] );

                        if constexpr( std::is_same_v< T, FSkyVertex > )
                        {
                            m_formatted.push_back( RgPrimitiveVertex{
                                .position     = { s->x * ONEGAMEUNIT_IN_METERS,
                                                  s->y * ONEGAMEUNIT_IN_METERS,
                                                  s->z * ONEGAMEUNIT_IN_METERS },
                                .normalPacked = fallbackNormal,
                                .texCoord     = { s->u, s->v },
                                .color        = rtcolor( s->color ),
                            } );
                        }
                        else if constexpr( std::is_same_v< T, FModelVertex > )
                        {
                            m_formatted.push_back( RgPrimitiveVertex{
                                .position     = { s->x * ONEGAMEUNIT_IN_METERS,
                                                  s->y * ONEGAMEUNIT_IN_METERS,
                                                  s->z * ONEGAMEUNIT_IN_METERS },
                                .normalPacked = rt.rgUtilPackNormal( unpack( s->packedNormal, 0 ),
                                                                     unpack( s->packedNormal, 10 ),
                                                                     unpack( s->packedNormal, 20 ) ),
                                .texCoord     = { s->u, s->v },
                                .color        = RG_PACKED_COLOR_WHITE,
                            } );
                        }
                        else if constexpr( std::is_same_v< T, FFlatVertex > )
                        {
                            // FFlatVertex keeps the map's x, y and the height in z;
                            // that is RTGL1's world space (z up), so no swizzle here
                            m_formatted.push_back( RgPrimitiveVertex{
                                .position     = { s->x * ONEGAMEUNIT_IN_METERS,
                                                  s->y * ONEGAMEUNIT_IN_METERS,
                                                  s->z * ONEGAMEUNIT_IN_METERS },
                                .normalPacked = fallbackNormal,
                                .texCoord     = { s->u, s->v },
                                .color        = RG_PACKED_COLOR_WHITE,
                            } );
                        }
                        else if constexpr( std::is_same_v< T, F2DDrawer::TwoDVertex > )
                        {
                            m_formatted.push_back( RgPrimitiveVertex{
                                .position     = { s->x, s->y, s->z },
                                .normalPacked = fallbackNormal,
                                .texCoord     = { s->u, s->v },
                                .color        = rtcolor_bgr_alphagamma( s->color0 ),
                            } );
                        }
                    }
                }
            },
            m_vertextype );
    }

    return std::span{ &m_formatted[ first ], count };
}

//
// Textures
//

namespace
{
bool IsIdentityRemap( const FRemapTable* remap )
{
    if( remap == nullptr || remap->Index == 0 )
    {
        return true;
    }
    for( int i = 0; i < 256; i++ )
    {
        if( remap->Remap[ i ] != i )
        {
            return false;
        }
    }
    // a base palette swap (night vision, underwater...) keeps the indices but not the colours
    for( int i = 1; i < 255; i++ )
    {
        const PalEntry a = remap->Palette[ i ];
        const PalEntry b = GPalette.BaseColors[ i ];
        if( a.r != b.r || a.g != b.g || a.b != b.b )
        {
            return false;
        }
    }
    return true;
}
} // namespace

auto RTHardwareTexture::TranslationSuffix( int translation ) -> std::string
{
    if( translation <= 0 )
    {
        return {};
    }
    if( IsLuminosityTranslation( translation ) )
    {
        return "_tl" + std::to_string( ( translation >> 16 ) & 0x3fff );
    }
    const FRemapTable* remap = GPalette.TranslationToTable( translation );
    if( IsIdentityRemap( remap ) )
    {
        return {};
    }
    // Index is a global, CRC-deduplicated index, so equal translations share a name.
    // Kept to file name safe characters: RTGL1 uses the name to look for material overrides.
    return "_tr" + std::to_string( remap->Index );
}

CVARD( Float, rt_voxel_glow, 0.4f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: how brightly voxel fires (fullbright actors' models) glow (1 = as glowing textures)" )
// A Build wall or floor is seen only from its own sector's side: a window that looks out
// over another sector does not show that sector's walls from behind (classic drew only
// the walls facing the viewer). The triangles are turned to face their sector and the
// path tracer culls those it meets from behind (only these: a sprite is seen from both
// sides). -1: the other way round (if walls vanish: a renderer with the other winding).
CVARD( Int, rt_one_sided, 1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: walls and floors are seen only from their own sector's side, as in the original (0 off, 1 on, -1 reversed)" )
// RgMeshPrimitiveFlags: the Metal backend's one-sided group (Raze RT's library)
constexpr uint32_t RT_PRIMITIVE_ONE_SIDED = 1u << 28;
CVARD( Bool, rt_voxel_alphatest, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: voxel models alpha tested, as their sprites are (slow; for comparison)" )

uint32_t g_rtTexCreates = 0;   // rt_debug_stats: textures given to the path tracer...
double   g_rtTexSeconds = 0;   // ...and the time it took (material files included)

auto RTHardwareTexture::MakeTextureName( FGameTexture& gametex, int translation ) -> std::string
{
    const std::string suffix = TranslationSuffix( translation );

    // an HD replacement of a tile (a texture pack): named after its tile, so the material
    // table, the window glass and the glow know it ("#01234~hd0": its own name, not the
    // tile's -- both can be in use; and its own material files, made from its texels)
    {
        int palnum = 0;
        if( const int tile = ::RT_HightileTileOf( &gametex, &palnum ); tile >= 0 )
        {
            char buf[ 32 ];
            snprintf( buf, sizeof( buf ), "#%05d~hd%d", tile, palnum );
            return buf + suffix;
        }
    }

    if( !gametex.GetName().IsEmpty() )
    {
        std::string name = gametex.GetName().GetChars();
        // Build tiles are named "#01234"; '#' is fine in a file name, keep it as is
        for( char& c : name )
        {
            if( c == '/' || c == '\\' || c == ':' )
            {
                c = '_';
            }
        }
        return name + suffix;
    }

    if( FTexture* ftex = gametex.GetTexture() )
    {
        if( FImageSource* imgsrc = ftex->GetImage() )
        {
            // a voxel model's palette: named by the palette (the same in every start, and
            // one texture for all the models of a pack -- they share the game's palette)
            const uint8_t* vpal = nullptr;
            if( IsVoxelPaletteTexture( imgsrc, &vpal ) )
            {
                uint32_t h = 2166136261u;
                for( int i = 0; vpal && i < 768; i++ )
                {
                    h = ( h ^ vpal[ i ] ) * 16777619u;
                }
                char buf[ 32 ];
                snprintf( buf, sizeof( buf ), "voxpal%08x", vpal ? h : 0u );
                return buf + suffix;
            }
            return "img" + std::to_string( imgsrc->GetId() ) + suffix;
        }
    }
    // no name and no image (movie frames, canvases): the caller makes one up
    return {};
}

RTHardwareTexture::~RTHardwareTexture()
{
    // Named textures stay in RTGL1 (another hardware texture may share the name).
    // The made up names of dynamic ones are never seen again: free their slots.
    if( m_dynamic && g_rtActive && rt.rgMarkOriginalTextureAsDeleted )
    {
        if( !m_name.empty() )
        {
            rt.rgMarkOriginalTextureAsDeleted( m_name.c_str() );
        }
        if( !m_nameClamped.empty() )
        {
            rt.rgMarkOriginalTextureAsDeleted( m_nameClamped.c_str() );
        }
    }
}

bool RTHardwareTexture::Upload( FGameTexture&       src,
                                int                 translation,
                                int                 flags,
                                const FRenderStyle& renderStyle,
                                const std::string&  name,
                                bool                clamp,
                                bool                materials )
{
    auto texbuffer = src.GetTexture()->CreateTexBuffer( translation, flags | CTF_ProcessData );
    if( !texbuffer.mBuffer || texbuffer.mWidth <= 0 || texbuffer.mHeight <= 0 )
    {
        return false;
    }

    if( renderStyle.Flags & STYLEF_RedIsAlpha )
    {
        const size_t n = size_t( texbuffer.mWidth ) * size_t( texbuffer.mHeight );
        for( size_t i = 0; i < n; i++ )
        {
            // BGRA: alpha = red
            texbuffer.mBuffer[ 4 * i + 3 ] = texbuffer.mBuffer[ 4 * i + 2 ];
        }
    }

    // a changing texture of a glowing class (the fire): no emission map, its own
    // colours are the glow (it moves with the texture); the light it casts has the
    // average colour
    if( !materials && !( flags & CTF_Indexed ) && !clamp )
    {
        if( const float g = RT_TableGlow( name ); g > 0.f )
        {
            m_glow = g;
            double     sum[ 3 ] = {};
            const size_t n      = size_t( texbuffer.mWidth ) * size_t( texbuffer.mHeight );
            for( size_t i = 0; i < n; i++ )
            {
                for( int k = 0; k < 3; k++ )
                {
                    const double v = texbuffer.mBuffer[ 4 * i + size_t( 2 - k ) ] / 255.0; // BGRA
                    sum[ k ] += v * v;
                }
            }
            for( int k = 0; k < 3; k++ )
            {
                m_emisAvg[ k ] = float( sum[ k ] / double( std::max< size_t >( n, 1 ) ) ) * g;
            }
        }
    }

    // RTGL1 reads the override files when the texture is given, so they come first
    if( materials && !( flags & CTF_Indexed ) )
    {
        RtMaterialInfo mi;
        m_hasEmission = RT_PrepareMaterialOverrides( src, name, texbuffer, mi );
        memcpy( m_emisAvg, mi.emisAvg, sizeof( m_emisAvg ) );
        m_polished = mi.polished;
        m_glass    = mi.glass;
        m_glow     = mi.glow;
        m_lamp     = mi.lamp;
        m_flame    = mi.flame;
        m_voxGlow.clear();
        if( mi.voxel && texbuffer.mWidth == 16 && texbuffer.mHeight == 16 )
        {
            m_voxGlow.assign( 256 * 3, 0.f );
            bool any = false;
            for( int i = 0; i < 256; i++ )
            {
                if( !mi.voxGlow[ i ] )
                {
                    continue;
                }
                for( int k = 0; k < 3; k++ )
                {
                    const float v              = texbuffer.mBuffer[ 4 * i + ( 2 - k ) ] / 255.f; // BGRA
                    m_voxGlow[ size_t( i ) * 3 + k ] = v * v;
                    any |= v > 0.05f;
                }
            }
            if( !any )
            {
                m_voxGlow.clear();
            }
        }
    }

    auto details = RgOriginalTextureDetailsEXT{
        .sType  = RG_STRUCTURE_TYPE_ORIGINAL_TEXTURE_DETAILS_EXT,
        .pNext  = nullptr,
        .flags  = 0,
        .format = ( flags & CTF_Indexed ) ? RG_FORMAT_R8_SRGB : RG_FORMAT_B8G8R8A8_SRGB,
    };

    const auto address = clamp ? RG_SAMPLER_ADDRESS_MODE_CLAMP : RG_SAMPLER_ADDRESS_MODE_REPEAT;

    auto info = RgOriginalTextureInfo{
        .sType        = RG_STRUCTURE_TYPE_ORIGINAL_TEXTURE_INFO,
        .pNext        = &details,
        .pTextureName = name.c_str(),
        .pPixels      = texbuffer.mBuffer,
        .size         = { uint32_t( texbuffer.mWidth ), uint32_t( texbuffer.mHeight ) },
        .filter       = RG_SAMPLER_FILTER_AUTO,
        .addressModeU = address,
        .addressModeV = address,
    };

    RG_CHECK( rt.rgProvideOriginalTexture( &info ) );
    return true;
}

void RTHardwareTexture::CreateIfWasnt( FGameTexture&       src,
                                       int                 translation,
                                       int                 flags,
                                       const FRenderStyle& renderStyle )
{
    if( m_created )
    {
        return;
    }
    m_created = true;

    if( !src.GetTexture() || src.isHardwareCanvas() )
    {
        return;
    }

    m_name = MakeTextureName( src, translation );
    // a tile the game writes (Blood's fire) is made again each time it changes
    const bool writable = IsWritableTile( &src );
    if( m_name.empty() || writable )
    {
        // RTGL1 keeps the first texture given a name, so a texture whose content is
        // replaced (a movie frame) must never reuse one. (A written tile keeps its
        // own name in front: "#02342~17" -- the material table still knows it; it
        // has no material files, they would be made for every frame.)
        static uint64_t serial = 0;
        m_name    = ( writable && !m_name.empty() ? m_name + "~" : std::string( "rtdyn" ) ) + std::to_string( ++serial );
        m_dynamic = true;
    }

    const auto t0 = std::chrono::steady_clock::now();
    if( !Upload( src, translation, flags, renderStyle, m_name, false, !m_dynamic ) )
    {
        m_name.clear();
    }
    g_rtTexCreates++;
    g_rtTexSeconds += std::chrono::duration< double >( std::chrono::steady_clock::now() - t0 ).count();
}

void RTHardwareTexture::CreateClampedIfWasnt( FGameTexture&       src,
                                              int                 translation,
                                              int                 flags,
                                              const FRenderStyle& renderStyle )
{
    if( m_createdClamped )
    {
        return;
    }
    m_createdClamped = true;

    if( !src.GetTexture() || src.isHardwareCanvas() )
    {
        return;
    }

    std::string base = MakeTextureName( src, translation );
    if( base.empty() )
    {
        // dynamic (movie frames): one name per hardware texture, freed with it
        static uint64_t serial = 0;
        base      = "rtdyn" + std::to_string( ++serial );
        m_dynamic = true;
    }
    m_nameClamped = base + "@ui";

    if( !Upload( src, translation, flags, renderStyle, m_nameClamped, true, false ) )
    {
        m_nameClamped.clear();
    }
}

//
// Render state
//

void RTRenderState::Draw( int dt, int index, int count, bool apply )
{
    if( IsCurrentDrawIgnored() || count <= 0 )
    {
        return;
    }

    const uint32_t* pIndices   = nullptr;
    uint32_t        indexCount = 0;
    bool            islines    = false;

    switch( dt )
    {
        case DT_Points: return;
        case DT_Lines: islines = true; break;
        case DT_Triangles: break;
        case DT_TriangleFan:
            rt.rgUtilScratchGetIndices( RG_UTIL_IM_SCRATCH_TOPOLOGY_TRIANGLE_FAN, count, &pIndices, &indexCount );
            break;
        case DT_TriangleStrip:
            rt.rgUtilScratchGetIndices( RG_UTIL_IM_SCRATCH_TOPOLOGY_TRIANGLE_STRIP, count, &pIndices, &indexCount );
            break;
        default: return;
    }

    auto vb = static_cast< RTVertexBuffer* >( mVertexBuffer );
    if( !vb )
    {
        return;
    }

    InternalDraw( vb->AccessFormatted( mVertexOffsets[ 0 ] + index, count ),
                  std::span{ pIndices, indexCount },
                  vb->IsUI(),
                  islines );
}

void RTRenderState::DrawIndexed( int dt, int index, int count, bool apply )
{
    if( IsCurrentDrawIgnored() || count <= 0 || index < 0 || dt != DT_Triangles )
    {
        return;
    }

    auto vb = static_cast< RTVertexBuffer* >( mVertexBuffer );
    auto ib = static_cast< RTIndexBuffer* >( mIndexBuffer );
    if( !vb || !ib )
    {
        return;
    }

    auto indices = ib->AccessFormatted( index, count );
    if( indices.empty() )
    {
        return;
    }

    auto [ vertFirst, vertCount ] = RTIndexBuffer::CalcFirstVertexAndVertexCount( indices );

    InternalDraw( vb->AccessFormatted( mVertexOffsets[ 0 ] + vertFirst, vertCount ),
                  ib->MakeWithNewFirstIndex( indices, vertFirst ),
                  vb->IsUI() );
}

void RTRenderState::RT_SetMatrices( const VSMatrix& view, const VSMatrix& proj )
{
    // GL clip space to Vulkan's: y down, z in [0,1]
    constexpr static float vkcorrection[] = {
        1, 0,  0,    0, //
        0, -1, 0,    0, //
        0, 0,  0.5f, 0, //
        0, 0,  0.5f, 1, //
    };

    MatMul44( vkcorrection, proj.get(), m_projection );
    memcpy( m_view, view.get(), sizeof( float ) * 16 );

    // ortho( 0, W, H, 0 ): remember W and H for the scissor
    const float* p = proj.get();
    if( std::abs( p[ 15 ] - 1.f ) < 1e-5f && p[ 0 ] != 0.f && p[ 5 ] != 0.f )
    {
        m_orthoSize[ 0 ] = 2.f / p[ 0 ];
        m_orthoSize[ 1 ] = -2.f / p[ 5 ];
    }
}

auto RTRenderState::ResolveTexture( bool clamped, const RTHardwareTexture** outTex ) -> const char*
{
    *outTex = nullptr;
    if( !mTextureEnabled || !mMaterial.mMaterial )
    {
        return nullptr;
    }
    FGameTexture* gametex = mMaterial.mMaterial->Source();
    if( !gametex )
    {
        return nullptr;
    }
    FTexture* base = gametex->GetTexture();
    if( !base )
    {
        return nullptr;
    }
    const int scaleflags = mMaterial.mMaterial->GetScaleFlags();
    auto      hwtex      = static_cast< RTHardwareTexture* >(
        base->GetHardwareTexture( mMaterial.mTranslation, scaleflags ) );
    if( !hwtex )
    {
        return nullptr;
    }
    if( clamped )
    {
        hwtex->CreateClampedIfWasnt( *gametex, mMaterial.mTranslation, scaleflags, mRenderStyle );
        return hwtex->GetRTNameClamped();
    }
    hwtex->CreateIfWasnt( *gametex, mMaterial.mTranslation, scaleflags, mRenderStyle );
    *outTex = hwtex;
    return hwtex->GetRTName();
}

// The 2D drawer clips with the scissor rectangle (menu transitions, scrolling lists,
// the console). RTGL1's swapchained primitives have no scissor, so clip the triangles
// on the CPU, in the drawer's pixel space.
auto RTRenderState::ClipToScissor( std::span< const RgPrimitiveVertex > verts,
                                   std::span< const uint32_t >          indices )
    -> std::span< const RgPrimitiveVertex >
{
    const float W = m_orthoSize[ 0 ], H = m_orthoSize[ 1 ];
    if( W <= 0 || H <= 0 || m_viewport.width <= 0 || m_viewport.height <= 0 )
    {
        return {};
    }

    // GL scissor: window pixels, origin at the bottom left
    const float sx = float( m_scissor[ 0 ] ), sy = float( m_scissor[ 1 ] );
    const float sw = float( m_scissor[ 2 ] ), sh = float( m_scissor[ 3 ] );
    const float kx = W / m_viewport.width, ky = H / m_viewport.height;

    const float x0 = ( sx - m_viewport.x ) * kx;
    const float x1 = ( sx + sw - m_viewport.x ) * kx;
    const float y0 = H - ( sy + sh - m_viewport.y ) * ky;
    const float y1 = H - ( sy - m_viewport.y ) * ky;

    struct V
    {
        float p[ 3 ];
        float t[ 2 ];
        float c[ 4 ];
    };
    auto unpack = []( const RgPrimitiveVertex& s ) {
        V v;
        memcpy( v.p, s.position, sizeof v.p );
        memcpy( v.t, s.texCoord, sizeof v.t );
        for( int k = 0; k < 4; k++ )
        {
            v.c[ k ] = float( ( s.color >> ( 8 * k ) ) & 0xFF );
        }
        return v;
    };
    auto pack = [ &verts ]( const V& v ) {
        RgPrimitiveVertex r = verts[ 0 ];
        memcpy( r.position, v.p, sizeof v.p );
        memcpy( r.texCoord, v.t, sizeof v.t );
        uint32_t c = 0;
        for( int k = 0; k < 4; k++ )
        {
            c |= uint32_t( std::clamp( std::lround( v.c[ k ] ), 0l, 255l ) ) << ( 8 * k );
        }
        r.color = c;
        return r;
    };
    auto lerp = []( const V& a, const V& b, float t ) {
        V r;
        for( int k = 0; k < 3; k++ ) r.p[ k ] = a.p[ k ] + ( b.p[ k ] - a.p[ k ] ) * t;
        for( int k = 0; k < 2; k++ ) r.t[ k ] = a.t[ k ] + ( b.t[ k ] - a.t[ k ] ) * t;
        for( int k = 0; k < 4; k++ ) r.c[ k ] = a.c[ k ] + ( b.c[ k ] - a.c[ k ] ) * t;
        return r;
    };

    m_clipverts.clear();

    const size_t triCount = indices.empty() ? verts.size() / 3 : indices.size() / 3;
    std::vector< V > poly, next;
    for( size_t t = 0; t < triCount; t++ )
    {
        poly.clear();
        for( int k = 0; k < 3; k++ )
        {
            const size_t idx = indices.empty() ? t * 3 + k : indices[ t * 3 + k ];
            if( idx >= verts.size() )
            {
                poly.clear();
                break;
            }
            poly.push_back( unpack( verts[ idx ] ) );
        }

        // Sutherland-Hodgman against the four edges: axis, limit, keep >= (true) or <=
        const struct
        {
            int   axis;
            float limit;
            bool  greater;
        } edges[] = { { 0, x0, true }, { 0, x1, false }, { 1, y0, true }, { 1, y1, false } };

        for( const auto& e : edges )
        {
            if( poly.empty() )
            {
                break;
            }
            next.clear();
            for( size_t i = 0; i < poly.size(); i++ )
            {
                const V& a   = poly[ i ];
                const V& b   = poly[ ( i + 1 ) % poly.size() ];
                const float da = e.greater ? a.p[ e.axis ] - e.limit : e.limit - a.p[ e.axis ];
                const float db = e.greater ? b.p[ e.axis ] - e.limit : e.limit - b.p[ e.axis ];
                if( da >= 0 )
                {
                    next.push_back( a );
                }
                if( ( da >= 0 ) != ( db >= 0 ) )
                {
                    next.push_back( lerp( a, b, da / ( da - db ) ) );
                }
            }
            poly.swap( next );
        }

        for( size_t i = 1; i + 1 < poly.size(); i++ )
        {
            m_clipverts.push_back( pack( poly[ 0 ] ) );
            m_clipverts.push_back( pack( poly[ i ] ) );
            m_clipverts.push_back( pack( poly[ i + 1 ] ) );
        }
    }

    return m_clipverts;
}

auto RTRenderState::FallbackUniqueId( std::span< const RgPrimitiveVertex > verts, const char* texname ) const
    -> uint64_t
{
    // a draw the scene code did not name: derive an id from what it looks like, so a
    // static primitive keeps its id from frame to frame
    uint64_t h = 1469598103934665603ull;
    auto     mix = [ &h ]( uint64_t v ) {
        h ^= v;
        h *= 1099511628211ull;
    };
    for( int c = 0; c < 3; c++ )
    {
        mix( uint64_t( int64_t( std::lround( verts[ 0 ].position[ c ] * 256.f ) ) ) );
    }
    mix( verts.size() );
    for( const char* p = texname; p && *p; ++p )
    {
        mix( uint8_t( *p ) );
    }
    return FRtState::MixId( h, RtIdKind::Other );
}

auto RTRenderState::MakeTransform( bool isSky, bool isModel ) const -> RgTransform
{
    auto fromGzMatrix = []( const float* m ) {
        return RgTransform{ {
            { m[ 0 ], m[ 4 ], m[ 8 ], m[ 12 ] * ONEGAMEUNIT_IN_METERS },
            { m[ 1 ], m[ 5 ], m[ 9 ], m[ 13 ] * ONEGAMEUNIT_IN_METERS },
            { m[ 2 ], m[ 6 ], m[ 10 ], m[ 14 ] * ONEGAMEUNIT_IN_METERS },
        } };
    };

    if( isSky )
    {
        // The sky is drawn with a view that only rotates. That view combined with the
        // main camera's inverse is the change from GL's axes to RTGL1's (rounded to
        // exact axes), placed at the camera; apply it to the dome's model matrix.
        auto l_unit = []( float f ) {
            return f > +0.5f ? +1.0f : f < -0.5f ? -1.0f : 0.0f;
        };

        float irr[ 16 ];
        MatMul44( m_mainCameraView_Inverse, m_view, irr );

        const float skyToMainCamera[ 16 ] = {
            l_unit( irr[ 0 ] ), l_unit( irr[ 1 ] ), l_unit( irr[ 2 ] ),  0, //
            l_unit( irr[ 4 ] ), l_unit( irr[ 5 ] ), l_unit( irr[ 6 ] ),  0, //
            l_unit( irr[ 8 ] ), l_unit( irr[ 9 ] ), l_unit( irr[ 10 ] ), 0, //
            irr[ 12 ],          irr[ 13 ],          irr[ 14 ],           1, //
        };

        const float* model = mModelMatrixEnabled ? mModelMatrix.get() : nullptr;
        float        identity[ 16 ] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };

        float t[ 16 ];
        MatMul44( skyToMainCamera, model ? model : identity, t );
        return fromGzMatrix( t );
    }

    if( mModelMatrixEnabled )
    {
        // model matrices (voxels) output GL's axes: x, height, y; swap to x, y, height
        const float* m = mModelMatrix.get();
        if( isModel )
        {
            return RgTransform{ {
                { m[ 0 ], m[ 4 ], m[ 8 ], m[ 12 ] * ONEGAMEUNIT_IN_METERS },
                { m[ 2 ], m[ 6 ], m[ 10 ], m[ 14 ] * ONEGAMEUNIT_IN_METERS },
                { m[ 1 ], m[ 5 ], m[ 9 ], m[ 13 ] * ONEGAMEUNIT_IN_METERS },
            } };
        }
        return fromGzMatrix( m );
    }

    return RT_TRANSFORM_IDENTITY;
}

void RTRenderState::InternalDraw( std::span< const RgPrimitiveVertex > verts,
                                  std::span< const uint32_t >          indices,
                                  const bool                           isUI,
                                  const bool                           islines )
{
    if( verts.empty() || ( islines && !isUI ) )
    {
        return;
    }
    if( !m_fb || !m_fb->RT_IsFrameStarted() )
    {
        m_droppedThisFrame++;
        return;
    }

    const bool isSky    = !isUI && rtstate.is< RtPrim::Sky >();
    const bool isSkyVis = !isUI && !isSky && rtstate.is< RtPrim::SkyVisibility >();
    const bool isModel  = static_cast< RTVertexBuffer* >( mVertexBuffer )->IsModel();
    const bool isMirror = !isUI && !isSky && !isSkyVis && rtstate.is< RtPrim::Mirror >();

    // fog-only and other untextured world draws (fog boundaries, solid fog colour) do
    // not exist under RT; the sky caps and the sky and mirror openings are untextured
    // on purpose
    if( !isUI && !isSky && !isSkyVis && !isMirror && !mTextureEnabled )
    {
        return;
    }

    // HUD and menu pictures are clamped at the edges, unless they are meant to tile
    const bool clampUI  = isUI && mMaterial.mClampMode != CLAMP_NONE;
    const RTHardwareTexture* hwtex = nullptr;
    const char* texname     = ( isSkyVis || isMirror ) ? nullptr : ResolveTexture( clampUI, &hwtex );
    const bool  emissionMap = hwtex && hwtex->HasEmission() && !isUI && !isSky;
    // a glowing texture with no emission map (one that changes: the fire)
    const bool  glowTex     = !emissionMap && hwtex && hwtex->Glow() > 0.f && !isUI && !isSky && !isSkyVis;

    if( isUI && !islines && m_scissor[ 2 ] >= 0 && m_scissor[ 3 ] >= 0 )
    {
        verts   = ClipToScissor( verts, indices );
        indices = {};
        if( verts.empty() )
        {
            return;
        }
    }

    if( isSky && texname )
    {
        m_fb->RT_MarkWasSky();
    }

    // texture matrix (the sky's scale and scrolling; Raze bakes flat panning into the
    // texture coordinates)
    if( mTextureMatrixEnabled && !isSkyVis )
    {
        m_tempverts.assign( verts.begin(), verts.end() );
        const float* tm = mTextureMatrix.get();
        const float  tu = tm[ 12 ];
        const float  tv = tm[ 13 ];
        for( auto& v : m_tempverts )
        {
            const float u = v.texCoord[ 0 ], w = v.texCoord[ 1 ];
            v.texCoord[ 0 ] = tm[ 0 ] * u + tm[ 4 ] * w + tu;
            v.texCoord[ 1 ] = tm[ 1 ] * u + tm[ 5 ] * w + tv;
        }
        verts = m_tempverts;
    }

    // Build's rooms over rooms: a room's ceiling is often at exactly the height of the
    // floor of the room above it (overlapping sectors). The rasterizer draws only the
    // one it can see; rays hit both at random -- a grainy, speckled mix of the two.
    // Every floor moves up a little and every ceiling down, into its own room, so each
    // side sees its own surface. (Walls reach past them: no gaps.)
    // (only those planes: moving every floor and ceiling opens slits under the walls of
    // raised platforms -- coffins showed their insides at the edges)
    if( !isUI && *rt_flat_offset > 0.f &&
        ( rtstate.is< RtPrim::FloorPlane >() || rtstate.is< RtPrim::CeilingPlane >() ) &&
        RT_FlatTouchesOther( rtstate.get_sector(), rtstate.is< RtPrim::CeilingPlane >() ) )
    {
        if( verts.data() != m_tempverts.data() )
        {
            m_tempverts.assign( verts.begin(), verts.end() );
        }
        const float dz = rtstate.is< RtPrim::FloorPlane >() ? float( *rt_flat_offset ) : -float( *rt_flat_offset );
        for( auto& v : m_tempverts )
        {
            v.position[ 2 ] += dz;
        }
        verts = m_tempverts;
    }

    // A sprite on a wall (a switch, a sign) or a floor is usually exactly flush with it:
    // rays then hit the sprite or the surface at random -- grainy, half see-through.
    // Raster depth offsets it; here it is moved off the surface, towards the viewer.
    if( !isUI && rtstate.is< RtPrim::Decal >() && *rt_decal_offset > 0.f )
    {
        const FVector4& gn = mStreamData.uVertexNormal;
        FVector3        n{ gn.X, gn.Z, gn.Y };
        if( n.LengthSquared() > 0.25f )
        {
            n.MakeUnit();
            if( verts.data() != m_tempverts.data() )
            {
                m_tempverts.assign( verts.begin(), verts.end() );
            }
            const RgFloat3D& cam = m_fb->RT_CameraPosition();
            const float*     p0  = m_tempverts[ 0 ].position;
            const FVector3   toCam{ cam.data[ 0 ] - p0[ 0 ], cam.data[ 1 ] - p0[ 1 ], cam.data[ 2 ] - p0[ 2 ] };
            if( ( n | toCam ) < 0.f )
            {
                n = -n;
            }
            const FVector3 d = n * float( *rt_decal_offset );
            for( auto& v : m_tempverts )
            {
                v.position[ 0 ] += d.X;
                v.position[ 1 ] += d.Y;
                v.position[ 2 ] += d.Z;
            }
            verts = m_tempverts;
        }
    }

    // A sprite's picture is not tiled, but its texture repeats: filtered at its edges, the
    // opposite edge bleeds in -- a line along the top of a flame (its bright bottom row).
    // Its texture coordinates stop half a texel in from the edges. (The HUD's pictures
    // have clamped textures; a sprite's carries its material maps, so it keeps its own.)
    if( !isUI && !isModel && ( rtstate.is< RtPrim::Sprite >() || rtstate.is< RtPrim::Decal >() ) && mMaterial.mMaterial &&
        mMaterial.mMaterial->Source() )
    {
        const float tw = float( std::max( 1, int( mMaterial.mMaterial->Source()->GetTexelWidth() ) ) );
        const float th = float( std::max( 1, int( mMaterial.mMaterial->Source()->GetTexelHeight() ) ) );
        if( verts.data() != m_tempverts.data() )
        {
            m_tempverts.assign( verts.begin(), verts.end() );
        }
        const float u0 = 0.5f / tw, v0 = 0.5f / th;
        for( auto& v : m_tempverts )
        {
            v.texCoord[ 0 ] = std::clamp( v.texCoord[ 0 ], u0, 1.f - u0 );
            v.texCoord[ 1 ] = std::clamp( v.texCoord[ 1 ], v0, 1.f - v0 );
        }
        verts = m_tempverts;
    }

    RgTransform transform = isUI ? RT_TRANSFORM_IDENTITY : MakeTransform( isSky, isModel );
    // a voxel model drawn twice at the same place (two passes reach its sprite) is the
    // same mesh twice: twice the triangles, all of them overlapping
    if( isModel )
    {
        uint64_t h = 1469598103934665603ull;
        auto     mix = [ &h ]( const void* p, size_t n ) {
            for( size_t i = 0; i < n; i++ )
            {
                h = ( h ^ static_cast< const uint8_t* >( p )[ i ] ) * 1099511628211ull;
            }
        };
        const void* data  = verts.data();
        const size_t count = verts.size();
        mix( &data, sizeof( data ) );
        mix( &count, sizeof( count ) );
        mix( &transform, sizeof( transform ) );
        if( rtstate.is< RtPrim::CopyMove >() )
        {
            mix( rtstate.get_copyoffset(), sizeof( double ) * 3 );
        }
        if( std::find( m_modelsSeen.begin(), m_modelsSeen.end(), h ) != m_modelsSeen.end() )
        {
            m_modelDupes++;
            return;
        }
        m_modelsSeen.push_back( h );
    }

    // a copy of geometry from elsewhere (Duke's world above the water, seen from under it)
    const bool isCopy = !isUI && ( rtstate.is< RtPrim::CopyMove >() || rtstate.is< RtPrim::CopyId >() );
    if( !isUI && rtstate.is< RtPrim::CopyMove >() )
    {
        const double* ofs = rtstate.get_copyoffset();
        transform.matrix[ 0 ][ 3 ] += float( ofs[ 0 ] ) * ONEGAMEUNIT_IN_METERS;
        transform.matrix[ 1 ][ 3 ] -= float( ofs[ 1 ] ) * ONEGAMEUNIT_IN_METERS;
        transform.matrix[ 2 ][ 3 ] -= float( ofs[ 2 ] ) * ONEGAMEUNIT_IN_METERS;
    }

    // colour: the game's light levels are not baked into the albedo, the path tracer
    // does the lighting; the alpha is kept
    const float alpha = mStreamData.uObjectColor.a * mStreamData.uVertexColor[ 3 ];
    RgColor4DPacked32 primColor;
    if( isUI || isSky )
    {
        primColor = rt.rgUtilPackColorFloat4D( mStreamData.uObjectColor.r * mStreamData.uVertexColor[ 0 ],
                                               mStreamData.uObjectColor.g * mStreamData.uVertexColor[ 1 ],
                                               mStreamData.uObjectColor.b * mStreamData.uVertexColor[ 2 ],
                                               alpha );
    }
    else if( isSkyVis || isMirror )
    {
        // must stay opaque: RTGL1 rasterizes anything with alpha below 0.98
        primColor = RG_PACKED_COLOR_WHITE;
    }
    else if( *rt_world_white )
    {
        primColor = rt.rgUtilPackColorFloat4D( 1.f, 1.f, 1.f, alpha );
    }
    else
    {
        primColor = rt.rgUtilPackColorFloat4D( mStreamData.uObjectColor.r * mStreamData.uVertexColor[ 0 ],
                                               mStreamData.uObjectColor.g * mStreamData.uVertexColor[ 1 ],
                                               mStreamData.uObjectColor.b * mStreamData.uVertexColor[ 2 ],
                                               alpha );
    }

    const bool additive =
        mRenderStyle.BlendOp == STYLEOP_Add && mRenderStyle.DestAlpha == STYLEALPHA_One;

    // a window: a see-through wall of a glass texture (the material table). Real glass
    // instead of a blended picture: it refracts what is behind it and reflects the room,
    // tinted by its texture (stained glass), and lets light through. A glass texture
    // painted on a solid wall (a window with its frame, no view through) stays as it is.
    const bool isGlass = !isUI && !isSky && !isSkyVis && !isMirror && *rt_glass && hwtex && hwtex->IsGlass() &&
                         !additive && alpha < 0.98f && !rtstate.is< RtPrim::Sprite >();
    if( isGlass )
    {
        // opaque, or RTGL1 rasterizes it as a blended surface
        primColor = rt.rgUtilPackColorFloat4D( 1.f, 1.f, 1.f, 1.f );
    }
    // a glowing class (the material table's fire: an oven's flames): solid, so it is
    // path traced -- its glow lights the room -- even where it is drawn see-through
    if( !isUI && !isSky && !isSkyVis && !isMirror && hwtex && hwtex->Glow() > 0.f && alpha < 0.98f && !additive )
    {
        primColor = rt.rgUtilPackColorFloat4D( 1.f, 1.f, 1.f, 1.f );
    }

    RgMeshPrimitiveFlags primFlags  = 0;
    float                emissive   = 0.f;
    float                emissiveGi = 0.f;
    float                glowScale  = 1.f; // a glowing texture's brightness, by its shade
    float                emisLightScale = 1.f; // and of the light it casts (LIGHTOVR)

    // classic (non path traced) brightness: the soft light level from the shade
    float classic = 1.0f;
    if( !isUI && mLightParms[ 3 ] >= 0.f )
    {
        classic = std::clamp( mLightParms[ 3 ], 0.f, 1.f );
    }

    if( isUI )
    {
        primFlags = RG_MESH_PRIMITIVE_TRANSLUCENT;
    }
    else if( isSky )
    {
        primFlags = RG_MESH_PRIMITIVE_SKY | RG_MESH_PRIMITIVE_TRANSLUCENT;
    }
    else if( isSkyVis )
    {
        primFlags = RG_MESH_PRIMITIVE_SKY_VISIBILITY;
    }
    else
    {
        if( rtstate.is< RtPrim::Mirror >() )
        {
            primFlags |= RG_MESH_PRIMITIVE_MIRROR;
        }
        else if( rtstate.is< RtPrim::Glass >() || isGlass )
        {
            primFlags |= RG_MESH_PRIMITIVE_GLASS | RG_MESH_PRIMITIVE_THIN_MEDIA | RG_MESH_PRIMITIVE_NO_SHADOW;
        }
        else if( rtstate.is< RtPrim::Water >() )
        {
            primFlags |= RG_MESH_PRIMITIVE_WATER;
        }
        else if( rtstate.is< RtPrim::Lava >() )
        {
            primFlags |= RG_MESH_PRIMITIVE_LAVA;
        }
        else if( rtstate.is< RtPrim::BloodPool >() )
        {
            // the stylized liquid surface, liquid 3 (Doom 64's blood: dark red)
            primFlags |= RG_MESH_PRIMITIVE_WATER | RG_MESH_PRIMITIVE_LIQUID_BIT0 | RG_MESH_PRIMITIVE_LIQUID_BIT1;
        }
        else if( rtstate.is< RtPrim::Slime >() )
        {
            // the stylized liquid surface, liquid 1 (Doom 64's nukage: green)
            primFlags |= RG_MESH_PRIMITIVE_WATER | RG_MESH_PRIMITIVE_LIQUID_BIT0;
        }

        if( additive && !isMirror )
        {
            primFlags |= RG_MESH_PRIMITIVE_TRANSLUCENT;
            emissive = *rt_additive_emis;
        }
        // (not a voxel model: its skin is a palette, every texel solid. Alpha tested
        // geometry is tested in a shader function at every hit the rays find on it, near
        // or far -- a few voxel models of a quarter million triangles made every frame
        // several times slower)
        else if( mAlphaThreshold > 0.f && texname && !isMirror && ( !isModel || *rt_voxel_alphatest ) )
        {
            primFlags |= RG_MESH_PRIMITIVE_ALPHA_TESTED;
        }

        // (a voxel model is drawn by its sprite, but it is solid geometry)
        const bool isSprite = rtstate.is< RtPrim::Sprite >() && !isModel;
        if( isSprite )
        {
            primFlags |= RG_MESH_PRIMITIVE_SPRITE;
        }
        if( rtstate.is< RtPrim::NoShadow >() )
        {
            primFlags |= RG_MESH_PRIMITIVE_NO_SHADOW;
        }
        if( emissionMap )
        {
            // the emission map (from Duke-RT's glowmaps) is the glow, shown as it is;
            // how much of it reaches the bounce light is separate. With a real light
            // cast by the surface (rt_emis_lights), the bounce light from the texture
            // itself is mostly redundant -- and it is the grainy, splotchy part
            primFlags |= RG_MESH_PRIMITIVE_EMISSIVE_OVERRIDE | RG_MESH_PRIMITIVE_EMISSIVE_SCREEN_SCALED;
            // a light switch darkens its sector (the shade goes up): the fixtures in
            // it go out with it. Lit rooms are shade 0..10 (level 0.7+), "off" is 20+
            // a fixture in a switched room goes out with its lights
            // (not a fullbright actor's: a fire burns in a dark alcove too)
            // (nor a candle's flame: it burns in a dark room)
            if( *rt_emis_follow_shade && !rtstate.is< RtPrim::Fullbright >() && !hwtex->IsFlame() )
            {
                const float sw = RT_SectorSwitch( rtstate.get_sector() );
                if( sw >= 0.f )
                {
                    glowScale = sw;
                }
            }
            // the RT author's tuning of this surface (LIGHTOVR emissiveoverride)
            {
                const char* nm   = hwtex->GetRTName();
                const int   tile = ( nm && nm[ 0 ] == '#' ) ? atoi( nm + 1 ) : -1;
                float       glow = -1.f;
                emisLightScale   = RT_EmissiveOverride( rtstate.get_sector(), tile, glow );
                if( glow >= 0.f )
                {
                    glowScale = glow;
                }
            }
            // a lamp (the material table's) is lit as its room is: a room whose light is
            // switched off (its shade goes up) has its lamps off
            float screenScale = 1.f;
            if( hwtex->IsLamp() )
            {
                glowScale *= std::clamp( ( classic - 0.25f ) / 0.45f, 0.f, 1.f );
                // its light by the class's glow; on screen softer than a sign's (it is
                // big and white: at full brightness it outshone the room it lights)
                emisLightScale *= hwtex->Glow();
                screenScale = 0.45f;
            }
            emissive   = glowScale * screenScale;
            emissiveGi = glowScale * float( *rt_emis_mult ) * ( *rt_emis_lights ? float( *rt_emis_gi ) : 1.f );
            // a voxel model's glowing faces (a voxel fire is mostly glow, and flickers
            // through its frames): softer than a sign's
            if( isModel && rtstate.is< RtPrim::Fullbright >() )
            {
                // (a fullbright actor's: Duke-RT's fires, which cast their own light too)
                emissive *= std::max( 0.f, float( *rt_voxel_glow ) );
                emissiveGi *= std::max( 0.f, float( *rt_voxel_glow ) );
            }
        }
        else
        {
            if( rtstate.is< RtPrim::Fullbright >() && *rt_fullbright_emis > 0.f )
            {
                emissive = std::max( emissive, float( *rt_fullbright_emis ) );
            }
            // a changing glowing texture (the fire): its own colours glow
            if( glowTex )
            {
                emissive   = std::max( emissive, hwtex->Glow() );
                emissiveGi = emissive * float( *rt_emis_mult ) * ( *rt_emis_lights ? float( *rt_emis_gi ) : 1.f );
            }
            // a floor under the path traced light, like Duke-RT's sector ambient: the
            // surface glows faintly with its sector's classic light level, so a room
            // the lights miss is dark rather than black (and it lights its
            // surroundings a little, through the bounce light)
            if( *rt_ambient > 0.f && !isMirror && !isGlass && !rtstate.is< RtPrim::Glass >() && !rtstate.is< RtPrim::Water >() )
            {
                // a switched-off room is darker than its half-light shade says
                const float sw = RT_SectorSwitch( rtstate.get_sector() );
                // and a flickering sector's glow follows its flicker softly: the glow feeds
                // the bounce light of the whole room, and a jumping one makes it crawl
                const float soft = RT_SectorFlickerSmoothing( rtstate.get_sector() );
                emissive = std::max( emissive, float( *rt_ambient ) * std::min( classic * soft, 1.f ) *
                                                   ( sw < 0.f ? 1.f : 0.35f + 0.65f * sw ) );
            }
        }

        // smooth texels (from the roughness map) mirror the room
        if( hwtex && hwtex->IsPolished() && *rt_polish && !isSprite && !isModel && !additive &&
            !( primFlags & ( RG_MESH_PRIMITIVE_GLASS | RG_MESH_PRIMITIVE_WATER | RG_MESH_PRIMITIVE_LAVA |
                             RG_MESH_PRIMITIVE_ACID ) ) )
        {
            primFlags |= RG_MESH_PRIMITIVE_POLISHED;
        }

        primFlags |= RG_MESH_PRIMITIVE_FORCE_EXACT_NORMALS;

        // Walls and floors do not move (bar doors and lifts): their motion is the
        // camera's alone. Matched with "the same primitive last frame" instead, any
        // part that is drawn in a different order or split differently this frame gets
        // another part's old position -- the denoiser then pulls history from the
        // wrong place: a smeared picture, black jagged edges as the view turns
        if( !*rt_world_motion && !isModel && !rtstate.is< RtPrim::Sprite >() && !rtstate.is< RtPrim::Decal >() )
        {
            primFlags |= RG_MESH_PRIMITIVE_NO_MOTION_VECTORS;
        }
    }

    // a one-sided wall or floor (rt_one_sided): its triangles turned to face its sector --
    // the side its normal points to (towards the sector: the wall's from its start to its
    // end, turned left; up for a floor, down for a ceiling) -- and marked so; solid only
    // (a see-through or liquid surface is seen from both sides)
    if( *rt_one_sided != 0 && !isUI && !isSky && !isSkyVis && !isMirror && !isModel && !islines &&
        rtstate.is< RtPrim::OneSided >() &&
        !( primFlags & ( RG_MESH_PRIMITIVE_ALPHA_TESTED | RG_MESH_PRIMITIVE_TRANSLUCENT | RG_MESH_PRIMITIVE_GLASS |
                         RG_MESH_PRIMITIVE_WATER | RG_MESH_PRIMITIVE_LAVA | RG_MESH_PRIMITIVE_ACID |
                         RG_MESH_PRIMITIVE_SPRITE | RG_MESH_PRIMITIVE_MIRROR ) ) )
    {
        const FVector4& gn = mStreamData.uVertexNormal;
        FVector3        n{ gn.X, gn.Z, gn.Y }; // (as the vertices: the renderer's axes, see AddEmissionLight)
        if( n.LengthSquared() > 0.25f )
        {
            if( *rt_one_sided < 0 )
            {
                n = -n;
            }
            const auto& m  = transform.matrix;
            auto        xf = [ &m ]( const float* p ) {
                return FVector3{ m[ 0 ][ 0 ] * p[ 0 ] + m[ 0 ][ 1 ] * p[ 1 ] + m[ 0 ][ 2 ] * p[ 2 ],
                                 m[ 1 ][ 0 ] * p[ 0 ] + m[ 1 ][ 1 ] * p[ 1 ] + m[ 1 ][ 2 ] * p[ 2 ],
                                 m[ 2 ][ 0 ] * p[ 0 ] + m[ 2 ][ 1 ] * p[ 1 ] + m[ 2 ][ 2 ] * p[ 2 ] };
            };
            const size_t count = indices.empty() ? verts.size() : indices.size();
            m_tempidx.resize( count );
            for( size_t i = 0; i < count; i++ )
            {
                m_tempidx[ i ] = indices.empty() ? uint32_t( i ) : indices[ i ];
            }
            // (a floor faces up, a ceiling down, whatever its normal says: Raze's for a
            // sloped ceiling was wrong, and a triangle turned by it went missing)
            const bool isFloor   = rtstate.is< RtPrim::FloorPlane >();
            const bool isCeiling = rtstate.is< RtPrim::CeilingPlane >();
            bool ok = count % 3 == 0;
            for( size_t t = 0; ok && t + 2 < count; t += 3 )
            {
                uint32_t* tri = &m_tempidx[ t ];
                if( tri[ 0 ] >= verts.size() || tri[ 1 ] >= verts.size() || tri[ 2 ] >= verts.size() )
                {
                    ok = false;
                    break;
                }
                const FVector3 a = xf( verts[ tri[ 0 ] ].position ), b = xf( verts[ tri[ 1 ] ].position ),
                               c = xf( verts[ tri[ 2 ] ].position );
                // RTGL1 keeps the side its geometric normal (the counterclockwise one) faces
                const FVector3 g      = ( b - a ) ^ ( c - a );
                const float    facing = isFloor ? g.Z : isCeiling ? -g.Z : ( g | n );
                if( ( *rt_one_sided < 0 && ( isFloor || isCeiling ) ? -facing : facing ) < 0.f )
                {
                    std::swap( tri[ 1 ], tri[ 2 ] );
                }
            }
            if( ok )
            {
                indices = std::span< const uint32_t >( m_tempidx.data(), m_tempidx.size() );
                primFlags |= RT_PRIMITIVE_ONE_SIDED;
            }
        }
    }

    // ids
    uint64_t uniqueId = rtstate.has_uniqueid() ? rtstate.get_uniqueid() : FallbackUniqueId( verts, texname );
    if( isUI && !rtstate.has_uniqueid() )
    {
        uniqueId = FRtState::MixId( 0x2D, RtIdKind::UI );
    }
    if( isCopy )
    {
        uniqueId = FRtState::MixId( uniqueId + uint64_t( rtstate.get_copygroup() ), RtIdKind::Copy );
    }

    auto ui = RgMeshPrimitiveSwapchainedEXT{
        .sType           = RG_STRUCTURE_TYPE_MESH_PRIMITIVE_SWAPCHAINED_EXT,
        .pNext           = nullptr,
        .flags           = islines ? uint32_t{ RG_MESH_PRIMITIVE_SWAPCHAINED_DRAW_AS_LINES } : 0u,
        .pViewport       = &m_viewport,
        .pView           = m_view,
        .pProjection     = m_projection,
        .pViewProjection = nullptr,
    };

    auto mesh = RgMeshInfo{
        .sType                = RG_STRUCTURE_TYPE_MESH_INFO,
        .pNext                = nullptr,
        .flags                = ( !isUI && rtstate.is< RtPrim::Viewer >() ) ? RgMeshInfoFlags( RG_MESH_FIRST_PERSON_VIEWER ) : 0,
        .uniqueObjectID       = uniqueId,
        .pMeshName            = nullptr,
        .transform            = transform,
        .isExportable         = false,
        .animationTime        = 0.0f,
        .localLightsIntensity = 1.0f,
    };

    auto prim = RgMeshPrimitiveInfo{
        .sType                = RG_STRUCTURE_TYPE_MESH_PRIMITIVE_INFO,
        .pNext                = isUI ? &ui : nullptr,
        .flags                = primFlags,
        .primitiveIndexInMesh = rtstate.next_primitiveindex( uniqueId ),
        .pVertices            = verts.data(),
        .vertexCount          = uint32_t( verts.size() ),
        .pIndices             = indices.empty() ? nullptr : indices.data(),
        .indexCount           = uint32_t( indices.size() ),
        .pTextureName         = texname,
        .textureFrame         = 0,
        .color                = primColor,
        .emissive             = emissive,
        .classicLight         = classic,
        .emissiveGi           = emissiveGi,
    };

    if( *rt_debug_uniqueid && !isUI && !rtstate.has_uniqueid() )
    {
        static int printed = 0;
        if( printed < 50 )
        {
            printed++;
            Printf( "RT: draw without an id: tex=%s verts=%u sky=%d skyvis=%d\n",
                    texname ? texname : "-",
                    unsigned( verts.size() ),
                    int( isSky ),
                    int( isSkyVis ) );
        }
    }

    const auto t0 = std::chrono::steady_clock::now();
    RgResult   r  = rt.rgUploadMeshPrimitive( &mesh, &prim );
    m_uploadSeconds += std::chrono::duration< double >( std::chrono::steady_clock::now() - t0 ).count();
    m_vertsThisFrame += verts.size();
    if( isModel )
    {
        m_modelDraws++;
        m_modelVerts += verts.size();
    }
    if( r != RG_RESULT_SUCCESS )
    {
        m_droppedThisFrame++;
        static int printed = 0;
        if( printed < 20 )
        {
            printed++;
            Printf( "RT: rgUploadMeshPrimitive failed (%d): tex=%s verts=%u ui=%d\n",
                    int( r ),
                    texname ? texname : "-",
                    unsigned( verts.size() ),
                    int( isUI ) );
        }
        return;
    }
    m_drawsThisFrame++;

    // a glowing surface lights the room with a real light, which the path tracer
    // samples directly (its emission alone only shows up in the bounce light, grainy)
    // (sprites: Duke-RT's LIGHTOVR names the lights of Duke's actors; in the other
    // games a glowing sprite -- a candle, a torch -- casts its own)
    const bool spriteGlow = rtstate.is< RtPrim::Sprite >() && *rt_sprite_lights && !RT_HaveActorLightRules();
    // (a flame class's light is the game's, placed at its flame: this one would be at the
    // middle of the picture, a candelabra's stem)
    if( ( emissionMap || glowTex ) && *rt_emis_lights && ( !rtstate.is< RtPrim::Sprite >() || spriteGlow ) && !isUI && !isSky &&
        glowScale > 0.01f && !hwtex->IsFlame() )
    {
        if( isModel )
        {
            if( const float* vg = hwtex->VoxelGlow() )
            {
                AddVoxelGlowLight( verts, indices, transform, vg, glowScale * emisLightScale,
                                   FRtState::MixId( uniqueId * 31 + prim.primitiveIndexInMesh, RtIdKind::Other ) );
            }
        }
        else
        {
            AddEmissionLight( verts, indices, transform, hwtex->EmissionAverage(), glowScale * emisLightScale,
                              FRtState::MixId( uniqueId * 31 + prim.primitiveIndexInMesh, RtIdKind::Other ) );
        }
    }
}

// A voxel model's palette texture holds every colour of the palette, the fullbright ones
// too, whether the model uses them or not: its emission map says nothing about the
// model. The faces of its fullbright colours do: their area, centre and colour (worked
// out once per model and palette) make the light, a small ball where they are.
void RTRenderState::AddVoxelGlowLight( std::span< const RgPrimitiveVertex > verts,
                                       std::span< const uint32_t >          indices,
                                       const RgTransform&                   transform,
                                       const float*                         glow,
                                       float                                scale,
                                       uint64_t                             id )
{
    uint64_t key = 1469598103934665603ull;
    for( uint64_t v : { uint64_t( uintptr_t( verts.data() ) ), uint64_t( verts.size() ), uint64_t( indices.size() ),
                        uint64_t( uintptr_t( glow ) ) } )
    {
        key = ( key ^ v ) * 1099511628211ull;
    }
    auto it = m_voxGlowCache.find( key );
    if( it == m_voxGlowCache.end() )
    {
        VoxelGlowGeo g;
        double       c[ 3 ] = {}, rgb[ 3 ] = {};
        const size_t tris   = indices.empty() ? verts.size() / 3 : indices.size() / 3;
        for( size_t t = 0; t < tris; t++ )
        {
            size_t i0 = t * 3, i1 = i0 + 1, i2 = i0 + 2;
            if( !indices.empty() )
            {
                i0 = indices[ i0 ], i1 = indices[ i1 ], i2 = indices[ i2 ];
            }
            if( i0 >= verts.size() || i1 >= verts.size() || i2 >= verts.size() )
            {
                continue;
            }
            // a face is one colour: one texel of the 16 x 16 palette
            const int    tx = std::clamp( int( verts[ i0 ].texCoord[ 0 ] * 16.f ), 0, 15 );
            const int    ty = std::clamp( int( verts[ i0 ].texCoord[ 1 ] * 16.f ), 0, 15 );
            const float* e  = &glow[ ( ty * 16 + tx ) * 3 ];
            if( e[ 0 ] + e[ 1 ] + e[ 2 ] <= 0.f )
            {
                continue;
            }
            const float* a  = verts[ i0 ].position;
            const float* b  = verts[ i1 ].position;
            const float* d  = verts[ i2 ].position;
            const FVector3 ab{ b[ 0 ] - a[ 0 ], b[ 1 ] - a[ 1 ], b[ 2 ] - a[ 2 ] };
            const FVector3 ad{ d[ 0 ] - a[ 0 ], d[ 1 ] - a[ 1 ], d[ 2 ] - a[ 2 ] };
            const double   ta = 0.5 * ( ab ^ ad ).Length();
            g.area += ta;
            for( int k = 0; k < 3; k++ )
            {
                c[ k ] += ( a[ k ] + b[ k ] + d[ k ] ) / 3.0 * ta;
                rgb[ k ] += e[ k ] * ta;
            }
        }
        for( int k = 0; k < 3; k++ )
        {
            g.centre[ k ] = g.area > 0 ? float( c[ k ] / g.area ) : 0.f;
            g.rgb[ k ]    = g.area > 0 ? float( rgb[ k ] / g.area ) : 0.f;
        }
        if( m_voxGlowCache.size() > 4096 )
        {
            m_voxGlowCache.clear();
        }
        it = m_voxGlowCache.emplace( key, g ).first;
    }
    const VoxelGlowGeo& g = it->second;
    if( g.area <= 0 )
    {
        return;
    }

    const auto&    m = transform.matrix;
    const FVector3 centre{
        m[ 0 ][ 0 ] * g.centre[ 0 ] + m[ 0 ][ 1 ] * g.centre[ 1 ] + m[ 0 ][ 2 ] * g.centre[ 2 ] + m[ 0 ][ 3 ],
        m[ 1 ][ 0 ] * g.centre[ 0 ] + m[ 1 ][ 1 ] * g.centre[ 1 ] + m[ 1 ][ 2 ] * g.centre[ 2 ] + m[ 1 ][ 3 ],
        m[ 2 ][ 0 ] * g.centre[ 0 ] + m[ 2 ][ 1 ] * g.centre[ 1 ] + m[ 2 ][ 2 ] * g.centre[ 2 ] + m[ 2 ][ 3 ],
    };
    // the model's scale (the voxel size): areas by its square
    const double det = m[ 0 ][ 0 ] * ( m[ 1 ][ 1 ] * m[ 2 ][ 2 ] - m[ 1 ][ 2 ] * m[ 2 ][ 1 ] ) -
                       m[ 0 ][ 1 ] * ( m[ 1 ][ 0 ] * m[ 2 ][ 2 ] - m[ 1 ][ 2 ] * m[ 2 ][ 0 ] ) +
                       m[ 0 ][ 2 ] * ( m[ 1 ][ 0 ] * m[ 2 ][ 1 ] - m[ 1 ][ 1 ] * m[ 2 ][ 0 ] );
    const double s2   = std::pow( std::abs( det ), 2.0 / 3.0 );
    const float  area = float( g.area * s2 );

    const float lum = 0.2126f * g.rgb[ 0 ] + 0.7152f * g.rgb[ 1 ] + 0.0722f * g.rgb[ 2 ];
    if( lum < 0.01f )
    {
        return;
    }
    // as a glowing sprite's light (AddEmissionLight), but from the glowing faces only (a
    // sprite counts its whole quad, dimmed by its average): the flame's surface is
    // small, and all of it glows -- *4 brings a voxel candle to its sprite's light
    float lumens = 4.f * scale * *rt_emis_light_scale * std::sqrt( lum ) * area *
                   RT_LightReachFade( { centre.X, centre.Y, centre.Z }, rtstate.get_sector() ) *
                   std::max( 0.f, float( *rt_sprite_light_scale ) );
    if( lumens < 0.5f )
    {
        return;
    }
    const float mx    = std::max( { g.rgb[ 0 ], g.rgb[ 1 ], g.rgb[ 2 ] } );
    auto        light = RgLightSphericalEXT{
        .sType     = RG_STRUCTURE_TYPE_LIGHT_SPHERICAL_EXT,
        .pNext     = nullptr,
        .color     = rt.rgUtilPackColorFloat4D( g.rgb[ 0 ] / mx, g.rgb[ 1 ] / mx, g.rgb[ 2 ] / mx, 1.f ),
        .intensity = lumens,
        .position  = { centre.X, centre.Y, centre.Z },
        .radius    = std::clamp( float( std::sqrt( area ) ) * 0.3f, 0.03f, 0.15f ),
    };
    auto info = RgLightInfo{
        .sType        = RG_STRUCTURE_TYPE_LIGHT_INFO,
        .pNext        = &light,
        .uniqueID     = id,
        .isExportable = false,
    };
    RG_CHECK( RT_UploadLight( &info ) );
}

void RTRenderState::AddEmissionLight( std::span< const RgPrimitiveVertex > verts,
                                      std::span< const uint32_t >          indices,
                                      const RgTransform&                   transform,
                                      const float                          avg[ 3 ],
                                      float                                scale,
                                      uint64_t                             id )
{
    // a faint glow (graffiti, a few lit pixels) glows on screen but lights nothing:
    // through the square root below even a trace of glow would become a visible light
    const float lum = 0.2126f * avg[ 0 ] + 0.7152f * avg[ 1 ] + 0.0722f * avg[ 2 ];
    if( lum < 0.01f )
    {
        return;
    }

    auto xf = [ &transform ]( const float* p ) {
        const auto& m = transform.matrix;
        return FVector3{ m[ 0 ][ 0 ] * p[ 0 ] + m[ 0 ][ 1 ] * p[ 1 ] + m[ 0 ][ 2 ] * p[ 2 ] + m[ 0 ][ 3 ],
                         m[ 1 ][ 0 ] * p[ 0 ] + m[ 1 ][ 1 ] * p[ 1 ] + m[ 1 ][ 2 ] * p[ 2 ] + m[ 1 ][ 3 ],
                         m[ 2 ][ 0 ] * p[ 0 ] + m[ 2 ][ 1 ] * p[ 1 ] + m[ 2 ][ 2 ] * p[ 2 ] + m[ 2 ][ 3 ] };
    };

    // area and centre (area weighted) of the triangles
    const size_t triCount = indices.empty() ? verts.size() / 3 : indices.size() / 3;
    double       area     = 0;
    FVector3     centre{ 0, 0, 0 };
    for( size_t t = 0; t < triCount; t++ )
    {
        size_t i0 = t * 3, i1 = t * 3 + 1, i2 = t * 3 + 2;
        if( !indices.empty() )
        {
            i0 = indices[ i0 ];
            i1 = indices[ i1 ];
            i2 = indices[ i2 ];
        }
        if( i0 >= verts.size() || i1 >= verts.size() || i2 >= verts.size() )
        {
            continue;
        }
        const FVector3 a = xf( verts[ i0 ].position ), b = xf( verts[ i1 ].position ), c = xf( verts[ i2 ].position );
        const double   ta = 0.5 * ( ( b - a ) ^ ( c - a ) ).Length();
        area += ta;
        centre += ( a + b + c ) * float( ta / 3.0 );
    }
    if( area < 1e-4 )
    {
        return;
    }
    centre /= float( area );

    // the renderer's normal (GL axes: x, up, y), turned to the side the surface is
    // seen from: a sign sprite's normal can point into the wall it hangs on
    const FVector4& gn = mStreamData.uVertexNormal;
    FVector3        n{ gn.X, gn.Z, gn.Y };
    if( n.LengthSquared() < 0.25f )
    {
        return;
    }
    n.MakeUnit();
    {
        // The side of the surface the light goes on: the one inside the surface's own
        // sector. (Not the camera's side: walls are drawn from behind too, and a screen
        // in the next room would put its light on this side of the wall.)
        const int   sec   = rtstate.get_sector();
        const float probe = 0.3f;
        const bool  front = RT_PointInSector( sec, centre.X + n.X * probe, centre.Y + n.Y * probe );
        const bool  back  = RT_PointInSector( sec, centre.X - n.X * probe, centre.Y - n.Y * probe );
        if( !front && back )
        {
            n = -n;
        }
        else if( rtstate.is< RtPrim::CeilingPlane >() || rtstate.is< RtPrim::FloorPlane >() )
        {
            // a ceiling lamp shines down into its room, a floor's glow up (RT: z up)
            const bool ceiling = rtstate.is< RtPrim::CeilingPlane >();
            if( ceiling ? n.Z > 0.f : n.Z < 0.f )
            {
                n = -n;
            }
        }
        else if( front == back && ( rtstate.is< RtPrim::Decal >() || rtstate.is< RtPrim::Sprite >() ) )
        {
            // a free-standing two-sided sprite: the side it is seen from
            const RgFloat3D& cam = m_fb->RT_CameraPosition();
            const FVector3   toCam{ cam.data[ 0 ] - centre.X, cam.data[ 1 ] - centre.Y, cam.data[ 2 ] - centre.Z };
            if( ( n | toCam ) < 0.f )
            {
                n = -n;
            }
        }
    }

    // by the square root of the average brightness: a neon sign is mostly dark
    // texels, and its few bright tubes must still light the wall it is on
    float lumens = scale * *rt_emis_light_scale * std::sqrt( lum ) * float( area );
    if( rtstate.is< RtPrim::Sprite >() )
    {
        // a glowing billboard (a candle flame): a small ball just in front of it, and
        // only near the view -- a level has hundreds of them
        lumens *= RT_LightReachFade( { centre.X, centre.Y, centre.Z }, rtstate.get_sector() ) *
                  std::max( 0.f, float( *rt_sprite_light_scale ) );
    }
    if( lumens < 0.5f )
    {
        return;
    }

    // the colour of the glow, at full brightness
    const float     mx    = std::max( { avg[ 0 ], avg[ 1 ], avg[ 2 ] } );
    const RgColor4DPacked32 color = rt.rgUtilPackColorFloat4D( avg[ 0 ] / mx, avg[ 1 ] / mx, avg[ 2 ] / mx, 1.f );

    // NEON (a strongly coloured glow) on a wall must reach the wall around it: real
    // neon stands off the wall and shines every way, and a spot facing out from the
    // wall leaves the wall and the floor below at the edge of its cone -- so it is a
    // sphere a little way out. Everything else -- screens, panels, ceiling and floor
    // fixtures -- shines out from its surface, not back onto it: a spot.
    const float mn       = std::min( { avg[ 0 ], avg[ 1 ], avg[ 2 ] } );
    const bool  neon     = mx > 0.f && ( mx - mn ) / mx > 0.75f;
    const bool  vertical = ( neon && ( std::abs( n.Z ) < 0.7f || rtstate.is< RtPrim::Decal >() ) ) ||
                          rtstate.is< RtPrim::Sprite >();
    if( vertical )
    {
        // (a candle's or a torch's a little way out from it: right by the flame it would
        // only light up the wall it is on)
        const float    out = rtstate.is< RtPrim::Sprite >() ? 0.3f : 0.25f;
        const FVector3 p   = centre + n * out;
        auto sphere = RgLightSphericalEXT{
            .sType     = RG_STRUCTURE_TYPE_LIGHT_SPHERICAL_EXT,
            .pNext     = nullptr,
            .color     = color,
            .intensity = lumens,
            .position  = { p.X, p.Y, p.Z },
            .radius    = std::clamp( float( std::sqrt( area ) ) * 0.2f, rtstate.is< RtPrim::Sprite >() ? 0.08f : 0.03f, 0.18f ),
        };
        auto info = RgLightInfo{
            .sType        = RG_STRUCTURE_TYPE_LIGHT_INFO,
            .pNext        = &sphere,
            .uniqueID     = id,
            .isExportable = false,
        };
        RG_CHECK( RT_UploadLight( &info ) );
        return;
    }

    const float    offset = 0.12f;
    const FVector3 pos    = centre + n * offset;

    auto spot = RgLightSpotEXT{
        .sType      = RG_STRUCTURE_TYPE_LIGHT_SPOT_EXT,
        .pNext      = nullptr,
        .color      = color,
        .intensity  = lumens,
        .position   = { pos.X, pos.Y, pos.Z },
        .direction  = { n.X, n.Y, n.Z },
        .radius     = std::clamp( float( std::sqrt( area ) ) * 0.3f, 0.03f, 0.5f ),
        .angleOuter = 89.f * 3.14159265f / 180.f,
        .angleInner = 60.f * 3.14159265f / 180.f,
    };
    auto info = RgLightInfo{
        .sType        = RG_STRUCTURE_TYPE_LIGHT_INFO,
        .pNext        = &spot,
        .uniqueID     = id,
        .isExportable = false,
    };
    RG_CHECK( RT_UploadLight( &info ) );
}

} // namespace rtx

void RT_UploadFixtureQuad( uint64_t id, const float corners[ 4 ][ 3 ], const float normal[ 3 ], const char* tileName, float glow )
{
    using namespace rtx;
    FTextureID texid = TexMan.CheckForTexture( tileName, ETextureType::Any );
    FGameTexture* gametex = texid.isValid() ? TexMan.GetGameTexture( texid ) : nullptr;
    FTexture*     base    = gametex ? gametex->GetTexture() : nullptr;
    if( !base )
    {
        return;
    }
    auto hwtex = static_cast< RTHardwareTexture* >( base->GetHardwareTexture( 0, 0 ) );
    if( !hwtex )
    {
        return;
    }
    hwtex->CreateIfWasnt( *gametex, 0, 0, LegacyRenderStyles[ STYLE_Normal ] );

    const RgNormalPacked32 np = rt.rgUtilPackNormal( normal[ 0 ], normal[ 1 ], normal[ 2 ] );
    static const float     uv[ 4 ][ 2 ] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
    RgPrimitiveVertex      v[ 4 ];
    for( int i = 0; i < 4; i++ )
    {
        v[ i ] = RgPrimitiveVertex{
            .position     = { corners[ i ][ 0 ], corners[ i ][ 1 ], corners[ i ][ 2 ] },
            .normalPacked = np,
            .texCoord     = { uv[ i ][ 0 ], uv[ i ][ 1 ] },
            .color        = RG_PACKED_COLOR_WHITE,
        };
    }
    static const uint32_t idx[ 6 ] = { 0, 1, 2, 0, 2, 3 };

    const bool emis  = hwtex->HasEmission();
    auto       mesh  = RgMeshInfo{
        .sType                = RG_STRUCTURE_TYPE_MESH_INFO,
        .pNext                = nullptr,
        .flags                = 0,
        .uniqueObjectID       = FRtState::MixId( id, RtIdKind::Other ),
        .pMeshName            = nullptr,
        .transform            = RT_TRANSFORM_IDENTITY,
        .isExportable         = false,
        .animationTime        = 0.0f,
        .localLightsIntensity = 1.0f,
    };
    auto prim = RgMeshPrimitiveInfo{
        .sType                = RG_STRUCTURE_TYPE_MESH_PRIMITIVE_INFO,
        .pNext                = nullptr,
        .flags                = RgMeshPrimitiveFlags( RG_MESH_PRIMITIVE_NO_SHADOW | RG_MESH_PRIMITIVE_FORCE_EXACT_NORMALS |
                                                      RG_MESH_PRIMITIVE_ALPHA_TESTED |
                                                      ( emis ? RG_MESH_PRIMITIVE_EMISSIVE_OVERRIDE | RG_MESH_PRIMITIVE_EMISSIVE_SCREEN_SCALED : 0 ) ),
        .primitiveIndexInMesh = 0,
        .pVertices            = v,
        .vertexCount          = 4,
        .pIndices             = idx,
        .indexCount           = 6,
        .pTextureName         = hwtex->GetRTName(),
        .textureFrame         = 0,
        .color                = RG_PACKED_COLOR_WHITE,
        .emissive             = glow,
        .classicLight         = 1.f,
        .emissiveGi           = glow * 0.25f,
    };
    rt.rgUploadMeshPrimitive( &mesh, &prim );
}
