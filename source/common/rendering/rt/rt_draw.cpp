// Raze RT: every primitive the hardware renderer draws goes through
// RTRenderState::Draw / DrawIndexed and ends up in InternalDraw, which
// classifies it, resolves its texture and hands it to RTGL1.
//
// Adapted from gzdoom-rt's rt_draw.cpp / rt_buffers.h (Doom 64 RT), without the
// Doom 64 specific parts.

#include "rt_backend.h"
#include "texturemanager.h"


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

auto RTHardwareTexture::MakeTextureName( FGameTexture& gametex, int translation ) -> std::string
{
    const std::string suffix = TranslationSuffix( translation );

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

    // RTGL1 reads the override files when the texture is given, so they come first
    if( materials && !( flags & CTF_Indexed ) )
    {
        RtMaterialInfo mi;
        m_hasEmission = RT_PrepareMaterialOverrides( src, name, texbuffer, mi );
        memcpy( m_emisAvg, mi.emisAvg, sizeof( m_emisAvg ) );
        m_polished = mi.polished;
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
    if( m_name.empty() )
    {
        // RTGL1 keeps the first texture given a name, so a texture whose content is
        // replaced (a movie frame) must never reuse one
        static uint64_t serial = 0;
        m_name    = "rtdyn" + std::to_string( ++serial );
        m_dynamic = true;
    }

    if( !Upload( src, translation, flags, renderStyle, m_name, false, !m_dynamic ) )
    {
        m_name.clear();
    }
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

    const RgTransform transform = isUI ? RT_TRANSFORM_IDENTITY : MakeTransform( isSky, isModel );

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
        else if( rtstate.is< RtPrim::Glass >() )
        {
            primFlags |= RG_MESH_PRIMITIVE_GLASS;
        }
        else if( rtstate.is< RtPrim::Water >() )
        {
            primFlags |= RG_MESH_PRIMITIVE_WATER;
        }
        else if( rtstate.is< RtPrim::Lava >() )
        {
            primFlags |= RG_MESH_PRIMITIVE_LAVA;
        }
        else if( rtstate.is< RtPrim::Slime >() )
        {
            primFlags |= RG_MESH_PRIMITIVE_ACID;
        }

        if( additive && !isMirror )
        {
            primFlags |= RG_MESH_PRIMITIVE_TRANSLUCENT;
            emissive = *rt_additive_emis;
        }
        else if( mAlphaThreshold > 0.f && texname && !isMirror )
        {
            primFlags |= RG_MESH_PRIMITIVE_ALPHA_TESTED;
        }

        const bool isSprite = rtstate.is< RtPrim::Sprite >();
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
            if( *rt_emis_follow_shade )
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
            emissive   = glowScale;
            emissiveGi = glowScale * float( *rt_emis_mult ) * ( *rt_emis_lights ? float( *rt_emis_gi ) : 1.f );
        }
        else
        {
            if( rtstate.is< RtPrim::Fullbright >() && *rt_fullbright_emis > 0.f )
            {
                emissive = std::max( emissive, float( *rt_fullbright_emis ) );
            }
            // a floor under the path traced light, like Duke-RT's sector ambient: the
            // surface glows faintly with its sector's classic light level, so a room
            // the lights miss is dark rather than black (and it lights its
            // surroundings a little, through the bounce light)
            if( *rt_ambient > 0.f && !isMirror && !rtstate.is< RtPrim::Glass >() && !rtstate.is< RtPrim::Water >() )
            {
                // a switched-off room is darker than its half-light shade says
                const float sw = RT_SectorSwitch( rtstate.get_sector() );
                emissive = std::max( emissive, float( *rt_ambient ) * classic * ( sw < 0.f ? 1.f : 0.35f + 0.65f * sw ) );
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
    }

    // ids
    uint64_t uniqueId = rtstate.has_uniqueid() ? rtstate.get_uniqueid() : FallbackUniqueId( verts, texname );
    if( isUI && !rtstate.has_uniqueid() )
    {
        uniqueId = FRtState::MixId( 0x2D, RtIdKind::UI );
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
        .flags                = 0,
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

    RgResult r = rt.rgUploadMeshPrimitive( &mesh, &prim );
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
    if( emissionMap && *rt_emis_lights && !rtstate.is< RtPrim::Sprite >() && !isUI && !isSky && glowScale > 0.01f )
    {
        AddEmissionLight( verts, indices, transform, hwtex->EmissionAverage(), glowScale * emisLightScale,
                          FRtState::MixId( uniqueId * 31 + prim.primitiveIndexInMesh, RtIdKind::Other ) );
    }
}

void RTRenderState::AddEmissionLight( std::span< const RgPrimitiveVertex > verts,
                                      std::span< const uint32_t >          indices,
                                      const RgTransform&                   transform,
                                      const float                          avg[ 3 ],
                                      float                                scale,
                                      uint64_t                             id )
{
    const float lum = 0.2126f * avg[ 0 ] + 0.7152f * avg[ 1 ] + 0.0722f * avg[ 2 ];
    if( lum <= 1e-4f )
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
        else if( front == back && rtstate.is< RtPrim::Decal >() )
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
    const float lumens = scale * *rt_emis_light_scale * std::sqrt( lum ) * float( area );
    if( lumens < 0.5f )
    {
        return;
    }

    // the colour of the glow, at full brightness
    const float     mx    = std::max( { avg[ 0 ], avg[ 1 ], avg[ 2 ] } );
    const RgColor4DPacked32 color = rt.rgUtilPackColorFloat4D( avg[ 0 ] / mx, avg[ 1 ] / mx, avg[ 2 ] / mx, 1.f );

    // On a wall (a sign, a screen) the light must reach the wall around it: real neon
    // stands off the wall and shines every way. A spot facing out from the wall lights
    // only what is straight across, and the wall itself and the floor below it sit at
    // the edge of its cone -- so there it is a sphere a little way out. Ceiling and
    // floor fixtures stay a spot pointing away from the surface (no light up into
    // the ceiling they hang from).
    const bool vertical = std::abs( n.Z ) < 0.7f || rtstate.is< RtPrim::Decal >();
    if( vertical )
    {
        const float    out = 0.25f;
        const FVector3 p   = centre + n * out;
        auto sphere = RgLightSphericalEXT{
            .sType     = RG_STRUCTURE_TYPE_LIGHT_SPHERICAL_EXT,
            .pNext     = nullptr,
            .color     = color,
            .intensity = lumens,
            .position  = { p.X, p.Y, p.Z },
            .radius    = std::clamp( float( std::sqrt( area ) ) * 0.2f, 0.03f, 0.18f ),
        };
        auto info = RgLightInfo{
            .sType        = RG_STRUCTURE_TYPE_LIGHT_INFO,
            .pNext        = &sphere,
            .uniqueID     = id,
            .isExportable = false,
        };
        RG_CHECK( rt.rgUploadLight( &info ) );
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
    RG_CHECK( rt.rgUploadLight( &info ) );
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
