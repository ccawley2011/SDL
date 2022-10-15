/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2024 Sam Lantinga <slouken@libsdl.org>

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
#include "../../SDL_internal.h"

#ifdef SDL_VIDEO_DRIVER_RISCOS

#include "SDL.h"
#include "SDL_version.h"
#include "SDL_syswm.h"
#include "../SDL_sysvideo.h"
#include "../../events/SDL_events_c.h"
#include "../../events/SDL_mouse_c.h"
#include "../../events/SDL_windowevents_c.h"

#include "SDL_riscosframebuffer_c.h"
#include "SDL_riscosvideo.h"
#include "SDL_riscoswindow.h"

#include <kernel.h>
#include <swis.h>

#if SDL_THREAD_PTHREAD
#include <pthread.h>
#endif

int RISCOS_InitWimp(_THIS)
{
    SDL_VideoData *driverdata = (SDL_VideoData *)_this->driverdata;
    const char *task_name = "SDL Application";
    _kernel_oserror *error;
    _kernel_swi_regs regs;
    int messages[2];

    messages[0] = 8; /* Pre quit */
    messages[1] = 0;

    regs.r[0] = 350;
    regs.r[1] = 0x4b534154;
    regs.r[2] = (int)task_name;
    regs.r[3] = (int)messages;

    error = _kernel_swi(Wimp_Initialise, &regs, &regs);
    if (error) {
        return SDL_SetError("Unable to initialise the Wimp: %s (%i)", error->errmess, error->errnum);
    }

    driverdata->wimp_version = regs.r[0];
    driverdata->task_handle = regs.r[1];

    return 0;
}

void RISCOS_QuitWimp(_THIS)
{
    SDL_VideoData *driverdata = (SDL_VideoData *)_this->driverdata;
    _kernel_swi_regs regs;

    regs.r[0] = driverdata->task_handle;
    regs.r[1] = 0x4b534154;
    _kernel_swi(Wimp_CloseDown, &regs, &regs);
}

static SDL_Window *RISCOS_GetWindowFromHandle(_THIS, int handle)
{
    SDL_Window *window;

    if (_this) {
        for (window = _this->windows; window; window = window->next) {
            SDL_WindowData *data = (SDL_WindowData *)window->driverdata;
            if (data && data->handle == handle) {
                return window;
            }
        }
    }
    return NULL;
}

void RISCOS_PollWimp(_THIS)
{
    int message[64];
    SDL_Window *window;
    _kernel_swi_regs regs;
    SDL_bool doPoll = SDL_TRUE;

    while (doPoll)
    {
        regs.r[0] = 0;
        regs.r[1] = (int)message;
        _kernel_swi(Wimp_Poll, &regs, &regs);

        switch (regs.r[0]) {
        case 0: /* Null event */
            doPoll = SDL_FALSE;
            break;

        case 1: /* Redraw window */
            window = RISCOS_GetWindowFromHandle(_this, message[0]);
            if (window) {
                RISCOS_RedrawWindowFramebuffer(_this, window, (wimp_redraw *)message);
            }
            break;

        case 2: /* Open window */
            /* TODO: Investigate closer? */
            _kernel_swi(Wimp_OpenWindow, &regs, &regs);
            break;

        case 3: /* Close window */
            window = RISCOS_GetWindowFromHandle(_this, message[0]);
            if (window) {
                SDL_SendWindowEvent(window, SDL_WINDOWEVENT_CLOSE, 0, 0);
            }
            break;

        case 17:
        case 18:
            switch (message[4]) {
            case 0: /* Quit */
                SDL_SendAppEvent(SDL_APP_TERMINATING);
                SDL_Quit();
                exit(0);
                break;

            case 8: /* Pre Quit */
                SDL_SendQuit();
                break;
            }
            break;

        default:
            break;
        }

#if SDL_THREAD_PTHREAD
        pthread_yield();
#endif

        /* TODO: Handle SysWM events */
    }
}

int RISCOS_CreateWindow(_THIS, SDL_Window *window)
{
    SDL_WindowData *driverdata;
    _kernel_oserror *error;
    _kernel_swi_regs regs;
    int window_block[22];

    driverdata = (SDL_WindowData *)SDL_calloc(1, sizeof(*driverdata));
    if (!driverdata) {
        return SDL_OutOfMemory();
    }
    driverdata->window = window;

    window_block[0] = 0;
    window_block[1] = 0;
    window_block[2] = window_block[0] + (window->w << 1);
    window_block[3] = window_block[1] + (window->h << 1);
    window_block[4] = 0;
    window_block[5] = 0;
    window_block[6] = -1;
    window_block[7] = 0x87040242;
    window_block[8] = 0xff070207;
    window_block[9] = 0x020c0103;
    window_block[10] = 0;
    window_block[11] = -(window->h << 1);
    window_block[12] =   window->w << 1;
    window_block[13] = 0;
    window_block[14] = 0x2700013d;
    window_block[15] = 0x00003000;
    window_block[16] = 1;
    window_block[17] = 0x00100010;
    window_block[18] = (int)driverdata->title;
    window_block[19] = -1;
    window_block[20] = 256;
    window_block[21] = 0;

    regs.r[1] = (unsigned int)(window_block);
    error = _kernel_swi(Wimp_CreateWindow, &regs, &regs);
    if (error != NULL) {
        free(driverdata);
        return SDL_SetError("Unable to create window: %s (%i)", error->errmess, error->errnum);
    }
    driverdata->handle = regs.r[0];

    /* All done! */
    window->driverdata = driverdata;
    return 0;
}

void RISCOS_DestroyWindow(_THIS, SDL_Window *window)
{
    SDL_WindowData *driverdata = (SDL_WindowData *)window->driverdata;
    _kernel_swi_regs regs;

    if (!driverdata) {
        return;
    }

    regs.r[1] = (int)&driverdata->handle;
    _kernel_swi(Wimp_DeleteWindow, &regs, &regs);

    SDL_free(driverdata);
    window->driverdata = NULL;
}

void RISCOS_SetWindowTitle(_THIS, SDL_Window *window)
{
    SDL_WindowData *driverdata = (SDL_WindowData *)window->driverdata;
    _kernel_swi_regs regs;

    /* TODO: Handle conversion from UTF-8 */
    SDL_strlcpy(driverdata->title, window->title, SDL_arraysize(driverdata->title));

    /* TODO: Redraw the title bar */
}

void RISCOS_ShowWindow(_THIS, SDL_Window *window)
{
    SDL_WindowData *driverdata = (SDL_WindowData *)window->driverdata;
    _kernel_swi_regs regs;
    wimp_open open;

    open.w = driverdata->handle;
    open.box.x0 = 0; /* TODO: Open this in the correct place */
    open.box.y0 = 0;
    open.box.x1 = open.box.x0 + (window->w << 1);
    open.box.y1 = open.box.y0 + (window->h << 1);
    open.xscroll = 0;
    open.yscroll = 0;
    open.next = -1;

    regs.r[1] = (int)&open;
    _kernel_swi(Wimp_OpenWindow, &regs, &regs);
}

void RISCOS_HideWindow(_THIS, SDL_Window *window)
{
    SDL_WindowData *driverdata = (SDL_WindowData *)window->driverdata;
    _kernel_swi_regs regs;

    regs.r[1] = (int)&driverdata->handle;
    _kernel_swi(Wimp_CloseWindow, &regs, &regs);
}

SDL_bool RISCOS_GetWindowWMInfo(_THIS, SDL_Window *window, struct SDL_SysWMinfo *info)
{
    if (info->version.major == SDL_MAJOR_VERSION) {
        info->subsystem = SDL_SYSWM_RISCOS;
        return SDL_TRUE;
    } else {
        SDL_SetError("Application not compiled with SDL %d",
                     SDL_MAJOR_VERSION);
        return SDL_FALSE;
    }
}

#endif /* SDL_VIDEO_DRIVER_RISCOS */

/* vi: set ts=4 sw=4 expandtab: */
