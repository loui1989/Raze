#pragma once

// Raze RT: the hardware renderer interfaces (DFrameBuffer, FRenderState and the
// buffers and textures behind them) implemented over RTGL1.
//
// Adapted from gzdoom-rt (Doom 64 RT): rt_buffers.h and rt_renderstate.h, minus
// the Doom 64 specific parts.

#include "rt_internal.h"

#include "gl_sysfb.h"
#include "hw_renderstate.h"
#include "hw_viewpointbuffer.h"
#include "hw_viewpointuniforms.h"
#include "flatvertices.h"
#include "hw_skydome.h"
#include "i_modelvertexbuffer.h"
#include "v_2ddrawer.h"
#include "v_draw.h"
#include "image.h"
#include "textures.h"
#include "gametexture.h"
#include "hw_material.h"
#include "palettecontainer.h"
#include "filesystem.h"

#include <span>
#include <string>
#include <variant>
#include <vector>

namespace rtx
{

//
// Buffers: plain CPU memory. RTGL1 takes the vertices of each draw as it is made.
//

class VectorAsBuffer : virtual public IBuffer
{
public:
    ~VectorAsBuffer() override = default;

    void SetSubData( size_t offset, size_t size, const void* data ) override
    {
        if( offset + size > m_buffer.size() )
        {
            m_buffer.resize( offset + size );
        }
        if( data )
        {
            memcpy( &m_buffer[ offset ], data, size );
        }
        buffersize = m_buffer.size();
        if( map )
        {
            map = m_buffer.data();
        }
    }
    void SetData( size_t size, const void* data, BufferUsageType type ) override
    {
        SetSubData( 0, size, data );
    }
    void* Lock( unsigned size ) override
    {
        SetSubData( 0, size, nullptr );
        return m_buffer.data();
    }
    void Unlock() override {}
    void Resize( size_t newsize ) override
    {
        m_buffer.resize( newsize );
        buffersize = m_buffer.size();
        if( map )
        {
            map = m_buffer.data();
        }
    }
    void Upload( size_t start, size_t size ) override {}
    void Map() override { map = m_buffer.data(); }
    void Unmap() override { map = nullptr; }
    void GPUDropSync() override {}
    void GPUWaitSync() override {}

protected:
    auto AccessBuffer() const { return std::span< const uint8_t >{ m_buffer }; }

private:
    std::vector< uint8_t > m_buffer;
};

class RTVertexBuffer
    : public IVertexBuffer
    , public VectorAsBuffer
{
    using Super = VectorAsBuffer;
    using VertexTypeHolder =
        std::variant< std::monostate, FSkyVertex, FModelVertex, FFlatVertex, F2DDrawer::TwoDVertex >;

public:
    void SetFormat( int                           numBindingPoints,
                    int                           numAttributes,
                    size_t                        stride,
                    const FVertexBufferAttribute* attrs ) override
    {
        static_assert( sizeof( FSkyVertex ) != sizeof( FModelVertex ) );
        static_assert( sizeof( FSkyVertex ) != sizeof( FFlatVertex ) );
        static_assert( sizeof( FSkyVertex ) != sizeof( F2DDrawer::TwoDVertex ) );
        static_assert( sizeof( FModelVertex ) != sizeof( FFlatVertex ) );
        static_assert( sizeof( FModelVertex ) != sizeof( F2DDrawer::TwoDVertex ) );
        static_assert( sizeof( FFlatVertex ) != sizeof( F2DDrawer::TwoDVertex ) );

        if( numBindingPoints == 1 && numAttributes == 4 && stride == sizeof( FSkyVertex ) )
        {
            m_vertextype = FSkyVertex{};
        }
        else if( numBindingPoints == 2 && numAttributes == 8 && stride == sizeof( FModelVertex ) )
        {
            m_vertextype = FModelVertex{};
        }
        else if( numBindingPoints == 1 && numAttributes == 3 && stride == sizeof( FFlatVertex ) )
        {
            m_vertextype = FFlatVertex{};
        }
        else if( numBindingPoints == 1 && numAttributes == 3 &&
                 stride == sizeof( F2DDrawer::TwoDVertex ) )
        {
            m_vertextype = F2DDrawer::TwoDVertex{};
        }
        else
        {
            Printf( "RT: unknown vertex format (%d binding points, %d attributes, stride %d)\n",
                    numBindingPoints,
                    numAttributes,
                    int( stride ) );
            m_vertextype = std::monostate{};
        }
        m_formatted.clear();
    }

    auto AccessFormatted( uint32_t first, uint32_t count ) -> std::span< const RgPrimitiveVertex >;

    void SetData( size_t size, const void* data, BufferUsageType type ) override
    {
        m_formatted.clear();
        Super::SetData( size, data, type );
    }
    void SetSubData( size_t offset, size_t size, const void* data ) override
    {
        m_formatted.clear();
        Super::SetSubData( offset, size, data );
    }
    void* Lock( unsigned size ) override
    {
        m_formatted.clear();
        return Super::Lock( size );
    }
    void Map() override
    {
        m_formatted.clear();
        Super::Map();
    }
    void Unmap() override
    {
        m_formatted.clear();
        Super::Unmap();
    }

    bool IsSky() const { return std::holds_alternative< FSkyVertex >( m_vertextype ); }
    bool IsUI() const { return std::holds_alternative< F2DDrawer::TwoDVertex >( m_vertextype ); }
    bool IsModel() const { return std::holds_alternative< FModelVertex >( m_vertextype ); }

private:
    VertexTypeHolder                 m_vertextype;
    std::vector< RgPrimitiveVertex > m_formatted;
};

class RTIndexBuffer
    : public IIndexBuffer
    , public VectorAsBuffer
{
    using IndexType = uint32_t;

public:
    auto AccessFormatted( uint32_t first, uint32_t count ) -> std::span< const IndexType >
    {
        const auto rawbuf = AccessBuffer();
        if( sizeof( IndexType ) * ( size_t( first ) + count ) > rawbuf.size_bytes() )
        {
            return {};
        }
        return std::span{ reinterpret_cast< const IndexType* >( rawbuf.data() ) + first, count };
    }

    static auto CalcFirstVertexAndVertexCount( std::span< const IndexType > indices )
        -> std::pair< uint32_t, uint32_t >
    {
        if( indices.empty() )
        {
            return { 0, 0 };
        }
        uint32_t imin = std::numeric_limits< uint32_t >::max();
        uint32_t imax = 0;
        for( const auto& i : indices )
        {
            imin = std::min( imin, i );
            imax = std::max( imax, i );
        }
        return { imin, imax - imin + 1 };
    }

    auto MakeWithNewFirstIndex( std::span< const IndexType > indices, IndexType newFirst )
        -> std::span< const IndexType >
    {
        m_cache.clear();
        m_cache.reserve( indices.size() );
        for( const auto& i : indices )
        {
            m_cache.push_back( i - newFirst );
        }
        return m_cache;
    }

private:
    std::vector< IndexType > m_cache;
};

//
// Textures: given to RTGL1 by name the first time they are drawn
//

class RTHardwareTexture : public IHardwareTexture
{
public:
    // only used by the software renderer
    unsigned int CreateTexture( unsigned char*, int, int, int, bool, const char* ) override { return 0; }
    void         AllocateBuffer( int, int, int ) override {}
    uint8_t*     MapBuffer() override { return nullptr; }

    ~RTHardwareTexture() override;

    void CreateIfWasnt( FGameTexture& src, int translation, int flags, const FRenderStyle& renderStyle );
    // the same picture, clamped at the edges (HUD and menus: wrapping would bleed the
    // opposite edge into a filtered border line)
    void CreateClampedIfWasnt( FGameTexture& src, int translation, int flags, const FRenderStyle& renderStyle );

    auto GetRTName() const -> const char*
    {
        return m_created && !m_name.empty() ? m_name.c_str() : nullptr;
    }
    auto GetRTNameClamped() const -> const char*
    {
        return m_createdClamped && !m_nameClamped.empty() ? m_nameClamped.c_str() : nullptr;
    }
    bool HasEmission() const { return m_hasEmission; }
    const float* EmissionAverage() const { return m_emisAvg; }
    bool IsPolished() const { return m_polished; }

private:
    static auto TranslationSuffix( int translation ) -> std::string;
    static auto MakeTextureName( FGameTexture& gametex, int translation ) -> std::string;
    bool Upload( FGameTexture& src, int translation, int flags, const FRenderStyle& renderStyle,
                 const std::string& name, bool clamp, bool materials );

    bool        m_created{ false };
    bool        m_createdClamped{ false };
    bool        m_dynamic{ false }; // no name of its own (movie frames...): unique name, freed with this
    bool        m_hasEmission{ false };
    float       m_emisAvg[ 3 ]{};
    bool        m_polished{ false };
    std::string m_name{};
    std::string m_nameClamped{};
};

// Duke-RT's material maps (the overlay's materials/glowmaps, roughness, metallic) as
// RTGL1 overrides in <rt>/mat_dev. Returns true if an emission map exists for 'name'.
struct RtMaterialInfo
{
    bool  hasEmission = false;
    float emisAvg[ 3 ]{}; // the emission map's average colour, linear
    bool  polished    = false;
};
bool RT_PrepareMaterialOverrides( FGameTexture& src, const std::string& name, const FTextureBuffer& albedo, RtMaterialInfo& out );

//
// The frame and its render state
//

class RTRenderState;

class RTFrameBuffer : public SystemBaseFrameBuffer
{
    using Super = SystemBaseFrameBuffer;

public:
    RTFrameBuffer( void* hMonitor, bool fullscreen );
    ~RTFrameBuffer() override;

    void InitializeState() override;
    void BeginFrame() override;
    void Update() override;

    FRenderState*     RenderState() override;
    IVertexBuffer*    CreateVertexBuffer() override;
    IIndexBuffer*     CreateIndexBuffer() override;
    IDataBuffer*      CreateDataBuffer( int bindingpoint, bool ssbo, bool needsresize ) override;
    IHardwareTexture* CreateHardwareTexture( int numchannels ) override;
    void              PrecacheMaterial( FMaterial* mat, int translation ) override;

    void SetVSync( bool vsync ) override { m_vsync = vsync; }
    void SetTextureFilterMode() override {}
    void Draw2D() override;
    int  Backend() override { return 3; }
    const char* DeviceName() const override { return "RTGL1"; }

    TArray< uint8_t > GetScreenshotBuffer( int& pitch, ESSType& color_type, float& gamma ) override;

    // called by the scene code at the start of the player's view
    void RT_SetMainCamera( const DVector3& pos, const FRotator& hwangles, float fovDeg, float ratio, float fovratio );

    void RT_MarkWasSky() { m_wassky = true; }
    const RgFloat3D& RT_CameraPosition() const { return m_camPos; }
    bool RT_IsFrameStarted() const { return m_frameStarted; }

private:
    void RT_StartFrame();
    void RT_DrawFrame();
    void RT_UploadLights();

    RTRenderState* m_state{ nullptr };
    bool           m_vsync{ false };
    bool           m_wassky{ false };
    bool           m_frameStarted{ false };
    bool           m_cameraUploaded{ false };

    RgFloat3D m_camPos{}, m_camUp{}, m_camRight{}, m_camForward{};
};

class RTRenderState : public FRenderState
{
public:
    explicit RTRenderState( RTFrameBuffer* parent ) : m_fb( parent ) {}
    ~RTRenderState() = default;

    void RT_BeginFrame()
    {
        rtstate.reset();
        m_drawsThisFrame    = 0;
        m_droppedThisFrame  = 0;
    }

    bool IsCurrentDrawIgnored() const
    {
        return rtstate.is< RtPrim::Ignored >() || mTextureMode == TM_FOGLAYER;
    }

    void Draw( int dt, int index, int count, bool apply = true ) override;
    void DrawIndexed( int dt, int index, int count, bool apply = true ) override;

    void ClearScreen() override {}
    bool SetDepthClamp( bool on ) override { return on; }
    void SetDepthMask( bool on ) override {}
    void SetDepthFunc( int func ) override {}
    void SetDepthRange( float min, float max ) override {}
    void SetColorMask( bool r, bool g, bool b, bool a ) override {}
    void SetStencil( int offs, int op, int flags = -1 ) override {}
    void SetCulling( int mode ) override {}
    void EnableClipDistance( int num, bool state ) override {}
    void Clear( int targets ) override {}
    void EnableStencil( bool on ) override {}
    void SetScissor( int x, int y, int w, int h ) override
    {
        m_scissor[ 0 ] = x;
        m_scissor[ 1 ] = y;
        m_scissor[ 2 ] = w;
        m_scissor[ 3 ] = h;
    }
    void SetViewport( int x, int y, int w, int h ) override
    {
        m_viewport = RgViewport{
            .x        = float( x ),
            .y        = float( y ),
            .width    = float( w ),
            .height   = float( h ),
            .minDepth = 0.0f,
            .maxDepth = 1.0f,
        };
    }
    void EnableDepthTest( bool on ) override {}
    void EnableMultisampling( bool on ) override {}
    void EnableLineSmooth( bool on ) override {}
    void EnableDrawBuffers( int count, bool apply ) override {}

    // the matrices of the viewpoint that is bound (HWViewpointBuffer)
    void RT_SetMatrices( const VSMatrix& view, const VSMatrix& proj );
    // what RTGL1 made of the main camera
    void RT_SetMainCameraInverse( const float* viewInverse, const float* projInverse )
    {
        memcpy( m_mainCameraView_Inverse, viewInverse, sizeof( m_mainCameraView_Inverse ) );
        memcpy( m_mainCameraProjection_Inverse, projInverse, sizeof( m_mainCameraProjection_Inverse ) );
    }

    uint32_t m_drawsThisFrame{ 0 };
    uint32_t m_droppedThisFrame{ 0 };

private:
    void InternalDraw( std::span< const RgPrimitiveVertex > verts,
                       std::span< const uint32_t >          indices,
                       bool                                 isUI,
                       bool                                 islines = false );

    auto MakeTransform( bool isSky, bool isModel ) const -> RgTransform;
    auto ResolveTexture( bool clamped, const RTHardwareTexture** outTex ) -> const char*;
    void AddEmissionLight( std::span< const RgPrimitiveVertex > verts,
                           std::span< const uint32_t >          indices,
                           const RgTransform&                   transform,
                           const float                          avg[ 3 ],
                           float                                scale,
                           uint64_t                             id );
    auto ClipToScissor( std::span< const RgPrimitiveVertex > verts, std::span< const uint32_t > indices )
        -> std::span< const RgPrimitiveVertex >;
    auto FallbackUniqueId( std::span< const RgPrimitiveVertex > verts, const char* texname ) const -> uint64_t;

    RgViewport m_viewport{};
    float      m_view[ 16 ]{};
    float      m_projection[ 16 ]{};
    float      m_orthoSize[ 2 ]{}; // the 2D drawer's size, from its orthographic projection
    int        m_scissor[ 4 ]{ -1, -1, -1, -1 };

    float m_mainCameraView_Inverse[ 16 ]{};
    float m_mainCameraProjection_Inverse[ 16 ]{};

    std::vector< RgPrimitiveVertex > m_tempverts{};
    std::vector< RgPrimitiveVertex > m_clipverts{};

public:
    RTFrameBuffer* m_fb{ nullptr };
};

class RTDataBuffer
    : public IDataBuffer
    , public VectorAsBuffer
{
public:
    void BindRange( FRenderState* state, size_t start, size_t length ) override;
};

} // namespace rtx

#include "rt_platform.h"
