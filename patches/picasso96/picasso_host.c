/*
 * PEEKaboo - libretro host side of Picasso96 / uaegfx RTG.
 *
 * WinUAE's picasso96_win.cpp renders the RTG screen into a Direct3D texture it
 * gets from win32gfx.cpp. There is no such texture here: a libretro core hands
 * the front-end one linear buffer a frame, which for this core is retro_bmp.
 * So the whole of that layer collapses into "give Picasso96 retro_bmp and tell
 * libretro how big the picture now is":
 *
 *   gfx_lock_picasso()        -> retro_bmp, with rowbytes = width * pixbytes
 *   gfx_set_picasso_modeinfo()-> record the mode; retro_run() picks the new
 *                                geometry up through retro_rtg_geometry()
 *   gfx_set_picasso_state()   -> chipset <-> RTG switch
 *
 * retro_bmp is the same buffer the chipset frame is drawn into, and only one of
 * the two is live at a time (adisplays[0].picasso_on), so they cannot collide.
 *
 * Only uaegfx is supported. The hardware boards in gfxboard.c need the PCem
 * device layer, which is not in this build - see the comment at the top of
 * sources/src/gfxboard.c.
 */

#include "sysconfig.h"
#include "sysdeps.h"

#ifdef PICASSO96

#include "options.h"
#include "uae.h"
#include "memory.h"
#include "custom.h"
#include "xwin.h"
#include "picasso96.h"
#include "picasso_host.h"
#include "libretro-core.h"

/* misc.c compiles against this when PICASSO96 is on: "handled by the graphics
 * drivers and set up in picasso96.c". We are the graphics driver. */
int screen_is_picasso = 0;

/* The refresh rate a mode's PixelClock is derived from when the RTG apmode has
 * none of its own. Picasso96 only ever multiplies it into a number it writes
 * into the ModeInfo, so nothing depends on it being exact. */
int default_freq = 60;

/* WinUAE's hardware cursor and device-restore hooks. NULL here on purpose: see
 * picasso_host.h. hwsprite then stays 0 and Picasso96 draws the pointer into
 * the frame buffer itself. */
bool (*D3D_setcursor)(int monid, int x, int y, int width, int height,
                      float mx, float my, bool visible, bool noscale) = NULL;
uae_u8 *(*D3D_setcursorsurface)(int monid, bool query, int *pitch) = NULL;
void (*D3D_restore)(int monid, bool) = NULL;

/* One thread renders here, so WinUAE's render-thread lock is a no-op. */
void gfx_lock(void) {}
void gfx_unlock(void) {}

/* ------------------------------------------------------------------------- */
/* Host pixel format                                                         */
/* ------------------------------------------------------------------------- */
/* Exactly what libretro-glue.c's graphics_setup() hands alloc_colors64k() for
 * the chipset frame, so an RTG pixel and a chipset pixel mean the same thing
 * to the front-end: RGB565 at 2 bytes, XRGB8888 at 4. */
static void host_rgb_layout(int *rb, int *gb, int *bb, int *rs, int *gs, int *bs, int *ab, int *as)
{
	if (pix_bytes == 4) {
		*rb = 8; *gb = 8; *bb = 8;
		*rs = 16; *gs = 8; *bs = 0;
		*ab = 0; *as = 24;
	} else {
		*rb = 5; *gb = 6; *bb = 5;
		*rs = 11; *gs = 5; *bs = 0;
		*ab = 0; *as = 0;
	}
}

int picasso_palette(struct MyCLUTEntry *CLUT, uae_u32 *clut)
{
	int rb, gb, bb, rs, gs, bs, ab, as;
	int changed = 0;
	int i;

	host_rgb_layout(&rb, &gb, &bb, &rs, &gs, &bs, &ab, &as);

	for (i = 0; i < 256 * 2; i++) {
		uae_u32 v = doMask256(CLUT[i].Red, rb, rs)
			| doMask256(CLUT[i].Green, gb, gs)
			| doMask256(CLUT[i].Blue, bb, bs)
			| doMask256((1 << ab) - 1, ab, as);
		if (v != clut[i]) {
			clut[i] = v;
			changed = 1;
		}
	}
	return changed;
}

void gfx_set_picasso_colors(int monid, RGBFTYPE rgbfmt)
{
	int rb, gb, bb, rs, gs, bs, ab, as;
	host_rgb_layout(&rb, &gb, &bb, &rs, &gs, &bs, &ab, &as);
	alloc_colors_picasso(rb, gb, bb, rs, gs, bs, rgbfmt, p96_rgbx16);
}

/* ------------------------------------------------------------------------- */
/* Frame buffer                                                              */
/* ------------------------------------------------------------------------- */
/* Largest RTG picture retro_bmp can hold, in bytes. retro_bmp is declared as
 * unsigned short, hence the doubling. */
/* sizeof the array itself, not RETRO_BMP_SIZE * 2: the macro is an unparenthesised
 * multiplication chain, and this cannot be got wrong if it ever gains a term. */
#define RTG_BUFFER_BYTES ((int)sizeof(retro_bmp))

static int rtg_width, rtg_height;   /* what the guest last asked for */
static int rtg_geometry_changed;

uae_u8 *gfx_lock_picasso(int monid, bool fullupdate)
{
	struct picasso_vidbuf_description *vidinfo = &picasso_vidinfo[monid];

	if (vidinfo->width <= 0 || vidinfo->height <= 0)
		return NULL;
	if (vidinfo->height * vidinfo->rowbytes > RTG_BUFFER_BYTES)
		return NULL;
	return (uae_u8 *)retro_bmp;
}

void gfx_unlock_picasso(int monid, bool dorender)
{
	/* retro_run() calls video_cb() on retro_bmp once a frame; there is no
	 * second present to do here. */
}

/* Picasso96 reports the rectangle it changed. The whole buffer goes to the
 * front-end every frame regardless, so this is bookkeeping only - but
 * p96_double_buffer_* must still be maintained, because picasso96.c reads
 * them back when it decides what to flush. */
void DX_Invalidate(struct AmigaMonitor *mon, int x, int y, int width, int height)
{
	struct picasso_vidbuf_description *vidinfo = &picasso_vidinfo[mon->monitor_id];

	if (width == 0 || height == 0)
		return;
	if (y < 0 || height < 0) {
		y = 0;
		height = vidinfo->height;
	}
	if (x < 0 || width < 0) {
		x = 0;
		width = vidinfo->width;
	}
	mon->p96_double_buffer_first = y;
	mon->p96_double_buffer_last = y + height - 1;
	mon->p96_double_buffer_firstx = x;
	mon->p96_double_buffer_lastx = x + width - 1;
	mon->p96_double_buffer_needs_flushing = 1;
}

int WIN32GFX_GetWidth(struct AmigaMonitor *mon)
{
	return picasso_vidinfo[mon->monitor_id].width;
}

int WIN32GFX_GetHeight(struct AmigaMonitor *mon)
{
	return picasso_vidinfo[mon->monitor_id].height;
}

/* ------------------------------------------------------------------------- */
/* Mode switching                                                            */
/* ------------------------------------------------------------------------- */
void gfx_set_picasso_modeinfo(int monid, RGBFTYPE rgbfmt)
{
	struct AmigaMonitor *mon = &AMonitors[monid];
	struct picasso96_state_struct *state = &picasso96_state[monid];
	struct picasso_vidbuf_description *vidinfo = &picasso_vidinfo[monid];
	int w = state->Width;
	int h = state->Height;

	if (!mon->screen_is_picasso)
		return;
	if (w <= 0 || h <= 0)
		return;

	/* A mode that cannot fit in retro_bmp is refused rather than half-drawn:
	 * picasso96.c carries on with the old one and the guest sees the screen
	 * fail to open, which is the honest outcome. picasso_host_enumerate_modes()
	 * keeps such modes off the list in the first place, so this only catches a
	 * guest that asks for a mode it was never offered. */
	if (h * w * pix_bytes > RTG_BUFFER_BYTES) {
		write_log(_T("RTG: %dx%dx%d does not fit the %d byte frame buffer\n"),
			w, h, pix_bytes * 8, RTG_BUFFER_BYTES);
		return;
	}

	gfx_set_picasso_colors(monid, rgbfmt);

	vidinfo->width = w;
	vidinfo->height = h;
	vidinfo->depth = pix_bytes * 8;
	vidinfo->pixbytes = pix_bytes;
	vidinfo->rowbytes = w * pix_bytes;
	vidinfo->maxwidth = w;
	vidinfo->maxheight = h;
	vidinfo->offset = 0;
	vidinfo->extra_mem = 1;
	vidinfo->splitypos = -1;
	vidinfo->full_refresh = 1;

	if (rtg_width != w || rtg_height != h) {
		rtg_width = w;
		rtg_height = h;
		rtg_geometry_changed = 1;
		write_log(_T("RTG: %dx%d host %dbit\n"), w, h, pix_bytes * 8);
	}

	state->ModeChanged = false;
}

void gfx_set_picasso_state(int monid, int on)
{
	struct AmigaMonitor *mon = &AMonitors[monid];
	struct picasso_vidbuf_description *vidinfo = &picasso_vidinfo[monid];

	if (mon->screen_is_picasso == on)
		return;
	mon->screen_is_picasso = on;
	if (monid == 0)
		screen_is_picasso = on;

	/* Both directions are a geometry change: on, to the RTG mode; off, back to
	 * whatever the chipset machinery works out for itself. */
	rtg_geometry_changed = 1;
	if (on) {
		vidinfo->full_refresh = 1;
	} else {
		rtg_width = 0;
		rtg_height = 0;
	}
	write_log(_T("RTG: display is now %s\n"), on ? _T("Picasso96") : _T("chipset"));
}

/* ------------------------------------------------------------------------- */
/* What libretro-core.c asks us                                              */
/* ------------------------------------------------------------------------- */
int picasso_host_active(void)
{
	return screen_is_picasso && rtg_width > 0 && rtg_height > 0;
}

int picasso_host_width(void)
{
	return rtg_width;
}

int picasso_host_height(void)
{
	return rtg_height;
}

int picasso_host_take_geometry_change(void)
{
	int v = rtg_geometry_changed;
	rtg_geometry_changed = 0;
	return v;
}

/* ------------------------------------------------------------------------- */
/* The mode list                                                             */
/* ------------------------------------------------------------------------- */
/* WinUAE fills Displays[] from EnumDisplaySettings - the modes the host monitor
 * can really do, because it opens a real full-screen mode for RTG. Nothing here
 * opens a mode: the picture is a texture the front-end scales, so the list is
 * simply the set of screen sizes we are willing to hand over. It is the classic
 * Picasso96 ladder, cut off at what retro_bmp holds at 32 bits per pixel.
 *
 * Picasso96 expands each entry over every depth in picasso96_modeflags, and
 * skips any that does not fit the board's VRAM, so this is resolutions only. */
static const struct { int w, h; } rtg_modes[] = {
	{  320,  200 }, {  320,  240 }, {  320,  256 },
	{  640,  400 }, {  640,  480 }, {  640,  512 },
	{  800,  600 },
	{ 1024,  768 },
	{ 1152,  864 },
	{ 1280, 1024 },
};

static struct PicassoResolution host_modes[sizeof(rtg_modes) / sizeof(rtg_modes[0]) + 1];

void picasso_host_enumerate_modes(void)
{
	static int done;
	unsigned i;
	int cnt = 0;

	if (done)
		return;
	done = 1;

	for (i = 0; i < sizeof(rtg_modes) / sizeof(rtg_modes[0]); i++) {
		int w = rtg_modes[i].w;
		int h = rtg_modes[i].h;
		/* 4 bytes per pixel, not pix_bytes: the front-end pixel format can be
		 * changed by a core option between runs, and the list the guest's
		 * Picasso96 prefs remember must not depend on it. */
		if (w * h * 4 > RTG_BUFFER_BYTES)
			continue;
		host_modes[cnt].res.width = w;
		host_modes[cnt].res.height = h;
		host_modes[cnt].depth = 4;
		host_modes[cnt].refresh[0] = default_freq;
		host_modes[cnt].refreshtype[0] = 0;
		host_modes[cnt].colormodes = RGBMASK_8BIT | RGBMASK_15BIT | RGBMASK_16BIT |
			RGBMASK_24BIT | RGBMASK_32BIT;
		host_modes[cnt].rawmode = 0;
		host_modes[cnt].lace = false;
		_stprintf(host_modes[cnt].name, _T("%dx%d"), w, h);
		cnt++;
	}
	host_modes[cnt].depth = -1;

	Displays[0].primary = true;
	Displays[0].adaptername = my_strdup(_T("Peekaboo"));
	Displays[0].adapterid = my_strdup(_T("Peekaboo"));
	Displays[0].adapterkey = my_strdup(_T("Peekaboo"));
	Displays[0].monitorname = my_strdup(_T("Peekaboo"));
	Displays[0].monitorid = my_strdup(_T("Peekaboo"));
	Displays[0].fullname = my_strdup(_T("Peekaboo RTG"));
	Displays[0].DisplayModes = host_modes;
	memset(&Displays[1], 0, sizeof(Displays[1]));

	/* Nothing in this port ever sets rtg_modes in the config, and an unset mask
	 * means Picasso96 finds no usable depth at all. uaegfx converts every one of
	 * these to the host format in software, so offer them all. */
	if (!currprefs.picasso96_modeflags) {
		currprefs.picasso96_modeflags = changed_prefs.picasso96_modeflags =
			RGBMASK_8BIT | RGBMASK_15BIT | RGBMASK_16BIT | RGBMASK_24BIT | RGBMASK_32BIT;
	}

	write_log(_T("RTG: %d host display modes, up to %dx%d\n"), cnt,
		cnt ? host_modes[cnt - 1].res.width : 0,
		cnt ? host_modes[cnt - 1].res.height : 0);
}

#endif /* PICASSO96 */
