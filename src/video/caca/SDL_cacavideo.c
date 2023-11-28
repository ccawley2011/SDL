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
#include "../SDL_pixels_c.h"
#include "../../events/SDL_events_c.h"

#include "SDL_cacawindow.h"
#include "SDL_cacaevents_c.h"
#include "SDL_cacaframebuffer_c.h"

/* Initialization/Query functions */
static int CACA_VideoInit(SDL_VideoDevice *_this);
static void CACA_VideoQuit(SDL_VideoDevice *_this);

/* caca driver bootstrap functions */

static void CACA_DeleteDevice(SDL_VideoDevice *device)
{
    SDL_free(device);
}

static SDL_VideoDevice *CACA_CreateDevice(void)
{
    SDL_VideoDevice *device;

    /* Initialize all variables that we clean on shutdown */
    device = (SDL_VideoDevice *)SDL_calloc(1, sizeof(SDL_VideoDevice));
    if (!device) {
        SDL_OutOfMemory();
        return NULL;
    }

    /* Set the function pointers */
    device->VideoInit = CACA_VideoInit;
    device->VideoQuit = CACA_VideoQuit;
    device->PumpEvents = CACA_PumpEvents;

    device->CreateSDLWindow = CACA_CreateWindow;
    device->DestroyWindow = CACA_DestroyWindow;
    device->SetWindowTitle = CACA_SetWindowTitle;

    device->CreateWindowFramebuffer = CACA_CreateWindowFramebuffer;
    device->UpdateWindowFramebuffer = CACA_UpdateWindowFramebuffer;
    device->DestroyWindowFramebuffer = CACA_DestroyWindowFramebuffer;

    device->free = CACA_DeleteDevice;

    return device;
}

VideoBootStrap CACA_bootstrap = {
    "caca", "Color ASCII Art Library",
    CACA_CreateDevice
};

int CACA_VideoInit(SDL_VideoDevice *_this)
{
    SDL_DisplayMode mode;

    /* Use a fake 16-bpp desktop mode */
    SDL_zero(mode);
    mode.format = SDL_PIXELFORMAT_RGB565;
    mode.w = 80;
    mode.h = 32;
    if (SDL_AddBasicVideoDisplay(&mode) == 0) {
        return -1;
    }

    /* We're done! */
    return 0;
}

void CACA_VideoQuit(SDL_VideoDevice *_this)
{
}

#endif /* SDL_VIDEO_DRIVER_CACA */
