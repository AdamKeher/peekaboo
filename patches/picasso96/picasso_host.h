/*
 * PEEKaboo - libretro host side of Picasso96 / uaegfx RTG.
 *
 * WinUAE's picasso96_win.cpp talks to od-win32 through win32gfx.h, direct3d.h and
 * registry.h. None of those exist here, so this header declares the same surface
 * and retrodep/picasso_host.c implements it against the libretro frame buffer.
 */
#ifndef PEEKABOO_PICASSO_HOST_H
#define PEEKABOO_PICASSO_HOST_H

#ifdef PICASSO96

#include "uae/types.h"
#include "rtgmodes.h"

struct AmigaMonitor;

/* --- win32gfx.h ---------------------------------------------------------- */
#define RTG_MODE_SCALE 1
#define RTG_MODE_CENTER 2
#define RTG_MODE_INTEGER_SCALE 3

int WIN32GFX_GetWidth(struct AmigaMonitor *mon);
int WIN32GFX_GetHeight(struct AmigaMonitor *mon);
void DX_Invalidate(struct AmigaMonitor *mon, int x, int y, int width, int height);

/* The refresh rate a mode's PixelClock is computed from when the RTG apmode
 * carries none of its own. */
extern int default_freq;

/* WinUAE serialises the render thread against the message loop with these.
 * The libretro core renders on the thread that calls retro_run(), and
 * rtg_multithread is forced off, so both are no-ops here. */
void gfx_lock(void);
void gfx_unlock(void);

/* --- direct3d.h ---------------------------------------------------------- */
#define CURSORMAXWIDTH 64
#define CURSORMAXHEIGHT 64

/* Function pointers in WinUAE, tested for NULL before every call. Kept as
 * pointers so picasso96.c's `if (D3D_setcursor)` guards do the right thing:
 * this core has no hardware cursor and no device to restore, so all three stay
 * NULL and every hardware-sprite / restore path drops out at run time rather
 * than being patched away. P96 then draws the pointer into the frame buffer
 * itself, which is what we want - Peekaboo's window has its own cursor. */
extern bool (*D3D_setcursor)(int monid, int x, int y, int width, int height,
                             float mx, float my, bool visible, bool noscale);
extern uae_u8 *(*D3D_setcursorsurface)(int monid, bool query, int *pitch);
extern void (*D3D_restore)(int monid, bool);

/* --- host frame buffer --------------------------------------------------- */
/* Build the host mode list Displays[0].DisplayModes hands to picasso96_alloc().
 * Called from enumeratedisplays(). */
void picasso_host_enumerate_modes(void);

/* True while the core is presenting an RTG frame rather than the chipset one. */
int picasso_host_active(void);

/* Geometry of the RTG frame currently being presented; 0 when there is none. */
int picasso_host_width(void);
int picasso_host_height(void);

/* One-shot: true once after the RTG mode changed or the display switched, so
 * libretro-core.c knows to tell the front-end. */
int picasso_host_take_geometry_change(void);

#endif /* PICASSO96 */

#endif /* PEEKABOO_PICASSO_HOST_H */
