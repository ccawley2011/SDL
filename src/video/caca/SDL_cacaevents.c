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

#include "../../events/SDL_events_c.h"
#include "../../events/SDL_mouse_c.h"

#include "SDL_cacawindow.h"
#include "SDL_cacaevents_c.h"

static void CACA_HandleMouseButton(SDL_Window *window, enum caca_event_type type, int button)
{
    Uint8 pressed = (type == CACA_EVENT_MOUSE_PRESS);
    int wheelX = 0, wheelY = 0, mapped = 0;

    switch (button) {
    case 1:
        mapped = SDL_BUTTON_LEFT;
        break;
    case 2:
        mapped = SDL_BUTTON_MIDDLE;
        break;
    case 3:
        mapped = SDL_BUTTON_RIGHT;
        break;
    case 4:
        wheelY = 1;
        break;
    case 5:
        wheelY = -1;
        break;
    case 6:
        wheelX = -1;
        break;
    case 7:
        wheelX = 1;
        break;
    default:
        mapped = button - (8 - SDL_BUTTON_X1);
        break;
    }

    if (wheelX || wheelY)
        SDL_SendMouseWheel(0, window, 0, wheelX, wheelY, SDL_MOUSEWHEEL_NORMAL);
    else if (mapped)
        SDL_SendMouseButton(0, window, 0, pressed, mapped);
}

void CACA_PumpEvents(SDL_VideoDevice *_this)
{
    SDL_Window *window = _this->windows;
    caca_event_t ev;

    while (window) {
        if (CACA_caca_get_event(window->driverdata->dp, CACA_EVENT_ANY, &ev, 0)) {
            switch (CACA_caca_get_event_type(&ev)) {
            case CACA_EVENT_QUIT:
                SDL_SendWindowEvent(window, SDL_EVENT_WINDOW_CLOSE_REQUESTED, 0, 0);
                break;
            case CACA_EVENT_MOUSE_MOTION:
		SDL_SendMouseMotion(0, window, 0, 0,
                                    CACA_caca_get_event_mouse_x(&ev),
                                    CACA_caca_get_event_mouse_y(&ev));
                break;
            case CACA_EVENT_MOUSE_PRESS:
            case CACA_EVENT_MOUSE_RELEASE:
                CACA_HandleMouseButton(window,
                                       CACA_caca_get_event_type(&ev),
                                       CACA_caca_get_event_mouse_button(&ev));
                break;
            default:
                break;
            }
        }

        window = window->next;
    }
}

#endif /* SDL_VIDEO_DRIVER_CACA */
