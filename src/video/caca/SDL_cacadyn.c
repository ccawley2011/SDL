/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2023 Sam Lantinga <slouken@libsdl.org>

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

#ifdef SDL_VIDEO_DRIVER_CACA

#define DEBUG_DYNAMIC_CACA 0

#include "SDL_cacadyn.h"

#ifdef SDL_VIDEO_DRIVER_CACA_DYNAMIC

typedef struct
{
    void *lib;
    const char *libname;
} cacadynlib;

static cacadynlib cacalibs[] = {
    { NULL, SDL_VIDEO_DRIVER_CACA_DYNAMIC }
};

static void *CACA_GetSym(const char *fnname, int *pHasModule)
{
    int i;
    void *fn = NULL;
    for (i = 0; i < SDL_arraysize(cacalibs); i++) {
        if (cacalibs[i].lib) {
            fn = SDL_LoadFunction(cacalibs[i].lib, fnname);
            if (fn) {
                break;
            }
        }
    }

#if DEBUG_DYNAMIC_CACA
    if (fn)
        SDL_Log("CACA: Found '%s' in %s (%p)\n", fnname, cacalibs[i].libname, fn);
    else
        SDL_Log("CACA: Symbol '%s' NOT FOUND!\n", fnname);
#endif

    if (!fn) {
        *pHasModule = 0; /* kill this module. */
    }

    return fn;
}

#endif /* SDL_VIDEO_DRIVER_CACA_DYNAMIC */

/* Define all the function pointers and wrappers... */
#define SDL_CACA_MODULE(modname)       int SDL_CACA_HAVE_##modname = 0;
#define SDL_CACA_SYM(rc, fn, params)   SDL_DYNCACAFN_##fn CACA_##fn = NULL;
#define SDL_CACA_SYM_CONST(type, name) SDL_DYNCACACONST_##name CACA_##name = NULL;
#include "SDL_cacasym.h"

static int caca_load_refcount = 0;

void SDL_CACA_UnloadSymbols(void)
{
    /* Don't actually unload if more than one module is using the libs... */
    if (caca_load_refcount > 0) {
        if (--caca_load_refcount == 0) {
#ifdef SDL_VIDEO_DRIVER_CACA_DYNAMIC
            int i;
#endif

            /* set all the function pointers to NULL. */
#define SDL_CACA_MODULE(modname)       SDL_CACA_HAVE_##modname = 0;
#define SDL_CACA_SYM(rc, fn, params)   CACA_##fn = NULL;
#define SDL_CACA_SYM_CONST(type, name) CACA_##name = NULL;
#include "SDL_cacasym.h"

#ifdef SDL_VIDEO_DRIVER_CACA_DYNAMIC
            for (i = 0; i < SDL_arraysize(cacalibs); i++) {
                if (cacalibs[i].lib) {
                    SDL_UnloadObject(cacalibs[i].lib);
                    cacalibs[i].lib = NULL;
                }
            }
#endif
        }
    }
}

/* returns non-zero if all needed symbols were loaded. */
int SDL_CACA_LoadSymbols(void)
{
    int rc = 1; /* always succeed if not using Dynamic CACA stuff. */

    /* deal with multiple modules needing these symbols... */
    if (caca_load_refcount++ == 0) {
#ifdef SDL_VIDEO_DRIVER_CACA_DYNAMIC
        int i;
        int *thismod = NULL;
        for (i = 0; i < SDL_arraysize(cacalibs); i++) {
            if (cacalibs[i].libname) {
                cacalibs[i].lib = SDL_LoadObject(cacalibs[i].libname);
            }
        }

#define SDL_CACA_MODULE(modname) SDL_CACA_HAVE_##modname = 1; /* default yes */
#include "SDL_cacasym.h"

#define SDL_CACA_MODULE(modname)       thismod = &SDL_CACA_HAVE_##modname;
#define SDL_CACA_SYM(rc, fn, params)   CACA_##fn = (SDL_DYNCACAFN_##fn)CACA_GetSym(#fn, thismod);
#define SDL_CACA_SYM_CONST(type, name) CACA_##name = *(SDL_DYNCACACONST_##name *)CACA_GetSym(#name, thismod);
#include "SDL_cacasym.h"

        if (SDL_CACA_HAVE_CACA) {
            /* all required symbols loaded. */
            SDL_ClearError();
        } else {
            /* in case something got loaded... */
            SDL_CACA_UnloadSymbols();
            rc = 0;
        }

#else /* no dynamic CACA */

#define SDL_CACA_MODULE(modname)       SDL_CACA_HAVE_##modname = 1; /* default yes */
#define SDL_CACA_SYM(rc, fn, params)   CACA_##fn = fn;
#define SDL_CACA_SYM_CONST(type, name) CACA_##name = name;
#include "SDL_cacasym.h"

#endif
    }

    return rc;
}

#endif /* SDL_VIDEO_DRIVER_CACA */
