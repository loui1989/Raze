// Raze RT: material maps for RTGL1.
//
// RTGL1 finds a texture's extra maps by name: <rt>/mat_dev/<name>_e.png (emission),
// <name>_orm.png (occlusion, roughness, metallic) and so on, read when the texture is
// given to it. Raze attaches Duke-RT's overlay maps (materials/glowmaps/auto/#01234.png
// and friends, mounted with -file) to the tile textures as material layers; here they
// become those files:
//
//  - emission: where the glowmap (or, without one, the palette's fullbright
//    brightmap) says the texture glows. A coloured glowmap (Duke-RT paints most of
//    them in the glow's own colour) is used as it is; a grey one is a mask over the
//    translated pixels, so a palette swapped sign glows in its own colour. Either
//    way the brightest texel is brought to full brightness: the glowmaps are drawn
//    soft (a neon sign peaks at a quarter), and RTGL1 shows _e as it is.
//  - roughness / metallic: packed into _orm (R unused, G roughness, B metallic).
//    RTGL1's "polished" surfaces mirror the scene (traced mirror rays; low roughness
//    alone gets denoised away), masked per texel by the _orm roughness -- and much
//    more strongly than a real dielectric (15% head on, where wood or concrete gives
//    4%). Duke-RT's roughness maps are smooth almost everywhere (mean 0.3), so a
//    texture is polished only where it has smooth METAL (the stars on the Hollywood
//    walk) or near-glass texels (window panes); in such a texture the other texels
//    are written just rough enough not to mirror.
//  - normal: World Tour's normal maps (setup.sh imports them as materials/normalmaps),
//    green flipped: they are made Y-down, as Duke-RT reads them, RTGL1 reads Y-up.
//
// A file is made once and kept; rt_materials_regen 1 makes them again.

#include "rt_backend.h"

#include "m_png.h"
#include "files.h"

#include <filesystem>
#include <unordered_set>
#include <unordered_map>

CVARD( Bool, rt_materials, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: use the material maps (glow, roughness, metallic) of the mounted overlay" )
CVARD( Bool, rt_materials_brightmaps, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: textures without a glowmap glow where the palette is fullbright" )
CVARD( Bool, rt_materials_regen, false, 0, "RT: make the material files again (this session)" )
CVARD( Bool, rt_materials_normals, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: use the normal maps (World Tour's, imported by setup.sh)" )

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

    // the BGRA texel at the texture coordinate of pixel (x, y) of a w2 x h2 picture
    const uint8_t* Texel( int x, int y, int w2, int h2 ) const
    {
        const int sx = std::min( w - 1, x * w / std::max( 1, w2 ) );
        const int sy = std::min( h - 1, y * h / std::max( 1, h2 ) );
        return &buf[ unsigned( ( sy * w + sx ) * 4 ) ];
    }

    // luminance 0..255 at the texture coordinate of pixel (x, y) of a w2 x h2 picture
    int At( int x, int y, int w2, int h2 ) const
    {
        const uint8_t* p = Texel( x, y, w2, h2 );
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

// A game's material table, <rt>/materials.txt (Blood: its textures sorted by what
// they are made of). For textures without the overlay's maps, each listed tile gets
// roughness / metallic / mirror polish of its class and a bump map from its own
// light and dark texels:
//   class <name> <roughness 0..1> <metallic 0..1> <bump strength> <polish 0..1.5>
//   <tile>[-<tile>] <class name>
struct MatClass
{
    std::string name;
    float       rough = 0.85f, metal = 0.f, bump = 0.f, polish = 0.f;
    uint32_t    hash  = 0;
};
struct MatTable
{
    std::vector< MatClass >             classes;
    std::unordered_map< int, unsigned > tiles; // tile -> class index
    bool                                loaded = false;
};
MatTable g_table;

const MatClass* TableClass( const std::string& texname )
{
    if( !g_table.loaded )
    {
        g_table.loaded = true;
        const auto path = std::filesystem::path( RT_GetOverrideFolder() ) / "materials.txt";
        FILE*      f    = fopen( path.string().c_str(), "rb" );
        if( !f )
        {
            return nullptr;
        }
        std::unordered_map< std::string, unsigned > byName;
        char line[ 512 ];
        int  nt = 0;
        while( fgets( line, sizeof( line ), f ) )
        {
            if( char* c = strchr( line, '#' ) )
            {
                *c = 0;
            }
            char  a[ 128 ], b[ 128 ];
            float r, m, bu, po;
            if( sscanf( line, " class %127s %f %f %f %f", a, &r, &m, &bu, &po ) == 5 )
            {
                MatClass mc{ a, r, m, bu, po, 0 };
                char     key[ 256 ];
                snprintf( key, sizeof( key ), "%s %.3f %.3f %.3f %.3f v1", a, r, m, bu, po );
                uint32_t hsh = 2166136261u;
                for( const char* k = key; *k; k++ )
                {
                    hsh = ( hsh ^ uint8_t( *k ) ) * 16777619u;
                }
                mc.hash      = hsh;
                byName[ a ] = unsigned( g_table.classes.size() );
                g_table.classes.push_back( mc );
                continue;
            }
            int t0 = -1, t1 = -1;
            if( sscanf( line, " %d-%d %127s", &t0, &t1, b ) == 3 || ( t1 = -1, sscanf( line, " %d %127s", &t0, b ) == 2 ) )
            {
                if( t1 < t0 )
                {
                    t1 = t0;
                }
                auto it = byName.find( b );
                if( it == byName.end() )
                {
                    continue;
                }
                for( int t = t0; t <= t1 && t - t0 < 10000; t++ )
                {
                    g_table.tiles[ t ] = it->second;
                    nt++;
                }
            }
        }
        fclose( f );
        Printf( "RT: material table: %d classes, %d textures\n", int( g_table.classes.size() ), nt );
    }
    if( texname.size() < 2 || texname[ 0 ] != '#' )
    {
        return nullptr;
    }
    const int  tile = atoi( texname.c_str() + 1 );
    const auto it   = g_table.tiles.find( tile );
    return it == g_table.tiles.end() ? nullptr : &g_table.classes[ it->second ];
}

bool Exists( const std::filesystem::path& p )
{
    std::error_code ec;
    return std::filesystem::exists( p, ec );
}

void Touch( const std::filesystem::path& p, const char* text = nullptr )
{
    if( FILE* f = fopen( p.string().c_str(), "wb" ) )
    {
        if( text )
        {
            fputs( text, f );
        }
        fclose( f );
    }
}

// the albedo's luminance 0..1, softened a little (the art is dithered), wrapping
std::vector< float > Heights( const FTextureBuffer& albedo )
{
    const int            w = albedo.mWidth, h = albedo.mHeight;
    std::vector< float > l( size_t( w ) * h ), o( size_t( w ) * h );
    for( int i = 0; i < w * h; i++ )
    {
        const uint8_t* a = &albedo.mBuffer[ size_t( i ) * 4 ]; // BGRA
        l[ size_t( i ) ] = ( 0.2126f * a[ 2 ] + 0.7152f * a[ 1 ] + 0.0722f * a[ 0 ] ) / 255.f;
    }
    for( int y = 0; y < h; y++ )
    {
        for( int x = 0; x < w; x++ )
        {
            float sum = 0;
            for( int dy = -1; dy <= 1; dy++ )
            {
                for( int dx = -1; dx <= 1; dx++ )
                {
                    const int   xx = ( x + dx + w ) % w, yy = ( y + dy + h ) % h;
                    const float k  = ( dx == 0 ? 2.f : 1.f ) * ( dy == 0 ? 2.f : 1.f );
                    sum += l[ size_t( yy * w + xx ) ] * k;
                }
            }
            o[ size_t( y * w + x ) ] = sum / 16.f;
        }
    }
    return o;
}

} // namespace

const std::string& RT_GetOverrideFolder();

bool rtx::RT_PrepareMaterialOverrides( FGameTexture& src, const std::string& name, const FTextureBuffer& albedo, RtMaterialInfo& out )
{
    out = {};
    float* outAvg = out.emisAvg;
    if( !*rt_materials || name.empty() || !albedo.mBuffer )
    {
        return false;
    }

    // the sidecar files carry a version: a new version makes the maps again
    const std::filesystem::path dir = std::filesystem::path( RT_GetOverrideFolder() ) / "mat_dev";
    const auto pathE   = dir / ( name + "_e.png" );
    const auto pathORM = dir / ( name + "_orm.png" );
    const auto pathNoE = dir / ( name + "_e.v3none" ); // looked, nothing glows
    const auto pathAvg = dir / ( name + "_e.v3avg" );  // the emission map's average colour (linear)
    const auto pathPol = dir / ( name + "_orm.v5" );   // 1 if polished (see above)
    const auto pathN   = dir / ( name + "_n.png" );
    const auto pathNv  = dir / ( name + "_n.v1" );     // made by this version

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
    bool hasORM = false;
    if( !regen && std::filesystem::exists( pathORM, ec ) )
    {
        if( FILE* f = fopen( pathPol.string().c_str(), "rb" ) )
        {
            float smooth = 0;
            hasORM       = fscanf( f, "%f", &smooth ) == 1;
            out.polished = smooth > 0.5f;
            fclose( f );
        }
    }

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
            // is the glowmap the glow's colour, or a grey mask?
            int maxM = 0;
            double satSum = 0, satW = 0;
            for( int y = 0; y < h; y++ )
            {
                for( int x = 0; x < w; x++ )
                {
                    const uint8_t* g = mask.Texel( x, y, w, h );
                    const int      m = mask.At( x, y, w, h );
                    maxM             = std::max( maxM, m );
                    const int hi = std::max( { g[ 0 ], g[ 1 ], g[ 2 ] } );
                    const int lo = std::min( { g[ 0 ], g[ 1 ], g[ 2 ] } );
                    if( g[ 3 ] >= 128 && hi > 24 )
                    {
                        satSum += double( hi - lo ) / hi * hi;
                        satW += hi;
                    }
                }
            }
            const bool coloured = satW > 0 && satSum / satW > 0.2;
            // the soft speckle around a glow is not glow
            const int floorM = std::max( 6, maxM / 12 );

            std::vector< float > e( size_t( w ) * h * 3, 0.f );
            float                peak = 0;
            for( int y = 0; y < h; y++ )
            {
                for( int x = 0; x < w; x++ )
                {
                    const uint8_t* a = &albedo.mBuffer[ size_t( y * w + x ) * 4 ]; // BGRA
                    const int      m = a[ 3 ] < 128 ? 0 : mask.At( x, y, w, h );
                    if( m < floorM )
                    {
                        continue;
                    }
                    float* o = &e[ size_t( y * w + x ) * 3 ];
                    if( coloured )
                    {
                        const uint8_t* g = mask.Texel( x, y, w, h );
                        o[ 0 ]           = g[ 2 ];
                        o[ 1 ]           = g[ 1 ];
                        o[ 2 ]           = g[ 0 ];
                    }
                    else
                    {
                        o[ 0 ] = a[ 2 ] * m / 255.f;
                        o[ 1 ] = a[ 1 ] * m / 255.f;
                        o[ 2 ] = a[ 0 ] * m / 255.f;
                    }
                    peak = std::max( { peak, o[ 0 ], o[ 1 ], o[ 2 ] } );
                }
            }
            const float gain = peak > 1.f ? std::min( 4.f, 245.f / peak ) : 1.f;

            std::vector< uint8_t > rgb( size_t( w ) * h * 3 );
            int                    lit = 0;
            double                 sum[ 3 ] = {};
            auto toLinear = []( uint8_t c ) {
                const double v = c / 255.0;
                return v * v;
            };
            for( size_t i = 0; i < rgb.size(); i++ )
            {
                rgb[ i ] = uint8_t( std::clamp( e[ i ] * gain + 0.5f, 0.f, 255.f ) );
                sum[ i % 3 ] += toLinear( rgb[ i ] );
            }
            for( size_t i = 0; i < rgb.size(); i += 3 )
            {
                lit += ( rgb[ i ] | rgb[ i + 1 ] | rgb[ i + 2 ] ) > 8;
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
            std::filesystem::remove( pathE, ec ); // an older version's
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

    // the game's material table: textures without the overlay's maps
    const MatClass* mc = ( !layers || ( !layers->Roughness.get() && !layers->Metallic.get() ) ) ? TableClass( name ) : nullptr;
    if( mc )
    {
        char sfx[ 32 ];
        snprintf( sfx, sizeof( sfx ), "_orm.t%08x", mc->hash );
        const auto pathT = dir / ( name + sfx );
        if( regen || !Exists( pathT ) || !Exists( pathORM ) )
        {
            const auto           hgt = Heights( albedo );
            double               avg = 0;
            for( float v : hgt )
            {
                avg += v;
            }
            avg /= std::max< size_t >( 1, hgt.size() );
            std::vector< uint8_t > rgb( size_t( w ) * h * 3 );
            for( int i = 0; i < w * h; i++ )
            {
                const uint8_t* a     = &albedo.mBuffer[ size_t( i ) * 4 ];
                const float    rel   = std::clamp( float( hgt[ size_t( i ) ] / std::max( avg, 0.02 ) ), 0.f, 2.f );
                float          r;
                if( mc->polish > 0.f && a[ 3 ] >= 128 )
                {
                    // the mirror: stronger on the lit, raised parts than in the cracks
                    const float pol = std::clamp( mc->polish * ( 0.5f + 0.5f * rel ), 0.f, 1.5f );
                    r               = 0.7f - pol / 4.5f;
                }
                else
                {
                    // rough enough never to mirror; the cracks and mortar rougher
                    r = std::max( mc->rough + ( 1.f - rel ) * 0.1f, 0.71f );
                }
                rgb[ size_t( i ) * 3 + 0 ] = 255;
                rgb[ size_t( i ) * 3 + 1 ] = uint8_t( std::clamp( r * 255.f + 0.5f, 0.f, 255.f ) );
                rgb[ size_t( i ) * 3 + 2 ] = uint8_t( std::clamp( mc->metal * ( 0.75f + 0.25f * rel ) * 255.f + 0.5f, 0.f, 255.f ) );
            }
            if( WritePNG( pathORM, rgb, w, h ) )
            {
                Touch( pathPol, mc->polish > 0.f ? "1\n" : "0\n" );
                Touch( pathT );
            }
        }
        if( FILE* f = fopen( pathPol.string().c_str(), "rb" ) )
        {
            float smooth = 0;
            hasORM       = fscanf( f, "%f", &smooth ) == 1;
            out.polished = smooth > 0.5f;
            fclose( f );
        }

        // the bump map, from the texture's own light and dark texels (RTGL1: Y up)
        if( mc->bump > 0.f && !( layers && layers->Normal.get() ) && *rt_materials_normals )
        {
            snprintf( sfx, sizeof( sfx ), "_n.t%08x", mc->hash );
            const auto pathNT = dir / ( name + sfx );
            if( regen || !Exists( pathNT ) || !Exists( pathN ) )
            {
                const auto             hgt = Heights( albedo );
                std::vector< uint8_t > rgb( size_t( w ) * h * 3 );
                // texels are big: the slope per texel, scaled so a 64 texel tile gets
                // about the same relief as a 256 texel one
                const float k = 6.f * mc->bump * std::clamp( 64.f / float( std::max( 16, std::min( w, h ) ) ), 0.35f, 2.f );
                for( int y = 0; y < h; y++ )
                {
                    for( int x = 0; x < w; x++ )
                    {
                        auto H = [ & ]( int xx, int yy ) {
                            return hgt[ size_t( ( ( yy + h ) % h ) * w + ( xx + w ) % w ) ];
                        };
                        const float dx = ( H( x + 1, y ) - H( x - 1, y ) ) * 0.5f;
                        const float dy = ( H( x, y + 1 ) - H( x, y - 1 ) ) * 0.5f;
                        float       nx = -dx * k, ny = dy * k, nz = 1.f;
                        const float il = 1.f / std::sqrt( nx * nx + ny * ny + nz * nz );
                        nx *= il, ny *= il, nz *= il;
                        uint8_t* o = &rgb[ size_t( y * w + x ) * 3 ];
                        o[ 0 ]     = uint8_t( std::clamp( ( nx * 0.5f + 0.5f ) * 255.f + 0.5f, 0.f, 255.f ) );
                        o[ 1 ]     = uint8_t( std::clamp( ( ny * 0.5f + 0.5f ) * 255.f + 0.5f, 0.f, 255.f ) );
                        o[ 2 ]     = uint8_t( std::clamp( ( nz * 0.5f + 0.5f ) * 255.f + 0.5f, 0.f, 255.f ) );
                    }
                }
                if( WritePNG( pathN, rgb, w, h ) )
                {
                    Touch( pathNT );
                }
            }
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
            size_t                 metalSmooth = 0, glassy = 0;
            for( int y = 0; y < oh; y++ )
            {
                for( int x = 0; x < ow; x++ )
                {
                    uint8_t* o = &rgb[ size_t( y * ow + x ) * 3 ];
                    o[ 0 ] = 255;
                    o[ 1 ] = uint8_t( hr ? rough.At( x, y, ow, oh ) : 255 );
                    o[ 2 ] = uint8_t( hm ? metal.At( x, y, ow, oh ) : 0 );
                    metalSmooth += o[ 2 ] >= 128 && o[ 1 ] < 140;
                    glassy += o[ 2 ] < 128 && o[ 1 ] < 38;
                }
            }
            const double n        = double( ow ) * oh;
            const bool   polished = metalSmooth >= 0.02 * n || glassy >= 0.05 * n;
            if( polished )
            {
                // RTGL1: polish = (0.7 - roughness) * 4.5, up to 3 -- and at 3 a texel
                // is 45% mirror head on and 95% at grazing angles, its own texture all
                // but gone. Here: smooth metal up to polish 1 (15% / 60%), by how
                // smooth and how metallic it is; near-glass (the window panes, painted
                // roughness 0) polish 1; everything else none
                for( size_t i = 0; i < rgb.size(); i += 3 )
                {
                    const float r = rgb[ i + 1 ] / 255.f;
                    const float m = rgb[ i + 2 ] / 255.f;
                    float       polish;
                    if( m >= 0.5f )
                    {
                        polish = std::clamp( ( 0.55f - r ) / 0.55f, 0.f, 1.f ) * m;
                    }
                    else
                    {
                        polish = r < 0.15f ? 1.f : 0.f;
                    }
                    const float rOut = polish > 0.f ? 0.7f - polish / 4.5f : std::max( r, 0.71f );
                    rgb[ i + 1 ]     = uint8_t( std::clamp( rOut * 255.f + 0.5f, 0.f, 255.f ) );
                }
            }
            if( WritePNG( pathORM, rgb, ow, oh ) )
            {
                out.polished = polished;
                if( FILE* f = fopen( pathPol.string().c_str(), "wb" ) )
                {
                    fprintf( f, "%d\n", polished ? 1 : 0 );
                    fclose( f );
                }
            }
        }
    }

    // normal map
    if( layers && layers->Normal.get() && *rt_materials_normals &&
        ( regen || !std::filesystem::exists( pathNv, ec ) ) )
    {
        Mask nm;
        if( nm.Load( layers->Normal.get() ) )
        {
            std::vector< uint8_t > rgb( size_t( nm.w ) * nm.h * 3 );
            for( int i = 0; i < nm.w * nm.h; i++ )
            {
                const uint8_t* p = &nm.buf[ unsigned( i * 4 ) ]; // BGRA
                rgb[ size_t( i ) * 3 + 0 ] = p[ 2 ];
                rgb[ size_t( i ) * 3 + 1 ] = uint8_t( 255 - p[ 1 ] );
                rgb[ size_t( i ) * 3 + 2 ] = p[ 0 ];
            }
            if( WritePNG( pathN, rgb, nm.w, nm.h ) )
            {
                if( FILE* f = fopen( pathNv.string().c_str(), "wb" ) )
                {
                    fclose( f );
                }
            }
        }
    }

    out.hasEmission = hasE;
    return hasE;
}
