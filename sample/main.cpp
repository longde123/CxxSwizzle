// CxxSwizzle
// Copyright (c) 2013-2015, Piotr Gwiazdowski <gwiazdorrr+github at gmail.com>

#if defined(USE_SIMD)
#include "use_simd.h"
#else
#include "use_scalar.h"
#endif

#include <swizzle/glsl/vector.h>
#include <swizzle/glsl/matrix.h>
#include <swizzle/glsl/texture_functions.h>

typedef swizzle::glsl::vector< float_type, 2 > vec2;
typedef swizzle::glsl::vector< float_type, 3 > vec3;
typedef swizzle::glsl::vector< float_type, 4 > vec4;

static_assert(sizeof(vec2) == sizeof(float_type[2]), "Too big");
static_assert(sizeof(vec3) == sizeof(float_type[3]), "Too big");
static_assert(sizeof(vec4) == sizeof(float_type[4]), "Too big");

typedef swizzle::glsl::matrix< swizzle::glsl::vector, vec4::scalar_type, 2, 2> mat2;
typedef swizzle::glsl::matrix< swizzle::glsl::vector, vec4::scalar_type, 3, 3> mat3;
typedef swizzle::glsl::matrix< swizzle::glsl::vector, vec4::scalar_type, 4, 4> mat4;


//! A really, really simplistic sampler using SDLImage
struct SDL_Surface;
class sampler2D : public swizzle::glsl::texture_functions::tag
{
public:
    enum WrapMode
    {
        Clamp,
        Repeat,
        MirrorRepeat
    };

    typedef const vec2& tex_coord_type;

    sampler2D(const char* path, WrapMode wrapMode);
    ~sampler2D();
    vec4 sample(const vec2& coord);

private:
    SDL_Surface *m_image;
    WrapMode m_wrapMode;

    // do not allow copies to be made
    sampler2D(const sampler2D&);
    sampler2D& operator=(const sampler2D&);
};

// this where the magic happens...
namespace glsl_sandbox
{
    // a nested namespace used when redefining 'inout' and 'out' keywords
    namespace ref
    {
#ifdef CXXSWIZZLE_VECTOR_INOUT_WRAPPER_ENABLED
        typedef swizzle::detail::vector_inout_wrapper<vec2> vec2;
        typedef swizzle::detail::vector_inout_wrapper<vec3> vec3;
        typedef swizzle::detail::vector_inout_wrapper<vec4> vec4;
#else
        typedef vec2& vec2;
        typedef vec3& vec3;
        typedef vec4& vec4;
#endif
        typedef ::float_type& float_type;
    }

    namespace in
    {
        typedef const ::vec2& vec2;
        typedef const ::vec3& vec3;
        typedef const ::vec4& vec4;
        typedef const ::float_type& float_type;
    }

    #include <swizzle/glsl/vector_functions.h>

    // constants shaders are using
    float_type time = 1;
    vec2 mouse(0, 0);
    vec2 resolution;

    // constants some shaders from shader toy are using
    vec2& iResolution = resolution;
    float_type& iGlobalTime = time;
    vec2& iMouse = mouse;

    sampler2D diffuse("diffuse.png", sampler2D::Repeat);
    sampler2D specular("specular.png", sampler2D::Repeat);

    struct fragment_shader
    {
        vec2 gl_FragCoord;
        vec4 gl_FragColor;
        void operator()(void);
    };

    // change meaning of glsl keywords to match sandbox
    #define uniform extern
    #define in in::
    #define out ref::
    #define inout ref::
    #define main fragment_shader::operator()
    #define float float_type   
    #define bool bool_type
    
    #pragma warning(push)
    #pragma warning(disable: 4244) // disable return implicit conversion warning
    #pragma warning(disable: 4305) // disable truncation warning
    
    //#include "shaders/sampler.frag"
    //#include "shaders/leadlight.frag"
    //#include "shaders/terrain.frag"
    //#include "shaders/complex.frag"
    //#include "shaders/road.frag"
    //#include "shaders/gears.frag"
    //#include "shaders/water_turbulence.frag"
    #include "shaders/sky.frag"

    // be a dear a clean up
    #pragma warning(pop)
    #undef bool
    #undef float
    #undef main
    #undef in
    #undef out
    #undef inout
    #undef uniform
}

// these headers, especially SDL.h & time.h set up names that are in conflict with sandbox'es;
// sandbox should be moved to a separate h/cpp pair, but out of laziness... including them
// *after* sandbox solves it too

#include <iostream>
#include <sstream>
#include <SDL.h>

#ifdef SDLIMAGE_FOUND
#include <SDL_image.h>
#endif

#include <time.h>
#include <memory>
#include <functional>
#if OMP_ENABLED
#include <omp.h>
#endif

//! A handy way of creating (and checking) unique_ptrs of SDL objects
template <class T>
std::unique_ptr< T, std::function<void (T*)> > makeUnique(T* value, std::function<void (T*)> deleter)
{
    if (!value)
    {
        throw std::runtime_error("Null pointer");
    }
    return std::unique_ptr<T, decltype(deleter)>(value, deleter);
}

//! As above, but allows null initialisation
template <class T>
std::unique_ptr< T, std::function<void (T*)> > makeUnique(std::function<void (T*)> deleter)
{
    return std::unique_ptr<T, decltype(deleter)>(nullptr, deleter);
}

//! Just a RAII wrapper around SDL_mutex
struct ScopedLock
{
    SDL_mutex* mutex;

    explicit ScopedLock( SDL_mutex* mutex ) : mutex(mutex)
    {
        SDL_LockMutex(mutex);
    }

    template <class T>
    explicit  ScopedLock( std::unique_ptr<SDL_mutex, T>& mutex ) : mutex(mutex.get())
    {
        SDL_LockMutex(this->mutex);
    }

    ~ScopedLock()
    {
        SDL_UnlockMutex(mutex);
    }
};


//! The surface to draw on.
auto g_surface = makeUnique<SDL_Surface>( SDL_FreeSurface );
//! Mutex used when exchaning frame between threads
auto g_frameHandshakeMutex = makeUnique<SDL_mutex>( SDL_CreateMutex(), SDL_DestroyMutex );
//! Signaled when a frame has been processed
auto m_frameReceivedEvent = makeUnique<SDL_cond>( SDL_CreateCond(), SDL_DestroyCond );
//! Signaled when a frame is ready to be processed
auto m_frameReadyEvent = makeUnique<SDL_cond>( SDL_CreateCond(), SDL_DestroyCond );
//! Additional flag set when a frame becomes ready, in case main thread is not waiting
bool g_frameReady = false;
//! Stop drawing
bool g_cancelDraw = false;
//! Quit!
bool g_quit = false;

const float_type c_one = 1.0f;
const float_type c_zero = 0.0f;

template <size_t Align, typename T>
T* alignPtr(T* ptr)
{
    static_assert((Align & (Align - 1)) == 0, "Align needs to be a power of two");
    auto value = reinterpret_cast<ptrdiff_t>(ptr);
    return reinterpret_cast<T*>((value + Align) & (~(Align - 1)));
}

//! Thread used for rendering; it invokes the shader
static int renderThread(void*)
{
    using ::swizzle::detail::static_for;

    // feel with 0...scalar_count
    raw_float_type offsets;
    {
        // well... this calls for an explanation: why not std::aligned_storage?
        // turns out there's a thing like max_align_t that defines max possible
        // align; SSE/AVX data has greater align than max_align_t on compilers
        // I checked, so std::aligned_storage is useless here.

        uint8_t unalignedBlob[scalar_count * sizeof(float) + float_entries_align];
        float* aligned = alignPtr<float_entries_align>(reinterpret_cast<float*>(unalignedBlob));
        static_for<0, scalar_count>([&](size_t i) { aligned[i] = static_cast<float>(i); });

        load_aligned(offsets, aligned);
    }
   

    while (true)
    {
        auto bmp = g_surface.get();

#if !defined(_DEBUG) && OMP_ENABLED
#pragma omp parallel 
        {
            int thredsCount = omp_get_num_threads();
            int threadNum = omp_get_thread_num();

            int heightStep = thredsCount;
            int heightStart = threadNum;
            int heightEnd = bmp->h;
#else
        {
            int heightStep = 1;
            int heightStart = 0;
            int heightEnd = bmp->h;
#endif
            // check the comment above for explanation
            unsigned unalignedBlob[3 * (scalar_count + uint_entries_align / sizeof(unsigned))];
            unsigned* pr = alignPtr<uint_entries_align>(unalignedBlob);
            unsigned* pg = alignPtr<uint_entries_align>(pr + scalar_count);
            unsigned* pb = alignPtr<uint_entries_align>(pg + scalar_count);

            glsl_sandbox::fragment_shader shader;
  
            for (int y = heightStart; !g_cancelDraw && y < heightEnd; y += heightStep)
            {
                shader.gl_FragCoord.y = static_cast<float>(bmp->h - 1 - y);

                uint8_t * ptr = reinterpret_cast<uint8_t*>(bmp->pixels) + y * bmp->pitch;

                int limitX = bmp->w - scalar_count;
                for (int x = 0; x < bmp->w; x += scalar_count)
                {
                    // since we are likely moving by more than one pixel,
                    // this will shift x and ptr left in case of width and scalar_count
                    // not being aligned; will redraw up to (scalar_count-1) pixels,
                    // but well, what you gonna do.
                    if (x > limitX)
                    {
                        ptr -= 3 * (x - limitX);
                        x = limitX;
                    }

                    shader.gl_FragCoord.x = static_cast<float>(x) + offsets;
                    
                    // vvvvvvvvvvvvvvvvvvvvvvvvvv
                    // THE SHADER IS INVOKED HERE
                    // ^^^^^^^^^^^^^^^^^^^^^^^^^^
                    shader();

                    // convert to [0;255]
                    auto color = glsl_sandbox::clamp(shader.gl_FragColor, c_zero, c_one);
                    color *= 255 + 0.5f;

                    // save in the bitmap
                    store_aligned(static_cast<uint_type>(static_cast<raw_float_type>(color.r)), pr);
                    store_aligned(static_cast<uint_type>(static_cast<raw_float_type>(color.g)), pg);
                    store_aligned(static_cast<uint_type>(static_cast<raw_float_type>(color.b)), pb);

                    static_for<0, scalar_count>([&](size_t i)
                    {
                        *ptr++ = static_cast<uint8_t>(pr[i]);
                        *ptr++ = static_cast<uint8_t>(pg[i]);
                        *ptr++ = static_cast<uint8_t>(pb[i]);
                    });
                }
            }
        }

        ScopedLock lock(g_frameHandshakeMutex);
        if ( g_quit )
        {
            return 0;
        }
        else
        {
            // frame is ready, change bool and raise signal (in case main thread is waiting)
            g_frameReady = true;
            SDL_CondSignal(m_frameReadyEvent.get());

            // wait for the main thread to process the frame
            SDL_CondWait(m_frameReceivedEvent.get(), g_frameHandshakeMutex.get());
            if ( g_quit )
            {
                return 0;
            }
        }
    }
}



extern "C" int main(int argc, char* argv[])
{
    using namespace std;

#ifdef SDLIMAGE_FOUND
    // initialise SDLImage
    int flags = IMG_INIT_JPG | IMG_INIT_PNG;
    int initted = IMG_Init(flags);
    if ((initted & flags) != flags) 
    {
        cerr << "WARNING: failed to initialise required jpg and png support: " << IMG_GetError() << endl;
    }
#endif

    // get initial resolution
    swizzle::glsl::vector<int, 2> initialResolution;
    initialResolution.x = 128;
    initialResolution.y = 128;
    if (argc == 2)
    {
        std::stringstream s;
        s << argv[1];
        if ( !(s >> initialResolution) )
        {
            cerr << "ERROR: unable to parse resolution argument" << endl;
            return 1;
        }
    }

    if ( initialResolution.x <= 0 || initialResolution.y < 0 )
    {
        cerr << "ERROR: invalid resolution: " << initialResolution  << endl;
        return 1;
    }

    cout << "\n";
    cout << "+/-   - increase/decrease time scale\n";
    cout << "lmb   - update glsl_sandbox::mouse\n";
    cout << "space - blit now! (show incomplete render)\n";
    cout << "esc   - quit\n\n";

    // it doesn't need cleaning up
    SDL_Surface* screen = nullptr;

    try 
    {
        // a function to resize the screen; throws if unsuccessful
        auto resizeOrCreateScreen = [&](int w, int h) -> void
        {
            screen = SDL_SetVideoMode( w, h, 24, SDL_SWSURFACE | SDL_RESIZABLE);
            if ( !screen )
            {
                throw std::runtime_error("Unable to set video mode");
            }
        };

        // a function used to resize the surface
        auto resizeOrCreateSurface = [&](int w, int h) -> void
        {
            g_surface.reset( SDL_CreateRGBSurface(SDL_SWSURFACE, w, h, 24, 0x000000ff, 0x0000ff00, 0x00ff0000, 0 ) );
            if ( !g_surface )
            {
                throw std::runtime_error("Unable to create surface");
            }
            // update shader value
            glsl_sandbox::resolution.x = static_cast<float>(w);
            glsl_sandbox::resolution.y = static_cast<float>(h);
        };

        // initial setup
        if (SDL_Init( SDL_INIT_VIDEO ) < 0 )
        {
            throw std::runtime_error("Unable to init SDL");
        }
        SDL_EnableKeyRepeat(200, 16);
        SDL_WM_SetCaption("SDL/Swizzle", "SDL/Swizzle");

        resizeOrCreateScreen(initialResolution.x, initialResolution.y);
        resizeOrCreateSurface(initialResolution.x, initialResolution.y);
        
        float timeScale = 1;
        int frame = 0;
        float time = 0;
        vec2 mousePosition(0, 0);
        bool pendingResize = false;
        bool mousePressed = false;


        auto renderThreadInstance = SDL_CreateThread(renderThread, nullptr);

        clock_t begin = clock();
        clock_t frameBegin = begin;
        float lastFPS = 0;

        while (!g_quit) 
        {
            bool blitNow = false;

            // process events
            SDL_Event event;
            while (SDL_PollEvent(&event)) 
            {
                switch ( event.type ) 
                {
                case SDL_VIDEORESIZE:
                    if ( event.resize.w != screen->w || event.resize.h != screen->h )
                    {
                        resizeOrCreateScreen( event.resize.w, event.resize.h );
                        ScopedLock lock(g_frameHandshakeMutex);
                        g_cancelDraw = pendingResize = true;
                    }
                    break;
                case SDL_QUIT:
                    {
                        ScopedLock lock(g_frameHandshakeMutex);
                        g_quit = g_cancelDraw = true;
                    }
                    break; 
                case SDL_KEYDOWN:
                    switch ( event.key.keysym.sym ) 
                    {
                    case SDLK_SPACE:
                        blitNow = true;
                        break;
                    case SDLK_ESCAPE:
                        {
                            ScopedLock lock(g_frameHandshakeMutex);
                            g_quit = g_cancelDraw = true;
                        }
                        break;
                    case SDLK_PLUS:
                    case SDLK_EQUALS:
                        timeScale *= 2.0f;
                        break;
                    case SDLK_MINUS:
                        timeScale /= 2.0f;
                        break;
                    default:
                        break;
                    }
                    break;
                case SDL_MOUSEMOTION:
                    if (mousePressed)
                    {
                        mousePosition.x = static_cast<float>(event.button.x);
                        mousePosition.y = static_cast<float>(g_surface->h - 1 - event.button.y);
                    }
                    break;
                case SDL_MOUSEBUTTONDOWN:
                    mousePressed = true;
                    mousePosition.x = static_cast<float>(event.button.x);
                    mousePosition.y = static_cast<float>(g_surface->h - 1 - event.button.y);
                    break;
                case SDL_MOUSEBUTTONUP:
                    mousePressed = false;
                default:
                    break;
                }
            }

            bool doFlip = false;
            {
                ScopedLock lock(g_frameHandshakeMutex);
                if ( g_quit )
                {
                    if ( g_frameReady )
                    {
                        // unlock waiting thread
                        SDL_CondSignal( m_frameReceivedEvent.get() );
                    }
                }
                // if either the flag is set or variable has been signaled do the blit
                else if ( blitNow || g_frameReady || SDL_CondWaitTimeout(m_frameReadyEvent.get(), g_frameHandshakeMutex.get(), 33) == 0 )
                {
                    doFlip = true;
                    SDL_BlitSurface( g_surface.get(), NULL, screen, NULL );

                    if ( pendingResize )
                    {
                        resizeOrCreateSurface(screen->w, screen->h);
                        pendingResize = false;
                    }

                    if (g_frameReady)
                    {
                        auto currClock = clock();
                        lastFPS = 1.0f / static_cast<float>((currClock - frameBegin) / double(CLOCKS_PER_SEC));
                        frameBegin = currClock;
                    }

                    if (!blitNow || g_frameReady)
                    {
                        // transfer variables (resolution is transfered elsewhere)
                        glsl_sandbox::time = time;
                        glsl_sandbox::mouse = mousePosition / vec2(screen->w, screen->h);
                        // reset flags
                        g_cancelDraw = g_frameReady = false;
                        SDL_CondSignal( m_frameReceivedEvent.get() );
                    }
                }
            }

            if (doFlip)
            {
                ++frame;
                SDL_Flip( screen );
            }

            cout << "frame: " << frame << "\t time: " << time << "\t timescale: " << timeScale << "\t fps: " << lastFPS << "     \r";
            cout.flush();

            clock_t delta = clock() - begin;
            time += static_cast<float>(delta / double(CLOCKS_PER_SEC) * timeScale);
            begin = clock();
        }

        // wait for the render thread to stop
        cout << "\nwaiting for the worker thread to finish...";
        SDL_WaitThread(renderThreadInstance, nullptr);
    } 
    catch ( exception& error ) 
    {
        cerr << "ERROR: " << error.what() << endl;
    } 
    catch (...) 
    {
        cerr << "ERROR: Unknown error" << endl;
    }

    SDL_Quit();
    return 0; 
}


sampler2D::sampler2D( const char* path, WrapMode wrapMode ) 
    : m_wrapMode(wrapMode)
    , m_image(nullptr)
{
#ifdef SDLIMAGE_FOUND
    m_image = IMG_Load(path);
    if (!m_image)
    {
        std::cerr << "WARNING: Failed to load texture " << path << "\n";
        std::cerr << "  SDL_Image message: " << IMG_GetError() << "\n";
    }
#else
    std::cerr << "WARNING: Texture " << path << " won't be loaded, SDL_image was not found.\n";
#endif

}

sampler2D::~sampler2D()
{
    if ( m_image )
    {
        SDL_FreeSurface(m_image);
        m_image = nullptr;
    }
}

vec4 sampler2D::sample( const vec2& coord )
{
    using namespace glsl_sandbox;
    vec2 uv;
    switch (m_wrapMode)
    {
    case Repeat:
        uv = mod(coord, 1);
        break;
    case MirrorRepeat:
        uv = abs(mod(coord - 1, 2) - 1);
        break;
    case Clamp:
    default:
        uv = clamp(coord, 0, 1);
        break;
    }

    // OGL uses left-bottom corner as origin...
    uv.y = 1 - uv.y;

    if ( !m_image )
    {
        // checkers
        auto s = step(0.5f, uv);
        auto m2 = abs(s.x - s.y);
        return mix(vec4(1, 0, 0, 1), vec4(0, 1, 0, 1), m2);
        /*if (uv_x < 0.5 && uv_y < 0.5 || uv_x > 0.5 && uv_y > 0.5)
        {
            return vec4(1, 0, 0, 1);
        }
        else
        {
            return vec4(0, 1, 0, 1);
        }*/
    }
    else
    {
        uint_type x = static_cast<uint_type>(static_cast<raw_float_type>(uv.x * (m_image->w - 1) + 0.5));
        uint_type y = static_cast<uint_type>(static_cast<raw_float_type>(uv.y * (m_image->h - 1) + 0.5));

        auto& format = *m_image->format;
        uint_type index = (y * m_image->pitch + x * format.BytesPerPixel);

        // stack-alloc blob for storing indices and color components
        uint8_t unalignedBlob[5 * (scalar_count * sizeof(unsigned) + uint_entries_align)];
        unsigned* pindex = alignPtr<uint_entries_align>(reinterpret_cast<unsigned*>(unalignedBlob));
        unsigned* pr = alignPtr<uint_entries_align>(pindex + scalar_count);
        unsigned* pg = alignPtr<uint_entries_align>(pr + scalar_count);
        unsigned* pb = alignPtr<uint_entries_align>(pg + scalar_count);
        unsigned* pa = alignPtr<uint_entries_align>(pb + scalar_count);

        store_aligned(index, pindex);
        
        // fill the buffers
        swizzle::detail::static_for<0, scalar_count>([&](size_t i)
        {
            auto pixelPtr = static_cast<uint8_t*>(m_image->pixels) + pindex[i];
            
            uint32_t pixel = 0;
            for (size_t i = 0; i < format.BytesPerPixel; ++i)
            {
                pixel |= (pixelPtr[i] << (i * 8));
            }

            pr[i] = (pixel & format.Rmask) >> format.Rshift;
            pg[i] = (pixel & format.Gmask) >> format.Gshift;
            pb[i] = (pixel & format.Bmask) >> format.Bshift;
            pa[i] = format.Amask ? ((pixel & format.Amask) >> format.Ashift) : 255;
        });

        // load data
        uint_type r, g, b, a;
        load_aligned(r, pr);
        load_aligned(g, pg);
        load_aligned(b, pb);
        load_aligned(a, pa);

        vec4 result;
        result.r = static_cast<raw_float_type>(r);
        result.g = static_cast<raw_float_type>(g);
        result.b = static_cast<raw_float_type>(b);
        result.a = static_cast<raw_float_type>(a);

        return clamp(result / 255.0f, c_zero, c_one);
    }
}