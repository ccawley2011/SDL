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

#ifdef SDL_VIDEO_DRIVER_DOSVESA

#include "../SDL_sysvideo.h"
#include "SDL_dosvideo.h"
#include "SDL_dosframebuffer_c.h"
#include "SDL_dosmouse.h"
#include "../../events/SDL_mouse_c.h"
#include "../../SDL_properties_c.h"

#include <pc.h>  // for inportb, outportb

// note that DOS_SURFACE's value is the same string that the dummy driver uses.
#define DOS_SURFACE "SDL.internal.window.surface"
#define DOS_LFB_SURFACE "SDL.internal.window.lfb_surface"

bool DOSVESA_CreateWindowFramebuffer(SDL_VideoDevice *device, SDL_Window *window, SDL_PixelFormat *format, void **pixels, int *pitch)
{
    SDL_VideoData *data = device->internal;
    const SDL_DisplayMode *mode = &data->current_mode;
    const SDL_PixelFormat surface_format = mode->format;
    int w, h;

    // writing to video RAM shows up as the screen refreshes, done or not, and it might have a weird pitch, so give the app a buffer of system RAM.
    SDL_GetWindowSizeInPixels(window, &w, &h);
    SDL_Surface *surface = SDL_CreateSurface(w, h, surface_format);
    if (!surface) {
        return false;
    }

    // we make a surface that uses video memory directly, so SDL can optimize the blit for us.
    // Point LFB surface at the back page (not currently displayed) for tear-free double-buffering.
    int back_page = data->page_flip_available ? (1 - data->current_page) : 0;
    void *lfb_pixels = (Uint8 *)DOS_PhysicalToLinear(data->mapping.address) + data->page_offset[back_page];
    SDL_Surface *lfb_surface = SDL_CreateSurfaceFrom(mode->w, mode->h, surface_format, lfb_pixels, mode->internal->pitch);
    if (!lfb_surface) {
        SDL_DestroySurface(surface);
        return false;
    }

    // For 8-bit indexed modes, both surfaces need palettes.
    // Share the same palette object so palette updates propagate to both.
    if (surface_format == SDL_PIXELFORMAT_INDEX8) {
        SDL_Palette *palette = SDL_CreateSurfacePalette(surface);
        if (palette) {
            // Initialize palette to all-black so that transitions start
            // from black instead of flashing uninitialized (white) colors.
            SDL_Color black[256];
            SDL_memset(black, 0, sizeof(black));
            for (int i = 0; i < 256; i++) {
                black[i].a = SDL_ALPHA_OPAQUE;
            }
            SDL_SetPaletteColors(palette, black, 0, 256);
            SDL_SetSurfacePalette(lfb_surface, palette);
        }
        data->palette_version = 0;  // force DAC update on first present

        // Also program the VGA DAC to all-black right now, so no flash
        // of stale/white palette colors before the first present.
        outportb(0x3C8, 0);
        for (int i = 0; i < 256; i++) {
            outportb(0x3C9, 0);
            outportb(0x3C9, 0);
            outportb(0x3C9, 0);
        }
    }

    // clear the framebuffer completely, in case another window at a larger size was using this before us.
    SDL_ClearSurface(lfb_surface, 0.0f, 0.0f, 0.0f, 0.0f);

    // Save the info and return!
    SDL_SetSurfaceProperty(SDL_GetWindowProperties(window), DOS_SURFACE, surface);
    SDL_SetSurfaceProperty(SDL_GetWindowProperties(window), DOS_LFB_SURFACE, lfb_surface);

    *format = surface_format;
    *pixels = surface->pixels;
    *pitch = surface->pitch;
    return true;
}

bool DOSVESA_SetWindowFramebufferVSync(SDL_VideoDevice *device, SDL_Window *window, int vsync)
{
    if (vsync < 0) {
        return SDL_SetError("Unsupported vsync type");
    }
    SDL_WindowData *data = window->internal;
    data->framebuffer_vsync = vsync;
    return true;
}

bool DOSVESA_GetWindowFramebufferVSync(SDL_VideoDevice *device, SDL_Window *window, int *vsync)
{
    if (vsync) {
        SDL_WindowData *data = window->internal;
        *vsync = data->framebuffer_vsync;
    }
    return true;
}

bool DOSVESA_UpdateWindowFramebuffer(SDL_VideoDevice *device, SDL_Window *window, const SDL_Rect *rects, int numrects)
{
    SDL_VideoData *vdata = device->internal;
    SDL_WindowData *windata = window->internal;
    SDL_Surface *src = (SDL_Surface *) SDL_GetPointerProperty(SDL_GetWindowProperties(window), DOS_SURFACE, NULL);
    if (!src) {
        return SDL_SetError("Couldn't find DOS surface for window");
    }

    SDL_Surface *dst = (SDL_Surface *) SDL_GetPointerProperty(SDL_GetWindowProperties(window), DOS_LFB_SURFACE, NULL);
    if (!dst) {
        return SDL_SetError("Couldn't find VESA linear framebuffer surface for window");
    }

    const SDL_Rect dstrect = { (dst->w - src->w) / 2, (dst->h - src->h) / 2, src->w, src->h };
    SDL_Mouse *mouse = SDL_GetMouse();
    SDL_Surface *cursor = NULL;
    SDL_Rect cursorrect;

    if (mouse && mouse->internal && !mouse->relative_mode && mouse->cursor_visible && mouse->cur_cursor && mouse->cur_cursor->internal) {
        cursor = mouse->cur_cursor->internal->surface;
        if (cursor) {
            cursorrect.x = dstrect.x + SDL_clamp((int) mouse->x, 0, window->w);
            cursorrect.y = dstrect.y + SDL_clamp((int) mouse->y, 0, window->h);
        }
    }

    // For 8-bit indexed modes, sync palette data between surfaces so the
    // blit uses the correct color mapping.  The actual VGA DAC programming
    // is deferred until vertical blanking to avoid visible palette flicker.
    SDL_Palette *dac_palette = NULL;
    bool dac_needs_update = false;

    if (src->format == SDL_PIXELFORMAT_INDEX8) {
        SDL_Palette *win_palette = window->surface ? SDL_GetSurfacePalette(window->surface) : NULL;
        SDL_Palette *src_palette = SDL_GetSurfacePalette(src);

        // Sync: if the app set colors on the window surface palette,
        // copy them to the internal surface so the blit works correctly.
        if (win_palette && src_palette && win_palette != src_palette &&
            win_palette->version != src_palette->version) {
            SDL_SetPaletteColors(src_palette, win_palette->colors, 0,
                                 SDL_min(win_palette->ncolors, src_palette->ncolors));
            // Also update the LFB surface palette for correct blitting
            SDL_Palette *dst_palette = SDL_GetSurfacePalette(dst);
            if (dst_palette && dst_palette != src_palette) {
                SDL_SetPaletteColors(dst_palette, win_palette->colors, 0,
                                     SDL_min(win_palette->ncolors, dst_palette->ncolors));
            }
        }

        // Determine whether the VGA DAC needs reprogramming.
        dac_palette = win_palette ? win_palette : src_palette;
        if (dac_palette && dac_palette->version != vdata->palette_version) {
            dac_needs_update = true;
        }
    }

    // Blit to the back page (or the only page, if no page-flipping)
    if (!SDL_BlitSurface(src, NULL, dst, &dstrect)) {
        return false;
    }

    if (cursor) {
        if (!SDL_BlitSurface(cursor, NULL, dst, &cursorrect)) {
            return false;
        }
    }

    if (vdata->page_flip_available) {
        // Program the VGA DAC during the vertical retrace that the page
        // flip waits for, so the new palette and new page appear together.
        const SDL_DisplayModeData *mdata = vdata->current_mode.internal;
        int back_page = 1 - vdata->current_page;
        Uint16 first_scanline = (Uint16)(vdata->page_offset[back_page] / mdata->pitch);

        // Wait for vblank start before touching the DAC or flipping.
        while (inportb(0x3DA) & 0x08) { SDL_CPUPauseInstruction(); }  // wait for non-vblank
        while (!(inportb(0x3DA) & 0x08)) { SDL_CPUPauseInstruction(); }  // wait for vblank

        if (dac_needs_update) {
            vdata->palette_version = dac_palette->version;
            outportb(0x3C8, 0);
            for (int i = 0; i < dac_palette->ncolors && i < 256; i++) {
                outportb(0x3C9, dac_palette->colors[i].r >> 2);
                outportb(0x3C9, dac_palette->colors[i].g >> 2);
                outportb(0x3C9, dac_palette->colors[i].b >> 2);
            }
        }

        // Flip: make the back page (which we just drew to) the visible page.
        // Use subfunction 0x80 (set display start, don't wait) since we
        // already waited for vblank above.
        __dpmi_regs regs;
        regs.x.ax = 0x4F07;
        regs.x.bx = 0x0080;  // set display start, no wait (we already synced)
        regs.x.cx = 0;       // first pixel in scan line
        regs.x.dx = first_scanline;
        __dpmi_int(0x10, &regs);

        vdata->current_page = back_page;

        // Update LFB surface to point at the new back page (the old front page)
        int new_back = 1 - vdata->current_page;
        dst->pixels = (Uint8 *)DOS_PhysicalToLinear(vdata->mapping.address) + vdata->page_offset[new_back];
    } else {
        // No page-flipping: wait for vsync, then update DAC atomically
        const int vsync_interval = windata->framebuffer_vsync;
        if (vsync_interval > 0 || dac_needs_update) {
            while (inportb(0x3DA) & 0x08) { SDL_CPUPauseInstruction(); }  // wait for non-vblank
            while (!(inportb(0x3DA) & 0x08)) { SDL_CPUPauseInstruction(); }  // wait for vblank
        }

        if (dac_needs_update) {
            vdata->palette_version = dac_palette->version;
            outportb(0x3C8, 0);
            for (int i = 0; i < dac_palette->ncolors && i < 256; i++) {
                outportb(0x3C9, dac_palette->colors[i].r >> 2);
                outportb(0x3C9, dac_palette->colors[i].g >> 2);
                outportb(0x3C9, dac_palette->colors[i].b >> 2);
            }
        }
    }

    return true;
}

void DOSVESA_DestroyWindowFramebuffer(SDL_VideoDevice *device, SDL_Window *window)
{
    SDL_VideoData *data = device->internal;
    SDL_Surface *lfb_surface = (SDL_Surface *) SDL_GetPointerProperty(SDL_GetWindowProperties(window), DOS_LFB_SURFACE, NULL);
    if (lfb_surface && data->mapping.size) {
        SDL_ClearSurface(lfb_surface, 0.0f, 0.0f, 0.0f, 0.0f);
    }
    SDL_ClearProperty(SDL_GetWindowProperties(window), DOS_SURFACE);
    SDL_ClearProperty(SDL_GetWindowProperties(window), DOS_LFB_SURFACE);
}

#endif // SDL_VIDEO_DRIVER_DOSVESA