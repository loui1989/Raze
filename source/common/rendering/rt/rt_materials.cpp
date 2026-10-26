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
#include "colormatcher.h"
#include "texinfo.h"

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
    bool        lamp  = false; // a class named lamp...: its glow follows its room's light (switched off: dark)
    uint32_t    hash  = 0;
    bool        flame = false; // a class named flame...: only its flame coloured texels glow (a candle's flame, not its wax)
};
struct MatTable
{
    std::vector< MatClass >             classes;
    std::unordered_map< int, unsigned > tiles; // tile -> class index
    bool                                loaded = false;
    float                               autoBump = 0.f; // "autobump N": bumps for textures with none
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
            if( float ab; sscanf( line, " autobump %f", &ab ) == 1 )
            {
                g_table.autoBump = std::max( 0.f, ab );
                continue;
            }
            if( sscanf( line, " class %127s %f %f %f %f %f", a, &r, &m, &bu, &po, &gl ) >= 5 )
            {
                MatClass mc{ a, r, m, bu, po, gl, strncmp( a, "lamp", 4 ) == 0, 0, strncmp( a, "flame", 5 ) == 0 };
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
    const int tile = atoi( texname.c_str() + 1 );
    auto      it   = g_table.tiles.find( tile );
    if( it == g_table.tiles.end() )
    {
        // a frame of an animation: its first frame's class (the table lists the first
        // frames; one frame of the water with a stone's bumps made it flip as it ran)
        for( int k = 1; k <= 63 && tile - k >= 0; k++ )
        {
            const auto& pa = GetExtInfo( tileGetTextureID( tile - k ) ).picanm;
            if( pa.num >= k && ( pa.type() == PICANM_ANIMTYPE_FWD || pa.type() == PICANM_ANIMTYPE_OSC ) )
            {
                it = g_table.tiles.find( tile - k );
                break;
            }
        }
    }
    return it == g_table.tiles.end() ? nullptr : &g_table.classes[ it->second ];
}

// a flame's colours (BGRA): the fire ramps of the palette, yellow to deep orange, no blue
// in them (the wax, brass and wood around a flame have some)
bool FlameTexel( const uint8_t* a )
{
    const int b = a[ 0 ], g = a[ 1 ], r = a[ 2 ];
    return a[ 3 ] >= 128 && b <= 12 && r >= 140 && g * 4 >= r && g <= r;
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

// The heights a normal map (as given: BGRA, Y down -- World Tour's) is the slope of:
// its gradient field integrated (a Poisson solve, wrapping: the textures tile), on a
// grid of at most 256 texels a side; 0..1, white = raised. For parallax mapping.
std::vector< float > HeightsFromNormals( const Mask& nm, int& W, int& H )
{
    const int big = std::max( nm.w, nm.h );
    const float s = std::max( 1.f, float( big ) / 256.f );
    W             = std::max( 4, int( nm.w / s ) );
    H             = std::max( 4, int( nm.h / s ) );
    std::vector< float > p( size_t( W ) * H ), q( size_t( W ) * H ), div( size_t( W ) * H ), h( size_t( W ) * H, 0.f );
    for( int y = 0; y < H; y++ )
    {
        for( int x = 0; x < W; x++ )
        {
            const uint8_t* t  = nm.Texel( x, y, W, H ); // BGRA
            const float    nx = t[ 2 ] / 127.5f - 1.f;
            const float    ny = t[ 1 ] / 127.5f - 1.f; // Y down: + is towards +y (image)
            const float    nz = std::max( 0.2f, t[ 0 ] / 127.5f - 1.f );
            // slope of the height along x and (image) y, per grid cell
            p[ size_t( y * W + x ) ] = -nx / nz * s;
            q[ size_t( y * W + x ) ] = -ny / nz * s;
        }
    }
    auto I = [ & ]( int x, int y ) { return size_t( ( ( y + H ) % H ) * W + ( x + W ) % W ); };
    for( int y = 0; y < H; y++ )
    {
        for( int x = 0; x < W; x++ )
        {
            div[ I( x, y ) ] = ( p[ I( x + 1, y ) ] - p[ I( x - 1, y ) ] ) * 0.5f + ( q[ I( x, y + 1 ) ] - q[ I( x, y - 1 ) ] ) * 0.5f;
        }
    }
    // Gauss-Seidel with over-relaxation: the large shapes come in slowly, but parallax
    // only needs the depth of a stone against its joints
    for( int it = 0; it < 300; it++ )
    {
        for( int y = 0; y < H; y++ )
        {
            for( int x = 0; x < W; x++ )
            {
                const float g = ( h[ I( x - 1, y ) ] + h[ I( x + 1, y ) ] + h[ I( x, y - 1 ) ] + h[ I( x, y + 1 ) ] - div[ I( x, y ) ] ) * 0.25f;
                h[ I( x, y ) ] += 1.85f * ( g - h[ I( x, y ) ] );
            }
        }
    }
    std::vector< float > sorted = h;
    std::sort( sorted.begin(), sorted.end() );
    const float lo = sorted[ sorted.size() * 2 / 100 ], hi = sorted[ sorted.size() * 98 / 100 ];
    const float span = std::max( hi - lo, 1e-4f );
    for( float& v : h )
    {
        v = std::clamp( ( v - lo ) / span, 0.f, 1.f );
    }
    return h;
}

} // namespace

const std::string& RT_GetOverrideFolder();

float rtx::RT_TableGlow( const std::string& name )
{
    const MatClass* g = TableClass( name );
    // (a flame class's texture glows only where its flame is: its emission map)
    return g && !g->flame ? g->glow : 0.f;
}

float RT_TableFlame( int tile )
{
    char nm[ 16 ];
    snprintf( nm, sizeof( nm ), "#%05d", tile );
    const MatClass* g = TableClass( nm );
    return g && g->flame ? g->glow : 0.f;
}

bool RT_FlameColor( uint8_t r, uint8_t g, uint8_t b )
{
    const uint8_t bgra[ 4 ] = { b, g, r, 255 };
    return FlameTexel( bgra );
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

    // a voxel model's palette texture: its fullbright colours glow, as on the sprites
    // (eyes, flames, embers) -- the game's brightmap does not cover these textures
    const uint8_t* vpal   = nullptr;
    FImageSource*  img    = src.GetTexture() ? src.GetTexture()->GetImage() : nullptr;
    const bool     voxPal = img && IsVoxelPaletteTexture( img, &vpal ) && w == 16 && h == 16;
    const auto     pathVx = dir / ( name + "_e.vox1" );
    if( voxPal )
    {
        out.voxel       = true;
        const int white = ColorMatcher.Pick( 255, 255, 255 );
        for( int i = 0; i < 256; i++ )
        {
            int idx = i;
            if( vpal )
            {
                auto c6 = []( uint8_t c ) { return int( ( c << 2 ) | ( c >> 4 ) ); };
                idx     = ColorMatcher.Pick( c6( vpal[ i * 3 ] ), c6( vpal[ i * 3 + 1 ] ), c6( vpal[ i * 3 + 2 ] ) );
            }
            out.voxGlow[ i ] = GPalette.HasGlobalBrightmap && GPalette.GlobalBrightmap.Remap[ idx ] == white;
        }
        if( !Exists( pathVx ) )
        {
            hasE = noE = false; // made before this version: look again
        }
    }

    // a glowing class (fire): the texture itself is the glow, as it is
    if( const MatClass* g = TableClass( name ); g && g->glow > 0.f )
    {
        char sfx[ 32 ];
        snprintf( sfx, sizeof( sfx ), "_e.t%08x", g->hash ^ uint32_t( g->glow * 1000.f ) ^ ( g->flame ? 0x52u : 0x51u ) );
        const auto pathGT = dir / ( name + sfx );
        if( regen || !Exists( pathGT ) || !Exists( pathE ) )
        {
            std::vector< uint8_t > rgb( size_t( w ) * h * 3 );
            double                 sum[ 3 ] = {};
            int                    peak     = 1;
            for( int i = 0; i < w * h; i++ )
            {
                const uint8_t* a = &albedo.mBuffer[ size_t( i ) * 4 ]; // BGRA
                if( g->flame && !FlameTexel( a ) )
                {
                    continue;
                }
                peak             = std::max( { peak, int( a[ 0 ] ), int( a[ 1 ] ), int( a[ 2 ] ) } );
            }
            const float gain = std::min( 4.f, 245.f / float( peak ) );
            for( int i = 0; i < w * h; i++ )
            {
                const uint8_t* a = &albedo.mBuffer[ size_t( i ) * 4 ];
                for( int k = 0; k < 3; k++ )
                {
                    const uint8_t c                   = a[ 3 ] < 128 || ( g->flame && !FlameTexel( a ) ) ? 0 : a[ 2 - k ];
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
        out.glow  = g->glow;
        out.lamp  = g->lamp;
        out.flame = g->flame;
    }

    // emission
    if( !hasE && !noE )
    {
        Mask mask;
        bool have = layers && layers->Glowmap.get() && mask.Load( layers->Glowmap.get() );
        if( !have && *rt_materials_brightmaps && voxPal )
        {
            mask.w = mask.h = 16;
            mask.buf.Resize( 256 * 4 );
            for( int i = 0; i < 256; i++ )
            {
                memset( &mask.buf[ unsigned( i * 4 ) ], out.voxGlow[ i ] ? 255 : 0, 4 );
            }
            have = true;
        }
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
        if( voxPal )
        {
            Touch( pathVx );
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
        const uint32_t ormHash = mc->hash ^ ( uint32_t( cav * 100.f + 0.5f ) * 0x9e3779b1u ) ^ 0x4u;
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
                    const float pol = std::clamp( mc->polish * ( 0.85f + 0.15f * rel ), 0.f, 3.f );
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

    // a see-through texture (a sprite, a grate), a glowing one (a sign) or one that is not
    // a tile (a voxel model's skin: the voxel enemies went black) has no height map or
    // made-up bumps
    size_t holes = 0;
    for( int i = 0; i < w * h; i++ )
    {
        holes += albedo.mBuffer[ size_t( i ) * 4 + 3 ] < 128;
    }
    // (and only the game's tiles, "#01234": not voxel models' skins, skies, pictures)
    const bool solid = holes <= size_t( w ) * h / 100 && w >= 16 && h >= 16 && !hasE && name.size() > 1 && name[ 0 ] == '#';

    // parallax for the textures with World Tour's normal maps: heights made from them
    if( layers && layers->Normal.get() && *rt_materials_normals && !solid )
    {
        std::filesystem::remove( dir / ( name + "_h.png" ), ec ); // (made by an earlier version)
        std::filesystem::remove( dir / ( name + "_h.n1" ), ec );
    }
    else if( layers && layers->Normal.get() && *rt_materials_normals )
    {
        const auto pathH  = dir / ( name + "_h.png" );
        const auto pathHv = dir / ( name + "_h.n1" );
        if( regen || !Exists( pathHv ) || !Exists( pathH ) )
        {
            Mask nm;
            if( nm.Load( layers->Normal.get() ) )
            {
                int        W = 0, H = 0;
                const auto hgt = HeightsFromNormals( nm, W, H );
                std::vector< uint8_t > rgb( size_t( W ) * H * 3 );
                for( size_t i = 0; i < hgt.size(); i++ )
                {
                    rgb[ i * 3 + 0 ] = rgb[ i * 3 + 1 ] = rgb[ i * 3 + 2 ] = uint8_t( hgt[ i ] * 255.f + 0.5f );
                }
                if( WritePNG( pathH, rgb, W, H ) )
                {
                    Touch( pathHv );
                }
            }
        }
    }
    // the rest (Duke: the textures World Tour made no normal map for): bumps and heights
    // from the texture's own light and dark texels, if the table says so (autobump) --
    // not for see-through ones (sprites, grates) nor glowing ones (signs, screens)
    else if( !mc && !out.glass && *rt_materials_normals && TableClass( "" ) == nullptr && g_table.autoBump > 0.f )
    {
        if( solid )
        {
            char sfx[ 32 ];
            snprintf( sfx, sizeof( sfx ), "_n.a%08x", uint32_t( g_table.autoBump * 1000.f ) ^ 0xa0u );
            const auto pathNA = dir / ( name + sfx );
            const auto pathH  = dir / ( name + "_h.png" );
            if( regen || !Exists( pathNA ) || !Exists( pathN ) || !Exists( pathH ) )
            {
                const auto hgt = Heights( albedo );
                const float k  = 14.f * g_table.autoBump * std::sqrt( std::clamp( float( std::min( w, h ) ) / 64.f, 0.5f, 4.f ) );
                std::vector< uint8_t > rgb( size_t( w ) * h * 3 ), hr( size_t( w ) * h * 3 );
                float lo = 1e9f, hi = -1e9f;
                for( float v : hgt )
                {
                    lo = std::min( lo, v ), hi = std::max( hi, v );
                }
                const float span = std::max( hi - lo, 1e-3f );
                for( int y = 0; y < h; y++ )
                {
                    for( int x = 0; x < w; x++ )
                    {
                        auto At = [ & ]( int xx, int yy ) { return hgt[ size_t( ( ( yy + h ) % h ) * w + ( xx + w ) % w ) ]; };
                        const float dx = ( At( x + 1, y ) - At( x - 1, y ) ) * 0.5f;
                        const float dy = ( At( x, y + 1 ) - At( x, y - 1 ) ) * 0.5f;
                        float       nx = -dx * k, ny = dy * k, nz = 1.f;
                        const float il = 1.f / std::sqrt( nx * nx + ny * ny + nz * nz );
                        nx *= il, ny *= il, nz *= il;
                        uint8_t* o = &rgb[ size_t( y * w + x ) * 3 ];
                        o[ 0 ]     = uint8_t( std::clamp( ( nx * 0.5f + 0.5f ) * 255.f + 0.5f, 0.f, 255.f ) );
                        o[ 1 ]     = uint8_t( std::clamp( ( ny * 0.5f + 0.5f ) * 255.f + 0.5f, 0.f, 255.f ) );
                        o[ 2 ]     = uint8_t( std::clamp( ( nz * 0.5f + 0.5f ) * 255.f + 0.5f, 0.f, 255.f ) );
                        const float v = 1.f - std::min( 1.f, g_table.autoBump ) * ( 1.f - ( At( x, y ) - lo ) / span );
                        uint8_t*    ho = &hr[ size_t( y * w + x ) * 3 ];
                        ho[ 0 ] = ho[ 1 ] = ho[ 2 ] = uint8_t( std::clamp( v * 255.f + 0.5f, 0.f, 255.f ) );
                    }
                }
                if( WritePNG( pathN, rgb, w, h ) && WritePNG( pathH, hr, w, h ) )
                {
                    Touch( pathNA );
                }
            }
        }
    }

    out.hasEmission = hasE;
    return hasE;
}
