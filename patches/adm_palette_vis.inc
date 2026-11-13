/*
 * ADM palette visualiser - textually included by sources/src/drawing.c.
 *
 * Replaces a scanline's decoded pen indices with a left-to-right ramp through
 * every pen the current bitplane depth can address.  The call site is right
 * after pfield_doline() and before HAM decoding and sprites, which is what
 * makes this useful: adjust_drawing_colors() has already latched this line's
 * colour table (including anything the copper wrote part-way down the frame),
 * so only the *pixel data* is replaced.  The picture becomes a per-scanline
 * palette chart - X is pen index, Y is scanline - and every copper colour
 * split, fade or raster bar shows up as a change down the bars.  Sprites still
 * draw on top, since they are composited later.
 *
 * This is an .inc rather than a .c because it needs drawing.c's file statics
 * (pixdata, src_pixel, playfield_start/end, bplplanecnt, bpldualpf), so it
 * cannot be a separate translation unit.  Keeping it out of the build scripts
 * keeps the C reviewable and identical on both platforms.
 *
 * Adapted from the ENGINE9000 debugger by alpine9000
 * (https://github.com/alpine9000/engine9000-public), GPL-2 - the same licence
 * this project is distributed under (see COPYING).  Specifically
 * ami9000/sources/src/drawing.c (drawing_applyPaletteVisToScanline and
 * drawing_paletteVisEncodeDualPfColor).  See PATCHES.md.
 */

int adm_palette_vis_enabled = 0;

/* Dual playfield stores the two playfields interleaved across the odd and even
   bitplanes, so a pen index cannot be written into apixels[] directly: bit i of
   the index belongs in bit 2i (playfield 1) or 2i+1 (playfield 2).  Encoding it
   back this way makes UAE's own dual-playfield lookup resolve the pen we meant,
   from the playfield we meant. */
static uae_u8 adm_palette_vis_encode_dualpf(int colorIndex, int playfieldIndex)
{
	uae_u8 encoded = 0;
	int outputBit = playfieldIndex;
	int inputBit;

	for (inputBit = 0; inputBit < 4 && outputBit < 8; inputBit++, outputBit += 2) {
		if ((colorIndex & (1 << inputBit)) != 0) {
			encoded = (uae_u8)(encoded | (uae_u8)(1u << outputBit));
		}
	}
	return encoded;
}

static void adm_palette_vis_apply_scanline(void)
{
	int sourceStart = src_pixel;
	int sourceWidth;
	int pixelIndex;

	if (!adm_palette_vis_enabled) {
		return;
	}

	sourceWidth = res_shift_from_window(playfield_end - playfield_start);
	if (sourceWidth <= 0) {
		return;
	}
	if (sourceStart < 0 || sourceStart >= MAX_PIXELS_PER_LINE * 2) {
		return;
	}
	if (sourceStart + sourceWidth > MAX_PIXELS_PER_LINE * 2) {
		sourceWidth = (MAX_PIXELS_PER_LINE * 2) - sourceStart;
		if (sourceWidth <= 0) {
			return;
		}
	}

	if (bpldualpf) {
		int pf1PlaneCount = (bplplanecnt + 1) / 2;
		int pf2PlaneCount = bplplanecnt / 2;
		int pf1ColorCount = 1 << pf1PlaneCount;
		int pf2ColorCount = 1 << pf2PlaneCount;
		/* Pen 0 is transparent in both playfields, so the two ranges share one
		   column rather than each getting their own identical blank. */
		int columnCount = pf1ColorCount + pf2ColorCount - 1;

		if (columnCount <= 0) {
			return;
		}
		for (pixelIndex = 0; pixelIndex < sourceWidth; pixelIndex++) {
			int columnIndex = (pixelIndex * columnCount) / sourceWidth;
			uae_u8 encoded;

			if (columnIndex == 0) {
				encoded = 0;
			} else if (columnIndex < pf1ColorCount) {
				encoded = adm_palette_vis_encode_dualpf(columnIndex, 0);
			} else {
				encoded = adm_palette_vis_encode_dualpf(columnIndex - pf1ColorCount + 1, 1);
			}
			pixdata.apixels[sourceStart + pixelIndex] = encoded;
		}
		return;
	}

	{
		int colorCount = 1 << bplplanecnt;

		if (colorCount <= 0) {
			return;
		}
		for (pixelIndex = 0; pixelIndex < sourceWidth; pixelIndex++) {
			int colorIndex = (pixelIndex * colorCount) / sourceWidth;

			if (colorIndex >= colorCount) {
				colorIndex = colorCount - 1;
			}
			pixdata.apixels[sourceStart + pixelIndex] = (uae_u8)colorIndex;
		}
	}
}
