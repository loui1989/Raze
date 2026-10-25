// Raze RT: every primitive the hardware renderer draws goes through
// RTRenderState::Draw / DrawIndexed and ends up in InternalDraw, which
// classifies it, resolves its texture and hands it to RTGL1.
//
// Adapted from gzdoom-rt's rt_draw.cpp / rt_buffers.h (Doom 64 RT), without the
// Doom 64 specific parts.

#include "rt_backend.h"


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
    if( m_dynamic && !m_name.empty() && g_rtActive && rt.rgMarkOriginalTextureAsDeleted )
    {
        rt.rgMarkOriginalTextureAsDeleted( m_name.c_str() );
    }
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

    auto texbuffer = src.GetTexture()->CreateTexBuffer( translation, flags | CTF_ProcessData );
    if( !texbuffer.mBuffer || texbuffer.mWidth <= 0 || texbuffer.mHeight <= 0 )
    {
        m_name.clear();
        return;
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

    auto details = RgOriginalTextureDetailsEXT{
        .sType  = RG_STRUCTURE_TYPE_ORIGINAL_TEXTURE_DETAILS_EXT,
        .pNext  = nullptr,
        .flags  = 0,
        .format = ( flags & CTF_Indexed ) ? RG_FORMAT_R8_SRGB : RG_FORMAT_B8G8R8A8_SRGB,
    };

    auto info = RgOriginalTextureInfo{
        .sType        = RG_STRUCTURE_TYPE_ORIGINAL_TEXTURE_INFO,
        .pNext        = &details,
        .pTextureName = m_name.c_str(),
        .pPixels      = texbuffer.mBuffer,
        .size         = { uint32_t( texbuffer.mWidth ), uint32_t( texbuffer.mHeight ) },
        .filter       = RG_SAMPLER_FILTER_AUTO,
        .addressModeU = RG_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeV = RG_SAMPLER_ADDRESS_MODE_REPEAT,
    };

    RG_CHECK( rt.rgProvideOriginalTexture( &info ) );
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
}

auto RTRenderState::ResolveTexture() -> const char*
{
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
    hwtex->CreateIfWasnt( *gametex, mMaterial.mTranslation, scaleflags, mRenderStyle );
    return hwtex->GetRTName();
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

    const char* texname = ( isSkyVis || isMirror ) ? nullptr : ResolveTexture();

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

    RgMeshPrimitiveFlags primFlags = 0;
    float                emissive  = 0.f;

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

        if( rtstate.is< RtPrim::Sprite >() )
        {
            primFlags |= RG_MESH_PRIMITIVE_SPRITE;
        }
        if( rtstate.is< RtPrim::Fullbright >() && *rt_fullbright_emis > 0.f )
        {
            emissive = std::max( emissive, float( *rt_fullbright_emis ) );
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

    // classic (non path traced) brightness, for rt_classic: the soft light level
    float classic = 1.0f;
    if( !isUI && mLightParms[ 3 ] >= 0.f )
    {
        classic = std::clamp( mLightParms[ 3 ], 0.f, 1.f );
    }

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
        .emissiveGi           = 0.f,
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
}

} // namespace rtx
