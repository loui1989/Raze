// Raze RT: material maps for RTGL1.
//
// RTGL1 finds a texture's extra maps by name: <rt>/mat_dev/<name>_e.png (emission),
// <name>_orm.png (occlusion, roughness, metallic) and so on, read when the texture is
// given to it. Raze attaches Duke-RT's overlay maps (materials/glowmaps/auto/#01234.png
// and friends, mounted with -file) to the tile textures as material layers; here they
// become those files:
//
//  - emission: the texture's colours where the glowmap (or, without one, the
//    palette's fullbright brightmap) says it glows. Made from the translated pixels,
//    so a palette swapped sign glows in its own colour.
//  - roughness / metallic: packed into _orm (R unused, G roughness, B metallic).
//
// A file is made once and kept; rt_materials_regen 1 makes them again.

#include "rt_backend.h"

#include "m_png.h"
#include "files.h"

#include <filesystem>
#include <unordered_set>

CVARD( Bool, rt_materials, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: use the material maps (glow, roughness, metallic) of the mounted overlay" )
CVARD( Bool, rt_materials_brightmaps, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: textures without a glowmap glow where the palette is fullbright" )
CVARD( Bool, rt_materials_regen, false, 0, "RT: make the material files again (this session)" )

const std::string& RT_GetOverrideFolder();

namespace
{

struct Mask
{
    TArray< uint8_t > buf; // BGRA
    int               w = 0, h = 0;

    bool Load( FTexture* tex )
    {
        if( !tex )
        {
            return false;
        }
        auto b = tex->CreateTexBuffer( 0, 0 );
        if( !b.mBuffer || b.mWidth <= 0 || b.mHeight <= 0 )
        {
            return false;
        }
        w = b.mWidth;
        h = b.mHeight;
        buf.Resize( unsigned( w * h * 4 ) );
        memcpy( buf.Data(), b.mBuffer, size_t( w ) * h * 4 );
        return true;
    }

    // luminance 0..255 at the texture coordinate of pixel (x, y) of a w2 x h2 picture
    int At( int x, int y, int w2, int h2 ) const
    {
        const int sx = std::min( w - 1, x * w / std::max( 1, w2 ) );
        const int sy = std::min( h - 1, y * h / std::max( 1, h2 ) );
        const uint8_t* p = &buf[ unsigned( ( sy * w + sx ) * 4 ) ];
        // weighted like Rec.709, BGRA order; transparent counts as nothing
        const int l = ( 54 * p[ 2 ] + 183 * p[ 1 ] + 19 * p[ 0 ] ) >> 8;
        return p[ 3 ] < 128 ? 0 : l;
    }
};

bool WritePNG( const std::filesystem::path& path, const std::vector< uint8_t >& rgb, int w, int h )
{
    std::error_code ec;
    std::filesystem::create_directories( path.parent_path(), ec );
    FileWriter* f = FileWriter::Open( path.string().c_str() );
    if( !f )
    {
        return false;
    }
    const bool ok = M_CreatePNG( f, rgb.data(), nullptr, SS_RGB, w, h, w * 3, 1.0f ) && M_FinishPNG( f );
    delete f;
    return ok;
}

std::unordered_set< std::string > g_made; // this session

} // namespace

const std::string& RT_GetOverrideFolder();

bool rtx::RT_PrepareMaterialOverrides( FGameTexture& src, const std::string& name, const FTextureBuffer& albedo, float outAvg[ 3 ] )
{
    outAvg[ 0 ] = outAvg[ 1 ] = outAvg[ 2 ] = 0.f;
    if( !*rt_materials || name.empty() || !albedo.mBuffer )
    {
        return false;
    }

    const std::filesystem::path dir = std::filesystem::path( RT_GetOverrideFolder() ) / "mat_dev";
    const auto pathE   = dir / ( name + "_e.png" );
    const auto pathORM = dir / ( name + "_orm.png" );
    const auto pathNoE = dir / ( name + "_e.v2none" ); // looked, nothing glows
    const auto pathAvg = dir / ( name + "_e.v2avg" );  // the emission map's average colour (linear)

    const bool regen = *rt_materials_regen && g_made.insert( name ).second;

    std::error_code ec;
    bool hasE = false;
    if( !regen && std::filesystem::exists( pathE, ec ) )
    {
        if( FILE* f = fopen( pathAvg.string().c_str(), "rb" ) )
        {
            hasE = fscanf( f, "%f %f %f", &outAvg[ 0 ], &outAvg[ 1 ], &outAvg[ 2 ] ) == 3;
            fclose( f );
        }
    }
    bool noE    = !regen && !hasE && std::filesystem::exists( pathNoE, ec );
    bool hasORM = !regen && std::filesystem::exists( pathORM, ec );

    const FMaterialLayers* layers = src.GetMaterialLayers();
    const int              w      = albedo.mWidth;
    const int              h      = albedo.mHeight;

    // emission
    if( !hasE && !noE )
    {
        Mask mask;
        bool have = layers && layers->Glowmap.get() && mask.Load( layers->Glowmap.get() );
        if( !have && *rt_materials_brightmaps )
        {
            have = mask.Load( src.GetBrightmap() );
        }
        if( have )
        {
            std::vector< uint8_t > rgb( size_t( w ) * h * 3 );
            int                    lit = 0;
            double                 sum[ 3 ] = {};
            auto toLinear = []( uint8_t c ) {
                const double v = c / 255.0;
                return v * v;
            };
            for( int y = 0; y < h; y++ )
            {
                for( int x = 0; x < w; x++ )
                {
                    const uint8_t* a = &albedo.mBuffer[ size_t( y * w + x ) * 4 ]; // BGRA
                    const int      m = a[ 3 ] < 128 ? 0 : mask.At( x, y, w, h );
                    uint8_t*       o = &rgb[ size_t( y * w + x ) * 3 ];
                    o[ 0 ] = uint8_t( a[ 2 ] * m / 255 );
                    o[ 1 ] = uint8_t( a[ 1 ] * m / 255 );
                    o[ 2 ] = uint8_t( a[ 0 ] * m / 255 );
                    lit += ( o[ 0 ] | o[ 1 ] | o[ 2 ] ) > 8;
                    sum[ 0 ] += toLinear( o[ 0 ] );
                    sum[ 1 ] += toLinear( o[ 1 ] );
                    sum[ 2 ] += toLinear( o[ 2 ] );
                }
            }
            if( lit > 0 && WritePNG( pathE, rgb, w, h ) )
            {
                hasE = true;
                for( int k = 0; k < 3; k++ )
                {
                    outAvg[ k ] = float( sum[ k ] / ( double( w ) * h ) );
                }
                if( FILE* f = fopen( pathAvg.string().c_str(), "wb" ) )
                {
                    fprintf( f, "%f %f %f\n", outAvg[ 0 ], outAvg[ 1 ], outAvg[ 2 ] );
                    fclose( f );
                }
            }
        }
        if( !hasE )
        {
            std::filesystem::create_directories( dir, ec );
            if( FILE* f = fopen( pathNoE.string().c_str(), "wb" ) )
            {
                fclose( f );
            }
        }
        else
        {
            std::filesystem::remove( pathNoE, ec );
        }
    }

    // roughness and metallic
    if( !hasORM && layers && ( layers->Roughness.get() || layers->Metallic.get() ) )
    {
        Mask rough, metal;
        const bool hr = rough.Load( layers->Roughness.get() );
        const bool hm = metal.Load( layers->Metallic.get() );
        const int  ow = hr ? rough.w : metal.w;
        const int  oh = hr ? rough.h : metal.h;
        if( ( hr || hm ) && ow > 0 && oh > 0 )
        {
            std::vector< uint8_t > rgb( size_t( ow ) * oh * 3 );
            for( int y = 0; y < oh; y++ )
            {
                for( int x = 0; x < ow; x++ )
                {
                    uint8_t* o = &rgb[ size_t( y * ow + x ) * 3 ];
                    o[ 0 ] = 255;
                    o[ 1 ] = uint8_t( hr ? rough.At( x, y, ow, oh ) : 255 );
                    o[ 2 ] = uint8_t( hm ? metal.At( x, y, ow, oh ) : 0 );
                }
            }
            WritePNG( pathORM, rgb, ow, oh );
        }
    }

    return hasE;
}
