// Platform compatibility layer
// Provides SDL-like utility macros on platforms where SDL is not available

#ifndef PORT_COMPAT_H
#define PORT_COMPAT_H

#ifdef __3DS__

#include <stdint.h>
#include <string.h>

// SDL-like zero/copy macros
#define SDL_zero(x) memset(&(x), 0, sizeof(x))
#define SDL_zeroa(x) memset((x), 0, sizeof(x))
#define SDL_zerop(p) memset((p), 0, sizeof(*(p)))
#define SDL_memmove memmove
#define SDL_copyp(dst, src) memcpy((dst), (src), sizeof(*(src)))
#define SDL_memset memset
#define SDL_memcpy memcpy
#define SDL_arraysize(x) (sizeof(x) / sizeof((x)[0]))

// SDL integer types
typedef int16_t Sint16;
typedef uint8_t Uint8;
typedef uint16_t Uint16;
typedef uint32_t Uint32;
typedef int32_t Sint32;
typedef uint64_t Uint64;

#define SDL_MAX_SINT16 INT16_MAX
#define SDL_ALPHA_OPAQUE 255
#define SDL_ALPHA_TRANSPARENT 0

#else

#include <SDL3/SDL.h>

#endif // __3DS__

#endif // PORT_COMPAT_H
