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
//  - roughness / metallic: packed into _orm (R occlusion, G roughness, B metallic).
//    R is 1 (open) but in the material table's maps, where it is the cavities
//    (rt_cavity): Raze RT's shaders darken a surface's ambient glow by it.
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
CVARD( Float, rt_cavity, 0.6f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: how much a material table texture's cracks and joints darken its ambient glow (0..1; new maps at the next start)" )

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
    float       glow  = 0.f; // optional 6th column: the texture glows (fire), this bright
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
            float r, m, bu, po, gl = 0.f;
            if( sscanf( line, " class %127s %f %f %f %f %f", a, &r, &m, &bu, &po, &gl ) >= 5 )
            {
                MatClass mc{ a, r, m, bu, po, gl, 0 };
                char     key[ 256 ];
                snprintf( key, sizeof( key ), "%s %.3f %.3f %.3f %.3f v2", a, r, m, bu, po );
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
    // two scales: the fine detail, and the broad shapes (a stone's bulge between its
    // dark joints) -- a texel-sized blur alone gave only thin lines at the edges
    auto blur = [ & ]( const std::vector< float >& in, int r ) {
        std::vector< float > tmp( in.size() ), out( in.size() );
        for( int y = 0; y < h; y++ )
        {
            for( int x = 0; x < w; x++ )
            {
                float sum = 0;
                for( int d = -r; d <= r; d++ )
                {
                    sum += in[ size_t( y * w + ( x + d + w * 4 ) % w ) ];
                }
                tmp[ size_t( y * w + x ) ] = sum / float( 2 * r + 1 );
            }
        }
        for( int y = 0; y < h; y++ )
        {
            for( int x = 0; x < w; x++ )
            {
                float sum = 0;
                for( int d = -r; d <= r; d++ )
                {
                    sum += tmp[ size_t( ( ( y + d + h * 4 ) % h ) * w + x ) ];
                }
                out[ size_t( y * w + x ) ] = sum / float( 2 * r + 1 );
            }
        }
        return out;
    };
    const int  big   = std::clamp( std::min( w, h ) / 24, 1, 6 );
    const auto fine  = blur( l, 1 );
    const auto broad = blur( blur( l, big ), big );
    for( size_t i = 0; i < o.size(); i++ )
    {
        o[ i ] = 0.45f * fine[ i ] + 0.55f * broad[ i ] * 1.6f;
    }
    return o;
}

} // namespace

const std::string& RT_GetOverrideFolder();

float rtx::RT_TableGlow( const std::string& name )
{
    const MatClass* g = TableClass( name );
    return g ? g->glow : 0.f;
}

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

    // a glowing class (fire): the texture itself is the glow, as it is
    if( const MatClass* g = TableClass( name ); g && g->glow > 0.f )
    {
        char sfx[ 32 ];
        snprintf( sfx, sizeof( sfx ), "_e.t%08x", g->hash ^ uint32_t( g->glow * 1000.f ) ^ 0x51u );
        const auto pathGT = dir / ( name + sfx );
        if( regen || !Exists( pathGT ) || !Exists( pathE ) )
        {
            std::vector< uint8_t > rgb( size_t( w ) * h * 3 );
            double                 sum[ 3 ] = {};
            int                    peak     = 1;
            for( int i = 0; i < w * h; i++ )
            {
                const uint8_t* a = &albedo.mBuffer[ size_t( i ) * 4 ]; // BGRA
                peak             = std::max( { peak, int( a[ 0 ] ), int( a[ 1 ] ), int( a[ 2 ] ) } );
            }
            const float gain = std::min( 4.f, 245.f / float( peak ) );
            for( int i = 0; i < w * h; i++ )
            {
                const uint8_t* a = &albedo.mBuffer[ size_t( i ) * 4 ];
                for( int k = 0; k < 3; k++ )
                {
                    const uint8_t c                   = a[ 3 ] < 128 ? 0 : a[ 2 - k ];
                    rgb[ size_t( i ) * 3 + size_t( k ) ] = uint8_t( std::clamp( c * gain + 0.5f, 0.f, 255.f ) );
                    const double v                    = rgb[ size_t( i ) * 3 + size_t( k ) ] / 255.0;
                    sum[ k ] += v * v;
                }
            }
            if( WritePNG( pathE, rgb, w, h ) )
            {
                Touch( pathGT );
                std::filesystem::remove( pathNoE, ec );
                for( int k = 0; k < 3; k++ )
                {
                    outAvg[ k ] = float( sum[ k ] / ( double( w ) * h ) ) * g->glow;
                }
                if( FILE* f = fopen( pathAvg.string().c_str(), "wb" ) )
                {
                    fprintf( f, "%f %f %f\n", outAvg[ 0 ], outAvg[ 1 ], outAvg[ 2 ] );
                    fclose( f );
                }
            }
        }
        if( Exists( pathE ) )
        {
            if( FILE* f = fopen( pathAvg.string().c_str(), "rb" ) )
            {
                hasE = fscanf( f, "%f %f %f", &outAvg[ 0 ], &outAvg[ 1 ], &outAvg[ 2 ] ) == 3;
                fclose( f );
            }
            noE = !hasE;
        }
        out.glow = g->glow;
    }

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

    // glass (whatever maps it has): see-through, it refracts (rt_draw.cpp)
    if( const MatClass* g = TableClass( name ) )
    {
        out.glass = g->name == "glass";
    }

    // the game's material table: textures without the overlay's maps
    const MatClass* mc = ( !layers || ( !layers->Roughness.get() && !layers->Metallic.get() ) ) ? TableClass( name ) : nullptr;
    if( mc )
    {
        char sfx[ 32 ];
        // (the cavity strength is part of the name: another strength makes them again)
        const float    cav     = std::clamp( float( *rt_cavity ), 0.f, 1.f );
        const uint32_t ormHash = mc->hash ^ ( uint32_t( cav * 100.f + 0.5f ) * 0x9e3779b1u ) ^ 0x3u;
        snprintf( sfx, sizeof( sfx ), "_orm.t%08x", ormHash );
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
                // the cavities: darker than the texture's average is deeper (a joint, a
                // crack), the more so the bumpier its class
                const float depthRel = std::clamp( 1.f - rel, 0.f, 1.f );
                const float occ      = 1.f - cav * std::clamp( mc->bump / 1.2f, 0.f, 1.f ) * depthRel * ( 2.f - depthRel );
                rgb[ size_t( i ) * 3 + 0 ] = uint8_t( std::clamp( occ * 255.f + 0.5f, 0.f, 255.f ) );
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
                                // (a bigger texture has more texels to the same stone: smaller steps per texel)
                const float k = 14.f * mc->bump * std::sqrt( std::clamp( float( std::min( w, h ) ) / 64.f, 0.5f, 4.f ) );
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

        // a class without bumps (glass) has no bump or height map -- not one made when the
        // texture was in another class
        if( !( mc->bump > 0.f ) && mc->name != "glass" )
        {
            std::filesystem::remove( dir / ( name + "_h.png" ), ec );
            if( !( layers && layers->Normal.get() ) )
            {
                std::filesystem::remove( pathN, ec );
            }
        }

        // the height map, for parallax occlusion mapping (RTGL1: _h, white = raised): the
        // same heights, stretched to the full range; a flatter class keeps to the top
        if( mc->bump > 0.f )
        {
            const auto pathH  = dir / ( name + "_h.png" );
            snprintf( sfx, sizeof( sfx ), "_h.t%08x", mc->hash ^ 0x1u );
            const auto pathHT = dir / ( name + sfx );
            if( regen || !Exists( pathHT ) || !Exists( pathH ) )
            {
                const auto hgt = Heights( albedo );
                float      lo = 1e9f, hi = -1e9f;
                for( float v : hgt )
                {
                    lo = std::min( lo, v ), hi = std::max( hi, v );
                }
                const float            span  = std::max( hi - lo, 1e-3f );
                const float            depth = std::clamp( mc->bump / 1.2f, 0.f, 1.f );
                std::vector< uint8_t > rgb( size_t( w ) * h * 3 );
                for( int i = 0; i < w * h; i++ )
                {
                    const float   v = 1.f - depth * ( 1.f - ( hgt[ size_t( i ) ] - lo ) / span );
                    const uint8_t b = uint8_t( std::clamp( v * 255.f + 0.5f, 0.f, 255.f ) );
                    rgb[ size_t( i ) * 3 + 0 ] = rgb[ size_t( i ) * 3 + 1 ] = rgb[ size_t( i ) * 3 + 2 ] = b;
                }
                if( WritePNG( pathH, rgb, w, h ) )
                {
                    Touch( pathHT );
                }
            }
        }
    }

    // glass: a wavy, hand-made pane (a slow ripple across it) with the texture's darker
    // lines (leading) set in a little -- Raze RT's shaders bend the view through it by
    // these bumps
    if( out.glass && *rt_materials_normals && !( layers && layers->Normal.get() ) )
    {
        char sfx[ 32 ];
        snprintf( sfx, sizeof( sfx ), "_n.g%08x", 0x6c617331u );
        const auto pathNG = dir / ( name + sfx );
        if( regen || !Exists( pathNG ) || !Exists( pathN ) )
        {
            const auto hgt = Heights( albedo );
            double     avg = 0;
            for( float v : hgt )
            {
                avg += v;
            }
            avg /= std::max< size_t >( 1, hgt.size() );
            // a few waves that tile with the texture, their phases fixed by its name
            uint32_t seed = 2166136261u;
            for( char c : name )
            {
                seed = ( seed ^ uint8_t( c ) ) * 16777619u;
            }
            auto rnd = [ & ]() {
                seed = seed * 1664525u + 1013904223u;
                return float( seed >> 8 ) / float( 1u << 24 );
            };
            struct Wave
            {
                float fx, fy, ph, amp;
            } waves[ 4 ];
            for( auto& wv : waves )
            {
                wv = { float( 1 + int( rnd() * 3 ) ) * ( rnd() < 0.5f ? -1.f : 1.f ), float( 1 + int( rnd() * 3 ) ),
                       rnd() * 6.2831853f, 0.5f + 0.5f * rnd() };
            }
            std::vector< float > H( size_t( w ) * h );
            for( int y = 0; y < h; y++ )
            {
                for( int x = 0; x < w; x++ )
                {
                    const float u = float( x ) / w, v = float( y ) / h;
                    float       r = 0;
                    for( auto& wv : waves )
                    {
                        r += wv.amp * std::sin( 6.2831853f * ( wv.fx * u + wv.fy * v ) + wv.ph );
                    }
                    const float rel = std::clamp( float( hgt[ size_t( y * w + x ) ] / std::max( avg, 0.02 ) ), 0.f, 2.f );
                    // ripple in texels of height, plus the lines a little lower
                    H[ size_t( y * w + x ) ] = r * float( std::max( w, h ) ) * 0.012f + rel * 1.5f;
                }
            }
            std::vector< uint8_t > rgb( size_t( w ) * h * 3 );
            for( int y = 0; y < h; y++ )
            {
                for( int x = 0; x < w; x++ )
                {
                    auto At = [ & ]( int xx, int yy ) { return H[ size_t( ( ( yy + h ) % h ) * w + ( xx + w ) % w ) ]; };
                    const float dx = ( At( x + 1, y ) - At( x - 1, y ) ) * 0.5f;
                    const float dy = ( At( x, y + 1 ) - At( x, y - 1 ) ) * 0.5f;
                    float       nx = -dx * 0.12f, ny = dy * 0.12f, nz = 1.f;
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
                Touch( pathNG );
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
