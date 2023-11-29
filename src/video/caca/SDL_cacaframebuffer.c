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

#include "../SDL_sysvideo.h"
#include "SDL_cacaframebuffer_c.h"
#include "SDL_cacawindow.h"

int CACA_CreateWindowFramebuffer(SDL_VideoDevice *_this, SDL_Window *window, Uint32 *format, void **pixels, int *pitch)
{
    SDL_WindowData *driverdata = window->driverdata;
    int w, h;

    /* Create a new framebuffer */
    SDL_GetWindowSizeInPixels(window, &w, &h);
    driverdata->surface = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGB565);
    if (!driverdata->surface) {
        return -1;
    }

    driverdata->dither = CACA_caca_create_dither(SDL_BITSPERPIXEL(driverdata->surface->format->format),
                                                 driverdata->surface->w,
                                                 driverdata->surface->h,
                                                 driverdata->surface->pitch,
                                                 driverdata->surface->format->Rmask,
                                                 driverdata->surface->format->Gmask,
                                                 driverdata->surface->format->Bmask,
                                                 driverdata->surface->format->Amask);
    if (!driverdata->dither) {
        SDL_DestroySurface(driverdata->surface);
        driverdata->surface = NULL;
        return SDL_SetError("Failed to create caca dither");
    }

    /* Save the info and return! */
    *format = driverdata->surface->format->format;
    *pixels = driverdata->surface->pixels;
    *pitch = driverdata->surface->pitch;
    return 0;
}

int CACA_UpdateWindowFramebuffer(SDL_VideoDevice *_this, SDL_Window *window, const SDL_Rect *rects, int numrects)
{
    SDL_WindowData *driverdata = window->driverdata;

    /* Send the data to the display */
    CACA_caca_dither_bitmap(driverdata->cv, 0, 0, driverdata->surface->w, driverdata->surface->h,
                            driverdata->dither, driverdata->surface->pixels);
    CACA_caca_refresh_display(driverdata->dp);

    return 0;
}

void CACA_DestroyWindowFramebuffer(SDL_VideoDevice *_this, SDL_Window *window)
{
    SDL_WindowData *driverdata = window->driverdata;

    if (driverdata->dither) {
        CACA_caca_free_dither(driverdata->dither);
        driverdata->dither = NULL;
    }
    if (driverdata->surface) {
        SDL_DestroySurface(driverdata->surface);
        driverdata->surface = NULL;
    }
}

#endif /* SDL_VIDEO_DRIVER_CACA */
