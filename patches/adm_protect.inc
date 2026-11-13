/* ADM memory protection (trainer locks) — see PATCHES.md.
 *
 * Blocks guest writes to an address or range, or forces a value there. This
 * file is #included by sources/src/memory.c immediately before
 * memory_put_long(), so the table is defined before the three functions that
 * consult it and no forward declarations are needed.
 *
 * Why here: every CPU write path in UAE funnels through
 * memory_put_{long,word,byte}. put_byte/put_word/put_long call them directly;
 * so do the _compatible and _jit variants; and the cycle-exact 68000 path
 * (put_*_ce000 -> mem_access_delay_*_write -> wait_cpu_cycle_write) ends up
 * calling put_byte/put_word/put_long too. Hooking these three covers the lot.
 *
 * The copper is hooked SEPARATELY, in custom.c: it reaches the chipset through
 * custom_wput_copper -> custom_wput_1 and never touches memory_put_*, so a lock
 * on a $DFFxxx register would otherwise only catch CPU writes - useless for the
 * registers a copper list actually drives (BPLCON1, the colours, the bitplane
 * pointers). That hook calls straight into adm_protect_check below.
 *
 * KNOWN LIMIT: DMA writes still do NOT pass through here. The blitter's D
 * channel and disk DMA write via their own paths, so a protected range in chip
 * RAM can still be modified by DMA. That is fine for game variables, which are
 * CPU-written.
 *
 * NOTE: adm_protect_set/adm_protect_clear/adm_protect_count are only visible
 * to the host because they are listed in libretro/link.T — the link uses
 * --version-script, so dllexport alone is ignored.
 */

#define ADM_PROTECT_MAX 256
#define ADM_PROTECT_MODE_BLOCK 0
#define ADM_PROTECT_MODE_SET 1

/* Must match adm::AmigaEmulator::ProtectEntry in src/core/AmigaEmulator.h.
   Natural alignment gives addr@0 len@4 mode@8 size@9 (pad) value@12,
   sizeof == 16; the host side static_asserts the same layout. */
typedef struct
{
	uae_u32 addr;  /* first protected byte */
	uae_u32 len;   /* number of protected bytes */
	uae_u8 mode;   /* ADM_PROTECT_MODE_BLOCK | ADM_PROTECT_MODE_SET */
	uae_u8 size;   /* 1/2/4 — width of the forced value (MODE_SET) */
	uae_u32 value; /* forced value, right-aligned (MODE_SET) */
} adm_protect_entry_t;

/* Read by the hooks in memory_put_* as a short-circuit, so the cost of the
   feature when nothing is protected is one predictable not-taken branch. */
int adm_protect_count = 0;
static adm_protect_entry_t adm_protect_table[ADM_PROTECT_MAX];

void adm_protect_clear(void)
{
	adm_protect_count = 0;
}

void adm_protect_set(const adm_protect_entry_t *entries, int count)
{
	int i;
	if (!entries || count <= 0) {
		adm_protect_count = 0;
		return;
	}
	if (count > ADM_PROTECT_MAX)
		count = ADM_PROTECT_MAX;
	for (i = 0; i < count; i++)
		adm_protect_table[i] = entries[i];
	adm_protect_count = count;
}

/* Consulted for every guest write once anything is protected.
 *
 * Returns 1 when the write must be dropped entirely (a BLOCK entry overlaps).
 * Returns 0 otherwise, having first spliced any overlapping SET entry's bytes
 * into *v — so a wide write that straddles a forced byte still lands, with the
 * forced byte surviving inside it.
 *
 * `width` is the access width in bytes (4/2/1). Values are big-endian on the
 * Amiga: for a `width`-byte access at `addr`, the byte at address b sits at
 * bit position (width - 1 - (b - addr)) * 8 of v.
 */
int adm_protect_check(uaecptr addr, int width, uae_u32 *v)
{
	int i;
	int blocked = 0;
	uae_u32 as = (uae_u32)addr;
	uae_u32 ae = (uae_u32)addr + (uae_u32)width;

	for (i = 0; i < adm_protect_count; i++) {
		const adm_protect_entry_t *e = &adm_protect_table[i];
		uae_u32 es = e->addr;
		uae_u32 ee = e->addr + e->len;
		uae_u32 b, from, to;

		if (ae <= es || as >= ee)
			continue; /* no overlap */

		if (e->mode == ADM_PROTECT_MODE_BLOCK) {
			blocked = 1;
			continue;
		}

		/* MODE_SET: overwrite the overlapping bytes in the outgoing value. */
		from = es > as ? es : as;
		to = ee < ae ? ee : ae;
		for (b = from; b < to; b++) {
			uae_u32 off = b - e->addr;
			int vshift, fshift;
			if (e->size == 0 || off >= (uae_u32)e->size)
				continue;
			vshift = (width - 1 - (int)(b - as)) * 8;
			fshift = ((int)e->size - 1 - (int)off) * 8;
			*v = (*v & ~((uae_u32)0xff << vshift)) |
			     ((((uae_u32)e->value >> fshift) & 0xffu) << vshift);
		}
	}

	return blocked;
}
