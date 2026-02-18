#pragma once
/*
 * SDL 1.2 → SDL2 compatibility shims.
 *
 * This header maps SDL 1.2 types, macros, and function names to their
 * SDL2 equivalents so that the bulk of the OpenXcom source can build
 * against SDL2 with minimal changes.
 *
 * Include this AFTER the real SDL headers.
 */
#include <SDL.h>
#include <string>

/* ---- Types ---- */
/*
 * In SDL1, SDLKey was an enum (distinct from int).
 * In SDL2, SDL_Keycode is Sint32 (same as int), which breaks overloads
 * that distinguished int* from SDLKey*.
 * Solution: wrap in a struct that converts to/from int/SDL_Keycode.
 */
#ifndef SDLKEY_COMPAT_DEFINED
#define SDLKEY_COMPAT_DEFINED
struct SDLKey {
    Sint32 _val;
    SDLKey() = default;                  // trivial default ctor (required for unions)
    SDLKey(Sint32 v) : _val(v) {}       // implicit from int/SDL_Keycode/SDLK_*
    operator Sint32() const { return _val; } // implicit to int for comparisons
    bool operator<(const SDLKey& o) const { return _val < o._val; }
    bool operator==(const SDLKey& o) const { return _val == o._val; }
    bool operator!=(const SDLKey& o) const { return _val != o._val; }
    /* Explicit comparisons against raw SDL_Keycode to avoid ambiguity */
    friend bool operator==(const SDLKey& a, SDL_Keycode b) { return a._val == b; }
    friend bool operator==(SDL_Keycode a, const SDLKey& b) { return a == b._val; }
    friend bool operator!=(const SDLKey& a, SDL_Keycode b) { return a._val != b; }
    friend bool operator!=(SDL_Keycode a, const SDLKey& b) { return a != b._val; }
};
#endif

/* SDL_GrabMode was an enum in SDL1; SDL2 uses SDL_bool for grab */
#ifndef SDL_GrabMode
typedef SDL_bool SDL_GrabMode;
#define SDL_GRAB_OFF SDL_FALSE
#define SDL_GRAB_ON  SDL_TRUE
#endif

/* ---- Surface flags (most are gone in SDL2, map to 0) ---- */
#ifndef SDL_HWSURFACE
#define SDL_HWSURFACE  0
#endif
#ifndef SDL_SWSURFACE
#define SDL_SWSURFACE  0
#endif
#ifndef SDL_DOUBLEBUF
#define SDL_DOUBLEBUF  0
#endif
#ifndef SDL_HWPALETTE
#define SDL_HWPALETTE  0
#endif
#ifndef SDL_ANYFORMAT
#define SDL_ANYFORMAT  0
#endif
#ifndef SDL_ASYNCBLIT
#define SDL_ASYNCBLIT  0
#endif

/* Window flags mapped from SDL1 names */
#ifndef SDL_FULLSCREEN
#define SDL_FULLSCREEN SDL_WINDOW_FULLSCREEN
#endif
#ifndef SDL_RESIZABLE
#define SDL_RESIZABLE  SDL_WINDOW_RESIZABLE
#endif
#ifndef SDL_NOFRAME
#define SDL_NOFRAME    SDL_WINDOW_BORDERLESS
#endif
#ifndef SDL_OPENGL
#define SDL_OPENGL     SDL_WINDOW_OPENGL
#endif

/*
 * SDL_BUTTON_WHEELUP/WHEELDOWN: in SDL2, mouse wheel is an event (SDL_MOUSEWHEEL),
 * not a button press. Map to unlikely button IDs so existing comparisons compile.
 * The actual wheel logic in SDL2 should come from SDL_MOUSEWHEEL events.
 */
#ifndef SDL_BUTTON_WHEELUP
#define SDL_BUTTON_WHEELUP   4
#endif
#ifndef SDL_BUTTON_WHEELDOWN
#define SDL_BUTTON_WHEELDOWN 5
#endif

/* SDL_SRCCOLORKEY → SDL_TRUE (used with SDL_SetColorKey) */
#ifndef SDL_SRCCOLORKEY
#define SDL_SRCCOLORKEY SDL_TRUE
#endif

/* SDL_AllocSurface was removed – it was always a macro for SDL_CreateRGBSurface */
#ifndef SDL_AllocSurface
#define SDL_AllocSurface SDL_CreateRGBSurface
#endif

/* SDL_SetColors → SDL_SetPaletteColors */
static inline int SDL_SetColors(SDL_Surface *surface, SDL_Color *colors, int firstcolor, int ncolors)
{
    if (surface && surface->format && surface->format->palette)
        return SDL_SetPaletteColors(surface->format->palette, colors, firstcolor, ncolors) == 0 ? 1 : 0;
    return 0;
}

/* ---- Functions removed / renamed ---- */

/*
 * Global SDL2 window singleton.
 * SDL1's SDL_SetVideoMode returned a surface; SDL2 needs a window.
 * We keep one window alive for the lifetime of the app.
 * Declared early so all shims below can use it.
 */
namespace SDL2Compat {
    inline SDL_Window*& getWindow()
    {
        static SDL_Window* win = nullptr;
        return win;
    }
    inline SDL_Renderer*& getRenderer()
    {
        static SDL_Renderer* rend = nullptr;
        return rend;
    }
    inline SDL_Surface*& getOffscreenSurface()
    {
        static SDL_Surface* surf = nullptr;
        return surf;
    }
}

/* SDL_DEFAULT_REPEAT_DELAY / INTERVAL removed in SDL2 */
#ifndef SDL_DEFAULT_REPEAT_DELAY
#define SDL_DEFAULT_REPEAT_DELAY    500
#define SDL_DEFAULT_REPEAT_INTERVAL 30
#endif

/* SDL_EnableKeyRepeat: SDL2 has key repeat enabled by default; make this a no-op */
static inline int SDL_EnableKeyRepeat(int /*delay*/, int /*interval*/) { return 0; }

/* SDL_EnableUNICODE: gone in SDL2, text input events replace it */
static inline int SDL_EnableUNICODE(int /*enable*/) { return 0; }

/* SDL_WarpMouse → SDL_WarpMouseInWindow (no-op in Emscripten: warping doesn't move the
 * browser cursor and would make SDL's internal position wrong, triggering edge-scroll). */
static inline void SDL_WarpMouse(Uint16 x, Uint16 y)
{
#ifdef __EMSCRIPTEN__
    (void)x;
    (void)y;
#else
    SDL_Window *win = SDL2Compat::getWindow();
    if (!win) win = SDL_GetKeyboardFocus();
    if (win) SDL_WarpMouseInWindow(win, x, y);
#endif
}

/* SDL_putenv: gone in SDL2, use SDL_setenv */
#ifndef SDL_putenv
static inline int SDL_putenv(const char *variable)
{
    /* variable is "NAME=VALUE" */
    std::string var(variable);
    size_t eq = var.find('=');
    if (eq == std::string::npos) return -1;
    std::string name = var.substr(0, eq);
    std::string value = var.substr(eq + 1);
    return SDL_setenv(name.c_str(), value.c_str(), 1);
}
#endif

/* SDL_WM_GrabInput: replaced by SDL_SetWindowGrab */
static inline SDL_bool SDL_WM_GrabInput(SDL_bool mode)
{
    SDL_Window *win = SDL2Compat::getWindow();
    if (!win) win = SDL_GetKeyboardFocus();
    if (win) SDL_SetWindowGrab(win, mode);
    return mode;
}

/* SDL_WM_SetCaption: replaced by SDL_SetWindowTitle */
static inline void SDL_WM_SetCaption(const char *title, const char * /*icon*/)
{
    SDL_Window *win = SDL2Compat::getWindow();
    if (!win) win = SDL_GetKeyboardFocus();
    if (win) SDL_SetWindowTitle(win, title);
}

/* SDL_WM_GetCaption */
static inline void SDL_WM_GetCaption(char **title, char ** /*icon*/)
{
    SDL_Window *win = SDL2Compat::getWindow();
    if (!win) win = SDL_GetKeyboardFocus();
    if (title) *title = win ? (char*)SDL_GetWindowTitle(win) : (char*)"";
}

/* SDL_WM_SetIcon: replaced by SDL_SetWindowIcon */
static inline void SDL_WM_SetIcon(SDL_Surface *icon, Uint8 * /*mask*/)
{
    SDL_Window *win = SDL2Compat::getWindow();
    if (!win) win = SDL_GetKeyboardFocus();
    if (win) SDL_SetWindowIcon(win, icon);
}

/* SDL_Flip: upload offscreen surface to the window via renderer+texture.
 * This avoids SDL_UpdateWindowSurface which has macOS Retina offset bugs. */
static inline int SDL_Flip(SDL_Surface * /*screen*/)
{
    SDL_Renderer *rend = SDL2Compat::getRenderer();
    SDL_Surface  *surf = SDL2Compat::getOffscreenSurface();
    if (!rend || !surf) return -1;

    SDL_Texture *tex = SDL_CreateTextureFromSurface(rend, surf);
    if (!tex) return -1;
    SDL_RenderClear(rend);
    SDL_RenderCopy(rend, tex, NULL, NULL);
    SDL_RenderPresent(rend);
    SDL_DestroyTexture(tex);
    return 0;
}

/*
 * SDL_SetVideoMode → create/resize an SDL2 window and return an offscreen surface.
 * Uses SDL_Renderer + texture for display to avoid macOS Retina offset bugs
 * with SDL_GetWindowSurface / SDL_UpdateWindowSurface.
 */
static inline SDL_Surface* SDL_SetVideoMode(int width, int height, int bpp, Uint32 flags)
{
    (void)bpp; /* SDL2 manages bpp internally */
    SDL_Window*&   win  = SDL2Compat::getWindow();
    SDL_Renderer*& rend = SDL2Compat::getRenderer();
    SDL_Surface*&  surf = SDL2Compat::getOffscreenSurface();

    /* Translate SDL1-style flags to SDL2 window flags */
    Uint32 winFlags = SDL_WINDOW_SHOWN;
    if (flags & SDL_WINDOW_FULLSCREEN)  winFlags |= SDL_WINDOW_FULLSCREEN;
    if (flags & SDL_WINDOW_OPENGL)      winFlags |= SDL_WINDOW_OPENGL;
    if (flags & SDL_WINDOW_RESIZABLE)   winFlags |= SDL_WINDOW_RESIZABLE;
    if (flags & SDL_WINDOW_BORDERLESS)  winFlags |= SDL_WINDOW_BORDERLESS;

    if (win)
    {
        /* Resize existing window */
        SDL_SetWindowSize(win, width, height);

        /* Update fullscreen state */
        if (winFlags & SDL_WINDOW_FULLSCREEN)
            SDL_SetWindowFullscreen(win, SDL_WINDOW_FULLSCREEN);
        else
            SDL_SetWindowFullscreen(win, 0);
    }
    else
    {
        win = SDL_CreateWindow("OpenXcom",
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            width, height, winFlags);
        if (!win) return nullptr;
    }

    /* For OpenGL mode, the caller will manage GL context separately */
    if (winFlags & SDL_WINDOW_OPENGL)
    {
        return SDL_GetWindowSurface(win);
    }

    /* Create / recreate the renderer (destroys any previous one) */
    if (rend) SDL_DestroyRenderer(rend);
    rend = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!rend) rend = SDL_CreateRenderer(win, -1, 0); /* fallback */
    if (!rend) return nullptr;

    /* Create / recreate an offscreen 32-bit surface as the "screen" */
    if (surf) SDL_FreeSurface(surf);
    surf = SDL_CreateRGBSurface(0, width, height, 32,
#if SDL_BYTEORDER == SDL_BIG_ENDIAN
        0xFF000000, 0x00FF0000, 0x0000FF00, 0x000000FF
#else
        0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000
#endif
    );
    if (!surf) return nullptr;

    return surf;
}

/*
 * SDL_ListModes → use SDL_GetDisplayMode to enumerate modes.
 * Returns a static array of SDL_Rect* (terminated by NULL).
 * Only supports format=NULL, flags=SDL_FULLSCREEN.
 */
static inline SDL_Rect** SDL_ListModes(void* /*format*/, Uint32 /*flags*/)
{
    static SDL_Rect** modes = nullptr;
    if (modes)
    {
        /* Free previous */
        for (int i = 0; modes[i]; i++) SDL_free(modes[i]);
        SDL_free(modes);
    }

    int numModes = SDL_GetNumDisplayModes(0);
    if (numModes <= 0) return nullptr;

    modes = (SDL_Rect**)SDL_malloc(sizeof(SDL_Rect*) * (numModes + 1));
    for (int i = 0; i < numModes; i++)
    {
        SDL_DisplayMode dm;
        SDL_GetDisplayMode(0, i, &dm);
        modes[i] = (SDL_Rect*)SDL_malloc(sizeof(SDL_Rect));
        modes[i]->x = 0;
        modes[i]->y = 0;
        modes[i]->w = dm.w;
        modes[i]->h = dm.h;
    }
    modes[numModes] = nullptr;
    return modes;
}

/*
 * SDL_GetVideoInfo → return a static struct with current display info.
 * SDL1's SDL_VideoInfo had current_w/current_h; we emulate just that.
 */
struct SDL_VideoInfo_Compat
{
    int current_w;
    int current_h;
};

static inline const SDL_VideoInfo_Compat* SDL_GetVideoInfo()
{
    static SDL_VideoInfo_Compat info;
    SDL_DisplayMode dm;
    if (SDL_GetCurrentDisplayMode(0, &dm) == 0)
    {
        info.current_w = dm.w;
        info.current_h = dm.h;
    }
    else
    {
        info.current_w = 640;
        info.current_h = 480;
    }
    return &info;
}
/* Map the old type name */
#define SDL_VideoInfo SDL_VideoInfo_Compat

/*
 * SDL_gfx surface-based drawing primitives → SDL2_gfx renderer-based bridge.
 *
 * In SDL 1.2's SDL_gfx, drawing primitives took SDL_Surface* as the target.
 * In SDL2_gfx, they take SDL_Renderer*. We bridge by creating a temporary
 * software renderer from the surface, calling the real SDL2_gfx function,
 * and cleaning up.
 */
#include <SDL2_gfxPrimitives.h>

static inline int characterRGBA(SDL_Surface* s, Sint16 x, Sint16 y, char c, Uint8 r, Uint8 g, Uint8 b, Uint8 a) {
    SDL_Renderer* rend = SDL_CreateSoftwareRenderer(s);
    if (!rend) return -1;
    int ret = characterRGBA(rend, x, y, c, r, g, b, a);
    SDL_RenderPresent(rend);
    SDL_DestroyRenderer(rend);
    return ret;
}
static inline int lineRGBA(SDL_Surface* s, Sint16 x1, Sint16 y1, Sint16 x2, Sint16 y2, Uint8 r, Uint8 g, Uint8 b, Uint8 a) {
    SDL_Renderer* rend = SDL_CreateSoftwareRenderer(s);
    if (!rend) return -1;
    int ret = lineRGBA(rend, x1, y1, x2, y2, r, g, b, a);
    SDL_RenderPresent(rend);
    SDL_DestroyRenderer(rend);
    return ret;
}
static inline int stringRGBA(SDL_Surface* s, Sint16 x, Sint16 y, const char* str, Uint8 r, Uint8 g, Uint8 b, Uint8 a) {
    SDL_Renderer* rend = SDL_CreateSoftwareRenderer(s);
    if (!rend) return -1;
    int ret = stringRGBA(rend, x, y, str, r, g, b, a);
    SDL_RenderPresent(rend);
    SDL_DestroyRenderer(rend);
    return ret;
}
static inline int lineColor(SDL_Surface* s, Sint16 x1, Sint16 y1, Sint16 x2, Sint16 y2, Uint32 color) {
    SDL_Renderer* rend = SDL_CreateSoftwareRenderer(s);
    if (!rend) return -1;
    int ret = lineColor(rend, x1, y1, x2, y2, color);
    SDL_RenderPresent(rend);
    SDL_DestroyRenderer(rend);
    return ret;
}
static inline int filledCircleColor(SDL_Surface* s, Sint16 x, Sint16 y, Sint16 rad, Uint32 color) {
    SDL_Renderer* rend = SDL_CreateSoftwareRenderer(s);
    if (!rend) return -1;
    int ret = filledCircleColor(rend, x, y, rad, color);
    SDL_RenderPresent(rend);
    SDL_DestroyRenderer(rend);
    return ret;
}
static inline int filledPolygonColor(SDL_Surface* s, const Sint16* vx, const Sint16* vy, int n, Uint32 color) {
    SDL_Renderer* rend = SDL_CreateSoftwareRenderer(s);
    if (!rend) return -1;
    int ret = filledPolygonColor(rend, vx, vy, n, color);
    SDL_RenderPresent(rend);
    SDL_DestroyRenderer(rend);
    return ret;
}
static inline int texturedPolygon(SDL_Surface* s, const Sint16* vx, const Sint16* vy, int n, SDL_Surface* texture, int dx, int dy) {
    SDL_Renderer* rend = SDL_CreateSoftwareRenderer(s);
    if (!rend) return -1;
    int ret = texturedPolygon(rend, vx, vy, n, texture, dx, dy);
    SDL_RenderPresent(rend);
    SDL_DestroyRenderer(rend);
    return ret;
}
static inline int stringColor(SDL_Surface* s, Sint16 x, Sint16 y, const char* str, Uint32 color) {
    SDL_Renderer* rend = SDL_CreateSoftwareRenderer(s);
    if (!rend) return -1;
    int ret = stringColor(rend, x, y, str, color);
    SDL_RenderPresent(rend);
    SDL_DestroyRenderer(rend);
    return ret;
}

/* SDL_NOEVENT: removed in SDL2. Use SDL_FIRSTEVENT (0) as a "no event" marker. */
#ifndef SDL_NOEVENT
#define SDL_NOEVENT 0
#endif

/* KMOD_LMETA/RMETA → KMOD_LGUI/RGUI in SDL2 */
#ifndef KMOD_LMETA
#define KMOD_LMETA KMOD_LGUI
#endif
#ifndef KMOD_RMETA
#define KMOD_RMETA KMOD_RGUI
#endif
