#pragma once
/**
 * Cross-platform mouse button state helper.
 *
 * In Emscripten builds our JS input bridge pushes synthetic SDL events
 * via SDL_PushEvent, but that does NOT update SDL's internal button
 * tracker. SDL_GetMouseState() therefore always returns 0 for our
 * buttons, which breaks drag-scroll detection in Globe, Camera, etc.
 *
 * This header provides OX_GetMouseButtonState() which returns the real
 * button mask in both native and Emscripten builds.
 */

#ifdef __EMSCRIPTEN__
extern "C" unsigned int emGetMouseButtons();
#define OX_GetMouseButtonState() emGetMouseButtons()
#else
#include <SDL.h>
#define OX_GetMouseButtonState() SDL_GetMouseState(0, 0)
#endif
