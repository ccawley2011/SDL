/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2022 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/

/**
 *  \file SDL_intrin.h
 *
 *  Common include for CPU intrinsics.
 */

#ifndef SDL_intrin_h_
#define SDL_intrin_h_

#include "SDL_stdinc.h"

/* Need to do this here because intrin.h has C++ code in it */
/* Visual Studio 2005 has a bug where intrin.h conflicts with winnt.h */
#if defined(_MSC_VER) && (_MSC_VER >= 1500) && (defined(_M_IX86) || defined(_M_X64))

#ifdef __clang__
/* As of Clang 11, '_m_prefetchw' is conflicting with the winnt.h's version,
   so we define the needed '_m_prefetch' here as a pseudo-header, until the issue is fixed. */

#ifndef __PRFCHWINTRIN_H
#define __PRFCHWINTRIN_H

static __inline__ void __attribute__((__always_inline__, __nodebug__))
_m_prefetch(void *__P)
{
  __builtin_prefetch (__P, 0, 3 /* _MM_HINT_T0 */);
}

#endif /* __PRFCHWINTRIN_H */
#endif /* __clang__ */

#include <intrin.h>

#ifndef _WIN64
#define HAVE_MMX_INTRINSICS 1
#define HAVE_3DNOW_INTRINSICS 1
#endif
#define HAVE_SSE_INTRINSICS 1
#define HAVE_SSE2_INTRINSICS 1
#define HAVE_SSE3_INTRINSICS 1
#if !defined(__clang__) || defined(__AVX__)
# define HAVE_AVX_INTRINSICS 1
#endif

#elif defined(_MSC_VER) && defined(_M_ARM64)
#include <arm64intr.h>
#include <arm64_neon.h>

#define HAVE_NEON_INTRINSICS 1

#elif defined(_MSC_VER) && defined(_M_ARM)
#include <armintr.h>
#include <arm_neon.h>

#define HAVE_NEON_INTRINSICS 1

#else
#ifdef __MMX__
#define HAVE_MMX_INTRINSICS 1
#endif
#ifdef __3dNOW__
#define HAVE_3DNOW_INTRINSICS 1
#endif
#ifdef __SSE__
#define HAVE_SSE_INTRINSICS 1
#endif
#ifdef __SSE2__
#define HAVE_SSE2_INTRINSICS 1
#endif
#ifdef __SSE3__
#define HAVE_SSE3_INTRINSICS 1
#endif
#ifdef __AVX__
#define HAVE_AVX_INTRINSICS 1
#endif
#ifdef __ALTIVEC__
#define HAVE_ALTIVEC_INTRINSICS 1
#endif
#ifdef __ARM_NEON
#define HAVE_NEON_INTRINSICS 1
#endif

#if defined(HAVE_IMMINTRIN_H) && !defined(SDL_DISABLE_IMMINTRIN_H)
#define HAVE_AVX_INTRINSICS 1
#endif
#if defined __clang__
# if (__has_attribute(target))
#   define SDL_MMX_TARGET    __attribute__((target("mmx")))
#   define SDL_3DNOW_TARGET  __attribute__((target("3dnow")))
#   define SDL_SSE_TARGET    __attribute__((target("sse")))
#   define SDL_SSE2_TARGET   __attribute__((target("sse2")))
#   define SDL_SSE3_TARGET   __attribute__((target("sse3")))
#   define SDL_AVX_TARGET    __attribute__((target("avx")))
# endif
#elif defined __GNUC__
# if (__GNUC__ < 4) || (__GNUC__ == 4 && __GNUC_MINOR__ < 9)
#   undef HAVE_AVX_INTRINSICS
# endif
#endif

/* MSVC will always accept AVX intrinsics when compiling for x64 */
#if defined(__clang__) || defined(__GNUC__)
#define SDL_MMX_TARGET    __attribute__((target("mmx")))
#define SDL_3DNOW_TARGET  __attribute__((target("3dnow")))
#define SDL_SSE_TARGET    __attribute__((target("sse")))
#define SDL_SSE2_TARGET   __attribute__((target("sse2")))
#define SDL_SSE3_TARGET   __attribute__((target("sse3")))
#define SDL_AVX_TARGET    __attribute__((target("avx")))
#endif

/* altivec.h redefining bool causes a number of problems, see bugs 3993 and 4392, so you need to explicitly define SDL_ENABLE_ALTIVEC_H to have it included. */
#if defined(HAVE_ALTIVEC_H) && defined(__ALTIVEC__) && !defined(__APPLE_ALTIVEC__) && defined(SDL_ENABLE_ALTIVEC_H)
#include <altivec.h>
#endif
#if !defined(SDL_DISABLE_ARM_NEON_H) && defined(__ARM_NEON)
#include <arm_neon.h>
#endif
#if defined(__MINGW64_VERSION_MAJOR)
#include <intrin.h>
#else
#if defined(__3dNOW__) && !defined(SDL_DISABLE_MM3DNOW_H)
#include <mm3dnow.h>
#endif
#if defined(HAVE_IMMINTRIN_H) && !defined(SDL_DISABLE_IMMINTRIN_H)
#include <immintrin.h>
#else
#if defined(HAVE_MMX_INTRINSICS) && !defined(SDL_DISABLE_MMINTRIN_H)
#include <mmintrin.h>
#endif
#if defined(HAVE_SSE_INTRINSICS) && !defined(SDL_DISABLE_XMMINTRIN_H)
#include <xmmintrin.h>
#endif
#if defined(HAVE_SSE2_INTRINSICS) && !defined(SDL_DISABLE_EMMINTRIN_H)
#include <emmintrin.h>
#endif
#if defined(HAVE_SSE3_INTRINSICS) && !defined(SDL_DISABLE_PMMINTRIN_H)
#include <pmmintrin.h>
#endif
#endif /* HAVE_IMMINTRIN_H */
#endif /* __MINGW64_VERSION_MAJOR */

#endif

#ifndef SDL_ALTIVEC_TARGET
#define SDL_ALTIVEC_TARGET
#endif
#ifndef SDL_NEON_TARGET
#define SDL_NEON_TARGET
#endif
#ifndef SDL_MMX_TARGET
#define SDL_MMX_TARGET
#endif
#ifndef SDL_3DNOW_TARGET
#define SDL_3DNOW_TARGET
#endif
#ifndef SDL_SSE_TARGET
#define SDL_SSE_TARGET
#endif
#ifndef SDL_SSE2_TARGET
#define SDL_SSE2_TARGET
#endif
#ifndef SDL_SSE3_TARGET
#define SDL_SSE3_TARGET
#endif
#ifndef SDL_AVX_TARGET
#define SDL_AVX_TARGET
#endif

#endif /* SDL_intrin_h_ */

/* vi: set ts=4 sw=4 expandtab: */
