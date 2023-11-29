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

/* *INDENT-OFF* */ /* clang-format off */

#ifndef SDL_CACA_MODULE
#define SDL_CACA_MODULE(modname)
#endif

#ifndef SDL_CACA_SYM
#define SDL_CACA_SYM(rc,fn,params)
#endif

#ifndef SDL_CACA_SYM_CONST
#define SDL_CACA_SYM_CONST(type, name)
#endif


SDL_CACA_MODULE(CACA)
SDL_CACA_SYM(caca_canvas_t *,caca_create_canvas,(int, int))
SDL_CACA_SYM(int,caca_free_canvas,(caca_canvas_t *))

SDL_CACA_SYM(caca_dither_t *,caca_create_dither,(int, int, int, int, uint32_t, uint32_t, uint32_t, uint32_t))
SDL_CACA_SYM(int,caca_dither_bitmap,(caca_canvas_t *, int, int, int, int, caca_dither_t const *, void const *))
SDL_CACA_SYM(int,caca_free_dither,(caca_dither_t *))

SDL_CACA_SYM(caca_display_t *,caca_create_display,(caca_canvas_t *))
SDL_CACA_SYM(int,caca_free_display,(caca_display_t *))
SDL_CACA_SYM(int,caca_refresh_display,(caca_display_t *))
SDL_CACA_SYM(int,caca_set_display_title,(caca_display_t *, char const *))

SDL_CACA_SYM(int,caca_get_event,(caca_display_t *, int, caca_event_t *, int))
SDL_CACA_SYM(enum caca_event_type,caca_get_event_type,(caca_event_t const *))
SDL_CACA_SYM(int,caca_get_event_mouse_button,(caca_event_t const *))
SDL_CACA_SYM(int,caca_get_event_mouse_x,(caca_event_t const *))
SDL_CACA_SYM(int,caca_get_event_mouse_y,(caca_event_t const *))

#undef SDL_CACA_MODULE
#undef SDL_CACA_SYM
#undef SDL_CACA_SYM_CONST

/* *INDENT-ON* */ /* clang-format on */
