/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2025 Sam Lantinga <slouken@libsdl.org>

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
#include "SDL_internal.h"

#ifdef SDL_TIMER_DOS

#include <time.h>   /* uclock, uclock_t, UCLOCKS_PER_SEC */
#include <dos.h>    /* delay */

/* DJGPP's uclock() reprograms PIT channel 0 for a higher tick rate on first
   call, giving ~1.19 MHz resolution (UCLOCKS_PER_SEC == 1193180).  This is
   the same approach SDL2-dos used and gives sub-microsecond precision without
   any extra setup. */

Uint64 SDL_GetPerformanceCounter(void)
{
    return (Uint64)uclock();
}

Uint64 SDL_GetPerformanceFrequency(void)
{
    return (Uint64)UCLOCKS_PER_SEC;
}

void SDL_SYS_DelayNS(Uint64 ns)
{
    /* For delays >= 1 ms, use DJGPP's delay() which yields to the DPMI host.
       For sub-millisecond delays, busy-wait on uclock(). */
    if (ns >= SDL_NS_PER_MS) {
        Uint32 ms = (Uint32)(ns / SDL_NS_PER_MS);
        if (ms > 0) {
            delay(ms);
        }
        ns -= (Uint64)ms * SDL_NS_PER_MS;
    }

    /* Busy-wait for any remaining sub-millisecond portion */
    if (ns > 0) {
        const uclock_t target_ticks = (uclock_t)((ns * UCLOCKS_PER_SEC) / SDL_NS_PER_SECOND);
        const uclock_t start = uclock();
        while ((uclock() - start) < target_ticks) {
            /* spin */
        }
    }
}

#endif /* SDL_TIMER_DOS */