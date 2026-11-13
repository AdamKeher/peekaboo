/*
 * ADM copper / bitplane-pointer write blocks - textually included by
 * sources/src/custom.c.
 *
 * Two write-suppression debug aids.  Neither stops the copper or changes any
 * timing: the copper program still runs, WAIT/SKIP still execute, copper DMA
 * cycles are still consumed and the copper PC still advances.  Only the effect
 * of selected register writes is dropped, inside a chosen band of scanlines.
 *
 *   Copper block        - drop every copper register write on lines in range.
 *                         Answers "which lines is this copper list responsible
 *                         for": sweep the band and whatever stops changing was
 *                         the copper's doing.  CPU writes are untouched, so
 *                         what disappears is exactly the copper's contribution.
 *
 *   Bitplane ptr block  - drop copper writes to BPLxPT for selected planes in
 *                         range.  Mid-screen bitplane re-pointing is how split
 *                         screens, hardware scrollers and copper-driven buffer
 *                         swaps work; pinning one plane's pointer makes the
 *                         part of the picture it feeds visibly repeat, which
 *                         identifies which MOVE drives which plane where.
 *
 * This is an .inc rather than a .c because it reads custom.c's file statics
 * (copper_access, vpos), so it cannot be its own translation unit.  Included
 * after `static int copper_access;` and before both use sites.
 *
 * Adapted from the ENGINE9000 debugger by alpine9000
 * (https://github.com/alpine9000/engine9000-public), GPL-2 - the same licence
 * this project is distributed under (see COPYING).  Specifically
 * ami9000/sources/src/custom.c (custom_isCopperLineLimited,
 * custom_isCopperBitplanePointerWriteBlocked and their setters).  See
 * PATCHES.md.
 */

/* Defaults span roughly the PAL visible display window. */
static int adm_copper_block_enabled = 0;
static int adm_copper_block_start = 52;
static int adm_copper_block_end = 308;

/* bit 0 = BPL1PT ... bit 5 = BPL6PT (the OCS/ECS pointer set). */
static int adm_bplptr_block_mask = 0;
static int adm_bplptr_block_start = 52;
static int adm_bplptr_block_end = 308;

static int adm_block_clamp_line(int line)
{
	if (line < 0) {
		return 0;
	}
	if (line >= MAXVPOS_LINES_ECS) {
		return MAXVPOS_LINES_ECS - 1;
	}
	return line;
}

/* Inclusive, and tolerant of the two ends being entered the wrong way round. */
static int adm_block_line_in_range(int line, int startLine, int endLine)
{
	if (startLine > endLine) {
		int tmp = startLine;
		startLine = endLine;
		endLine = tmp;
	}
	return line >= startLine && line <= endLine;
}

static int adm_is_copper_line_blocked(int line)
{
	if (!adm_copper_block_enabled) {
		return 0;
	}
	return adm_block_line_in_range(line, adm_copper_block_start, adm_copper_block_end);
}

/* addr is the $DFF000 register offset; PTH and PTL fold to the same plane, so
   a pointer is blocked as a whole rather than a half of one being left live. */
static int adm_bplptr_index_from_addr(uaecptr addr)
{
	switch (addr & 0x1FE) {
		case 0x0E0:
		case 0x0E2:
			return 0;
		case 0x0E4:
		case 0x0E6:
			return 1;
		case 0x0E8:
		case 0x0EA:
			return 2;
		case 0x0EC:
		case 0x0EE:
			return 3;
		case 0x0F0:
		case 0x0F2:
			return 4;
		case 0x0F4:
		case 0x0F6:
			return 5;
		default:
			break;
	}
	return -1;
}

static int adm_is_copper_bplptr_write_blocked(uaecptr addr)
{
	int idx;

	/* Copper writes only. A CPU write to BPLxPT still lands - the point of
	   this tool is to isolate what the copper is doing mid-screen. */
	if (!copper_access) {
		return 0;
	}
	if (!adm_bplptr_block_mask) {
		return 0;
	}
	idx = adm_bplptr_index_from_addr(addr);
	if (idx < 0) {
		return 0;
	}
	if (!(adm_bplptr_block_mask & (1 << idx))) {
		return 0;
	}
	return adm_block_line_in_range(vpos, adm_bplptr_block_start, adm_bplptr_block_end);
}

void adm_set_copper_block(int enabled, int startLine, int endLine)
{
	adm_copper_block_enabled = enabled ? 1 : 0;
	adm_copper_block_start = adm_block_clamp_line(startLine);
	adm_copper_block_end = adm_block_clamp_line(endLine);
}

void adm_set_bplptr_block(int mask, int startLine, int endLine)
{
	adm_bplptr_block_mask = mask & 0x3F;
	adm_bplptr_block_start = adm_block_clamp_line(startLine);
	adm_bplptr_block_end = adm_block_clamp_line(endLine);
}
