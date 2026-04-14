/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

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

#include "../../SDL_properties_c.h"
#include "../../events/SDL_mouse_c.h"
#include "../SDL_sysvideo.h"
#include "SDL_dosframebuffer_c.h"
#include "SDL_dosmouse.h"
#include "SDL_dosvideo.h"

#include <pc.h>           // for inportb, outportb
#include <sys/movedata.h> // for dosmemput (banked framebuffer writes)

// VGA DAC (Digital-to-Analog Converter) ports for palette programming
#define VGA_DAC_WRITE_INDEX 0x3C8 // write index register (set starting color index)
#define VGA_DAC_DATA        0x3C9 // data register (write R, G, B in sequence)

// VGA Input Status Register 1 (for vblank detection)
#define VGA_STATUS_PORT   0x3DA
#define VGA_STATUS_VBLANK 0x08 // bit 3: vertical retrace active

// note that DOS_SURFACE's value is the same string that the dummy driver uses.
#define DOS_SURFACE     "SDL.internal.window.surface"
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

    SDL_Surface *lfb_surface = NULL;

    if (!data->banked_mode) {
        // LFB path: we make a surface that uses video memory directly, so SDL can optimize the blit for us.
        // Point LFB surface at the back page (not currently displayed) for tear-free double-buffering.
        int back_page = data->page_flip_available ? (1 - data->current_page) : 0;
        void *lfb_pixels = (Uint8 *)DOS_PhysicalToLinear(data->mapping.address) + data->page_offset[back_page];
        lfb_surface = SDL_CreateSurfaceFrom(mode->w, mode->h, surface_format, lfb_pixels, mode->internal->pitch);
        if (!lfb_surface) {
            SDL_DestroySurface(surface);
            return false;
        }
    }
    // Banked path: no LFB surface — we'll copy from the system RAM surface
    // directly to the banked window in UpdateWindowFramebuffer.

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
            if (lfb_surface) {
                SDL_SetSurfacePalette(lfb_surface, palette);
            }
        }
        data->palette_version = 0; // force DAC update on first present

        // Also program the VGA DAC to all-black right now, so no flash
        // of stale/white palette colors before the first present.
        outportb(VGA_DAC_WRITE_INDEX, 0);
        for (int i = 0; i < 256; i++) {
            outportb(VGA_DAC_DATA, 0);
            outportb(VGA_DAC_DATA, 0);
            outportb(VGA_DAC_DATA, 0);
        }
    }

    // clear the framebuffer completely, in case another window at a larger size was using this before us.
    if (lfb_surface) {
        SDL_ClearSurface(lfb_surface, 0.0f, 0.0f, 0.0f, 0.0f);
    }
    // (For banked mode, the framebuffer was already zeroed in DOSVESA_SetDisplayMode.)

    // Save the info and return!
    SDL_SetSurfaceProperty(SDL_GetWindowProperties(window), DOS_SURFACE, surface);
    if (lfb_surface) {
        SDL_SetSurfaceProperty(SDL_GetWindowProperties(window), DOS_LFB_SURFACE, lfb_surface);
    }

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

// Bank-switched copy of a rectangular region from a system RAM surface to the
// VGA banked window.  `src_rect` is in source-surface coordinates; `dst_x` and
// `dst_y` give the top-left of the *surface's* position on screen (the
// centering offset).  The routine handles bank boundaries correctly even when a
// scanline spans two banks.
//
// `current_bank` is an in/out parameter so that consecutive calls (one per dirty
// rect) can avoid redundant bank switches when rects happen to fall in the same
// bank.  Initialise it to -1 before the first call.
static void BankedFramebufferCopyRect(const SDL_DisplayModeData *mdata,
                                      const SDL_Surface *src,
                                      const SDL_Rect *src_rect,
                                      int dst_x, int dst_y,
                                      Uint32 win_gran_bytes,
                                      Uint32 win_size_bytes,
                                      Uint32 win_base,
                                      int *current_bank,
                                      Uint32 win_func_ptr)
{
    const Uint16 dst_pitch = mdata->pitch;
    const int bytes_per_pixel = SDL_BYTESPERPIXEL(src->format);
    const int row_bytes = src_rect->w * bytes_per_pixel;

    // Fast path: if the source row width matches src pitch AND the destination
    // row width matches dst pitch, the data is contiguous in both source and
    // destination — we can copy it as one flat block, minimizing dosmemput calls.
    if (row_bytes == src->pitch && row_bytes == dst_pitch) {
        const Uint8 *src_data = (const Uint8 *)src->pixels + src_rect->y * src->pitch + src_rect->x * bytes_per_pixel;
        Uint32 dst_offset = (Uint32)(dst_y + src_rect->y) * dst_pitch + (Uint32)(dst_x + src_rect->x) * bytes_per_pixel;
        int total_bytes = row_bytes * src_rect->h;
        int src_off = 0;

        while (total_bytes > 0) {
            int bank = (int)(dst_offset / win_gran_bytes);
            Uint32 off_in_win = dst_offset % win_gran_bytes;
            Uint32 avail = win_size_bytes - off_in_win;
            int n = total_bytes;
            if ((Uint32)n > avail) {
                n = (int)avail;
            }

            if (bank != *current_bank) {
                __dpmi_regs regs;
                SDL_zero(regs);
                regs.x.bx = 0;
                regs.x.dx = (Uint16)bank;
                if (win_func_ptr) {
                    // Call WinFuncPtr directly — faster than INT 10h.
                    // CS:IP = segment:offset of the real-mode bank-switch function.
                    regs.x.cs = (Uint16)(win_func_ptr >> 16);
                    regs.x.ip = (Uint16)(win_func_ptr & 0xFFFF);
                    __dpmi_simulate_real_mode_procedure_retf(&regs);
                } else {
                    // Fallback: use VBE INT 10h function 0x4F05.
                    regs.x.ax = 0x4F05;
                    __dpmi_int(0x10, &regs);
                }
                *current_bank = bank;
            }

            dosmemput(src_data + src_off, n, win_base + off_in_win);
            src_off += n;
            dst_offset += n;
            total_bytes -= n;
        }
        return;
    }

    for (int y = 0; y < src_rect->h; y++) {
        Uint32 dst_offset = (Uint32)(dst_y + src_rect->y + y) * dst_pitch + (Uint32)(dst_x + src_rect->x) * bytes_per_pixel;
        const Uint8 *src_row = (const Uint8 *)src->pixels + (src_rect->y + y) * src->pitch + src_rect->x * bytes_per_pixel;
        int bytes_remaining = row_bytes;
        int src_off = 0;

        while (bytes_remaining > 0) {
            int bank = (int)(dst_offset / win_gran_bytes);
            Uint32 off_in_win = dst_offset % win_gran_bytes;

            // How many bytes can we write before hitting the end of this
            // bank's addressable window?
            Uint32 avail = win_size_bytes - off_in_win;
            int n = bytes_remaining;
            if ((Uint32)n > avail) {
                n = (int)avail;
            }

            if (bank != *current_bank) {
                __dpmi_regs regs;
                SDL_zero(regs);
                regs.x.bx = 0; // Window A
                regs.x.dx = (Uint16)bank;
                if (win_func_ptr) {
                    // Call WinFuncPtr directly — faster than INT 10h.
                    // CS:IP = segment:offset of the real-mode bank-switch function.
                    regs.x.cs = (Uint16)(win_func_ptr >> 16);
                    regs.x.ip = (Uint16)(win_func_ptr & 0xFFFF);
                    __dpmi_simulate_real_mode_procedure_retf(&regs);
                } else {
                    // Fallback: use VBE INT 10h function 0x4F05.
                    regs.x.ax = 0x4F05;
                    __dpmi_int(0x10, &regs);
                }
                *current_bank = bank;
            }

            dosmemput(src_row + src_off, n, win_base + off_in_win);

            src_off += n;
            dst_offset += n;
            bytes_remaining -= n;
        }
    }
}

static void WaitForVBlank(void)
{
    while (inportb(VGA_STATUS_PORT) & VGA_STATUS_VBLANK) {
        SDL_CPUPauseInstruction();
    }
    while (!(inportb(VGA_STATUS_PORT) & VGA_STATUS_VBLANK)) {
        SDL_CPUPauseInstruction();
    }
}

static void ProgramVGADAC(SDL_Palette *palette)
{
    outportb(VGA_DAC_WRITE_INDEX, 0);
    for (int i = 0; i < palette->ncolors && i < 256; i++) {
        outportb(VGA_DAC_DATA, palette->colors[i].r >> 2);
        outportb(VGA_DAC_DATA, palette->colors[i].g >> 2);
        outportb(VGA_DAC_DATA, palette->colors[i].b >> 2);
    }
}

bool DOSVESA_UpdateWindowFramebuffer(SDL_VideoDevice *device, SDL_Window *window, const SDL_Rect *rects, int numrects)
{
    SDL_VideoData *vdata = device->internal;
    SDL_WindowData *windata = window->internal;
    SDL_Surface *src = (SDL_Surface *)SDL_GetPointerProperty(SDL_GetWindowProperties(window), DOS_SURFACE, NULL);
    if (!src) {
        return SDL_SetError("Couldn't find DOS surface for window");
    }

    const SDL_DisplayModeData *mdata = vdata->current_mode.internal;

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

            if (!vdata->banked_mode) {
                // Also update the LFB surface palette for correct blitting
                SDL_Surface *dst = (SDL_Surface *)SDL_GetPointerProperty(SDL_GetWindowProperties(window), DOS_LFB_SURFACE, NULL);
                if (dst) {
                    SDL_Palette *dst_palette = SDL_GetSurfacePalette(dst);
                    if (dst_palette && dst_palette != src_palette) {
                        SDL_SetPaletteColors(dst_palette, win_palette->colors, 0,
                                             SDL_min(win_palette->ncolors, dst_palette->ncolors));
                    }
                }
            }
        }

        // Determine whether the VGA DAC needs reprogramming.
        dac_palette = win_palette ? win_palette : src_palette;
        if (dac_palette && dac_palette->version != vdata->palette_version) {
            dac_needs_update = true;
        }
    }

    if (vdata->banked_mode) {
        // --- Banked framebuffer path (dirty-rect aware) ---
        // We composite the cursor onto the source surface (in system RAM),
        // then bank-copy only the dirty rectangles to the VGA window.  We
        // need to undo the cursor composite afterwards so the app's surface
        // isn't permanently modified.

        const int dst_x = ((int)mdata->w - src->w) / 2;
        const int dst_y = ((int)mdata->h - src->h) / 2;

        SDL_Mouse *mouse = SDL_GetMouse();
        SDL_Surface *cursor = NULL;
        SDL_Rect cursorrect;
        SDL_Rect cursor_clipped; // cursorrect clipped to src bounds
        SDL_Surface *cursor_save = NULL;
        bool have_cursor_rect = false;

        SDL_Cursor *cur = mouse ? mouse->cur_cursor : NULL;
        if (cur && cur->animation) {
            cur = cur->animation->frames[cur->animation->current_frame];
        }
        if (mouse && mouse->internal && !mouse->relative_mode && mouse->cursor_visible && cur && cur->internal) {
            cursor = cur->internal->surface;
            if (cursor) {
                cursorrect.x = SDL_clamp((int)mouse->x, 0, window->w) - cur->internal->hot_x;
                cursorrect.y = SDL_clamp((int)mouse->y, 0, window->h) - cur->internal->hot_y;
                cursorrect.w = cursor->w;
                cursorrect.h = cursor->h;

                // Clip cursor rect to src bounds for save/restore.
                cursor_clipped = cursorrect;
                if (cursor_clipped.x < 0) {
                    cursor_clipped.w += cursor_clipped.x;
                    cursor_clipped.x = 0;
                }
                if (cursor_clipped.y < 0) {
                    cursor_clipped.h += cursor_clipped.y;
                    cursor_clipped.y = 0;
                }
                if (cursor_clipped.x + cursor_clipped.w > src->w) {
                    cursor_clipped.w = src->w - cursor_clipped.x;
                }
                if (cursor_clipped.y + cursor_clipped.h > src->h) {
                    cursor_clipped.h = src->h - cursor_clipped.y;
                }

                if (cursor_clipped.w > 0 && cursor_clipped.h > 0) {
                    have_cursor_rect = true;

                    // Save the pixels under the cursor so we can restore them after the copy.
                    cursor_save = SDL_CreateSurface(cursor_clipped.w, cursor_clipped.h, src->format);
                    if (cursor_save) {
                        if (src->format == SDL_PIXELFORMAT_INDEX8) {
                            SDL_Palette *sp = SDL_GetSurfacePalette(src);
                            if (sp) {
                                SDL_SetSurfacePalette(cursor_save, sp);
                            }
                        }
                        SDL_BlitSurface(src, &cursor_clipped, cursor_save, NULL);
                    }
                }

                // Composite cursor onto the source surface.
                SDL_BlitSurface(cursor, NULL, src, &cursorrect);
            }
        }

        // Wait for vsync before the copy to reduce tearing.
        const int vsync_interval = windata->framebuffer_vsync;
        if (vsync_interval > 0 || dac_needs_update) {
            WaitForVBlank();
        }

        if (dac_needs_update) {
            vdata->palette_version = dac_palette->version;
            ProgramVGADAC(dac_palette);
        }

        // Bank-switched copy of only the dirty rectangles.
        // Pre-compute constants shared across all rect copies.
        const Uint32 win_gran_bytes = (Uint32)mdata->win_granularity * 1024;
        const Uint32 win_size_bytes = (Uint32)mdata->win_size * 1024;
        const Uint32 win_base = (Uint32)mdata->win_a_segment << 4;
        int current_bank = -1;

        // Track whether the cursor region was already covered by a dirty rect
        // so we don't copy it twice.
        bool cursor_covered = false;

        for (int r = 0; r < numrects; r++) {
            // Clip the dirty rect to the source surface bounds.
            SDL_Rect rect = rects[r];
            if (rect.x < 0) {
                rect.w += rect.x;
                rect.x = 0;
            }
            if (rect.y < 0) {
                rect.h += rect.y;
                rect.y = 0;
            }
            if (rect.x + rect.w > src->w) {
                rect.w = src->w - rect.x;
            }
            if (rect.y + rect.h > src->h) {
                rect.h = src->h - rect.y;
            }
            if (rect.w <= 0 || rect.h <= 0) {
                continue;
            }

            // If the cursor is visible, check whether this rect fully covers it.
            if (have_cursor_rect && !cursor_covered) {
                if (rect.x <= cursor_clipped.x &&
                    rect.y <= cursor_clipped.y &&
                    rect.x + rect.w >= cursor_clipped.x + cursor_clipped.w &&
                    rect.y + rect.h >= cursor_clipped.y + cursor_clipped.h) {
                    cursor_covered = true;
                }
            }

            BankedFramebufferCopyRect(mdata, src, &rect, dst_x, dst_y,
                                      win_gran_bytes, win_size_bytes, win_base,
                                      &current_bank, mdata->win_func_ptr);
        }

        // If no dirty rect covered the cursor, copy the cursor region separately.
        if (have_cursor_rect && !cursor_covered) {
            BankedFramebufferCopyRect(mdata, src, &cursor_clipped, dst_x, dst_y,
                                      win_gran_bytes, win_size_bytes, win_base,
                                      &current_bank, mdata->win_func_ptr);
        }

        // Restore the source surface pixels under the cursor.
        if (cursor_save) {
            SDL_Rect restore_rect = cursor_clipped;
            SDL_BlitSurface(cursor_save, NULL, src, &restore_rect);
            SDL_DestroySurface(cursor_save);
        }

    } else {
        // --- LFB path (original code) ---
        SDL_Surface *dst = (SDL_Surface *)SDL_GetPointerProperty(SDL_GetWindowProperties(window), DOS_LFB_SURFACE, NULL);
        if (!dst) {
            return SDL_SetError("Couldn't find VESA linear framebuffer surface for window");
        }

        const SDL_Rect dstrect = { (dst->w - src->w) / 2, (dst->h - src->h) / 2, src->w, src->h };
        SDL_Mouse *mouse = SDL_GetMouse();
        SDL_Surface *cursor = NULL;
        SDL_Rect cursorrect;

        SDL_Cursor *cur = mouse ? mouse->cur_cursor : NULL;
        if (cur && cur->animation) {
            cur = cur->animation->frames[cur->animation->current_frame];
        }
        if (mouse && mouse->internal && !mouse->relative_mode && mouse->cursor_visible && cur && cur->internal) {
            cursor = cur->internal->surface;
            if (cursor) {
                cursorrect.x = dstrect.x + SDL_clamp((int)mouse->x, 0, window->w) - cur->internal->hot_x;
                cursorrect.y = dstrect.y + SDL_clamp((int)mouse->y, 0, window->h) - cur->internal->hot_y;
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
            // Page-flip with optional vsync.
            const int vsync_interval = windata->framebuffer_vsync;
            int back_page = 1 - vdata->current_page;
            Uint16 first_scanline = (Uint16)(vdata->page_offset[back_page] / mdata->pitch);

            if (vsync_interval > 0 || dac_needs_update) {
                // Wait for vblank so the flip and DAC update appear together.
                WaitForVBlank();
            }

            if (dac_needs_update) {
                vdata->palette_version = dac_palette->version;
                ProgramVGADAC(dac_palette);
            }

            // Flip: make the back page (which we just drew to) the visible page.
            // Always use subfunction 0x0080 (set display start, don't wait) —
            // vsync is controlled by our manual vblank wait above.
            __dpmi_regs regs;
            SDL_zero(regs);
            regs.x.ax = 0x4F07;
            regs.x.bx = 0x0080;
            regs.x.cx = 0; // first pixel in scan line
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
                WaitForVBlank();
            }

            if (dac_needs_update) {
                vdata->palette_version = dac_palette->version;
                ProgramVGADAC(dac_palette);
            }
        }
    }

    return true;
}

void DOSVESA_DestroyWindowFramebuffer(SDL_VideoDevice *device, SDL_Window *window)
{
    SDL_VideoData *data = device->internal;
    if (!data->banked_mode) {
        SDL_Surface *lfb_surface = (SDL_Surface *)SDL_GetPointerProperty(SDL_GetWindowProperties(window), DOS_LFB_SURFACE, NULL);
        if (lfb_surface && data->mapping.size) {
            SDL_ClearSurface(lfb_surface, 0.0f, 0.0f, 0.0f, 0.0f);
        }
    }
    SDL_ClearProperty(SDL_GetWindowProperties(window), DOS_SURFACE);
    SDL_ClearProperty(SDL_GetWindowProperties(window), DOS_LFB_SURFACE);
}

#endif // SDL_VIDEO_DRIVER_DOSVESA