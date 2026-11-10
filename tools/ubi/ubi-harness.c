/*
 * ubi-harness.c -- run ubi_lite on a file holding a UBI partition (the
 * ECC-corrected data, as backup-netbsd.sh writes it), on the PC: scan,
 * remove the named volumes, scan again, and write the result.  The flash
 * is simulated: programming only clears bits, a page of all 0xff reads as
 * erased.
 *
 *	cc -Iinc -o ubi-harness ubi-harness.c ../../sys/dev/ubi/ubi_lite.c
 *	./ubi-harness in.bin out.bin badpeb [volume ...]
 *
 * inc/ holds stand-ins for the kernel headers ubi_lite.c includes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <sys/param.h>
#include <dev/ubi/ubi_lite.h>
#define PG 2048
#define PPB 64
static uint8_t *img; static long npebs; static long badpeb = -1;
static int rd(void *a __attribute__((unused)), uint32_t page, uint8_t *buf) {
	memcpy(buf, img + (long)page * PG, PG);
	for (int i = 0; i < PG; i++) if (buf[i] != 0xff) return 0;
	return UBI_READ_ERASED;
}
static int prog(void *a __attribute__((unused)), uint32_t page, const uint8_t *buf) {
	uint8_t *d = img + (long)page * PG;
	for (int i = 0; i < PG; i++) d[i] &= buf[i];	/* NAND: 1 -> 0 only */
	return 0;
}
static int er(void *a __attribute__((unused)), uint32_t b) { memset(img + (long)b * PG * PPB, 0xff, PG * PPB); return 0; }
static bool bad(void *a __attribute__((unused)), uint32_t b) { return (long)b == badpeb; }
int main(int argc, char **argv) {
	/* harness in.bin out.bin badpeb name... */
	FILE *f = fopen(argv[1], "rb"); fseek(f, 0, SEEK_END); long sz = ftell(f); rewind(f);
	img = malloc(sz); fread(img, 1, sz, f); fclose(f);
	npebs = sz / (PG * PPB); badpeb = atol(argv[3]);
	struct ubi_lite u = { .ul_read_page = rd, .ul_program_page = prog,
	    .ul_erase_block = er, .ul_block_bad = bad, .ul_npebs = npebs,
	    .ul_page_size = PG, .ul_pages_per_peb = PPB };
	u.ul_peb = calloc(npebs, sizeof(*u.ul_peb)); u.ul_buf = malloc(PG);
	int e = ubi_lite_scan(&u);
	printf("scan %d: used %u free %u bad %u corrupt %u sqnum %llu\n", e, u.ul_nused, u.ul_nfree, u.ul_nbad, u.ul_ncorrupt, (unsigned long long)u.ul_max_sqnum);
	uint8_t *leb = malloc(u.ul_leb_size);
	for (int i = 4; i < argc; i++) {
		int v = ubi_lite_lookup(&u, argv[i]);
		e = v < 0 ? ENOENT : ubi_lite_remove(&u, v, leb);
		printf("remove %s (vol %d): %d\n", argv[i], v, e);
	}
	/* scan again from the flash, as the next boot would */
	e = ubi_lite_scan(&u);
	printf("rescan %d: used %u free %u\n", e, u.ul_nused, u.ul_nfree);
	for (int v = 0; v < UBI_LITE_MAXVOL; v++) if (u.ul_vol[v].v_reserved_pebs)
		printf("  vol %d %s %u LEBs %u mapped\n", v, u.ul_vol[v].v_name, u.ul_vol[v].v_reserved_pebs, ubi_lite_mapped(&u, v));
	f = fopen(argv[2], "wb"); fwrite(img, 1, sz, f); fclose(f);
	return 0;
}
