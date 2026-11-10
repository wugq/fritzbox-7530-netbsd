/* $NetBSD$ */

/*-
 * Copyright (c) 2026 wugq
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * ubi_lite: a small implementation of Linux' UBI on-flash format (see
 * Linux' include/uapi/mtd/ubi-media.h for the format; this is a separate
 * implementation).  Enough to find the volumes of an existing UBI device,
 * read them, and rewrite the eraseblocks of a dynamic volume the way UBI
 * does (new copy with a higher sequence number, then erase the old), and
 * create and remove volumes and swap their names.  No wear-leveling
 * moves, no fastmap.
 *
 * OS-independent: flash access goes through the callbacks in struct
 * ubi_lite, the caller provides the per-eraseblock array.
 *
 * UBI in brief: the partition is divided into physical eraseblocks
 * (PEBs).  Each PEB starts with an erase counter header ("UBI#", the
 * number of times it was erased) and, if in use, a volume identifier
 * header ("UBI!": volume id, logical eraseblock number, sequence number).
 * The rest of the PEB is the logical eraseblock's data.  The layout
 * volume (two copies) holds the volume table: names and sizes.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>

#include <dev/ubi/ubi_lite.h>

#define UBI_EC_MAGIC		0x55424923	/* "UBI#" */
#define UBI_VID_MAGIC		0x55424921	/* "UBI!" */
#define UBI_CRC32_INIT		0xffffffffU
#define UBI_HDR_SIZE		64
#define UBI_VTBL_RECORD_SIZE	172
#define UBI_MAX_VOLUMES		128	/* records in the volume table */
#define UBI_LAYOUT_LEBS		2	/* the layout volume: two copies */
#define UBI_COMPAT_REJECT	5	/* the layout volume's "compat" */
#define UBI_VOL_NAME_MAX	127

static uint32_t
be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
	    (uint32_t)p[2] << 8 | p[3];
}

static uint64_t
be64(const uint8_t *p)
{
	return (uint64_t)be32(p) << 32 | be32(p + 4);
}

static void drop_torn_copies(struct ubi_lite *);
static bool vtbl_copy_valid(struct ubi_lite *, uint32_t);

/* UBI's CRC-32: the reflected IEEE polynomial, no final inversion */
uint32_t
ubi_crc32(uint32_t crc, const uint8_t *p, size_t n)
{
	while (n--) {
		crc ^= *p++;
		for (int k = 0; k < 8; k++)
			crc = (crc >> 1) ^ (0xedb88320U & -(crc & 1));
	}
	return crc;
}

static int
ubi_read(struct ubi_lite *u, uint32_t peb, uint32_t pg, uint8_t *buf)
{
	return u->ul_read_page(u->ul_arg,
	    (u->ul_first_peb + peb) * u->ul_pages_per_peb + pg, buf);
}

/*
 * Scan every PEB: erase counter and VID header.  For each (volume, LEB)
 * the copy with the highest sequence number wins (the older one is
 * still there if the power failed before UBI erased it).
 */
int
ubi_lite_scan(struct ubi_lite *u)
{
	uint8_t *buf = u->ul_buf;

	u->ul_max_sqnum = 0;
	u->ul_nbad = u->ul_nfree = u->ul_nused = u->ul_ncorrupt = 0;
	for (uint32_t p = 0; p < u->ul_npebs; p++) {
		struct ubi_peb *pe = &u->ul_peb[p];
		int error;

		memset(pe, 0, sizeof(*pe));
		if (u->ul_block_bad(u->ul_arg, u->ul_first_peb + p)) {
			pe->pe_state = UBI_PEB_BAD;
			u->ul_nbad++;
			continue;
		}
		error = ubi_read(u, p, 0, buf);
		if (error == UBI_READ_ERASED) {
			pe->pe_state = UBI_PEB_EMPTY;	/* not even an EC header */
			u->ul_nfree++;
			continue;
		}
		if (error != 0 || be32(buf) != UBI_EC_MAGIC ||
		    ubi_crc32(UBI_CRC32_INIT, buf, UBI_HDR_SIZE - 4) !=
		    be32(buf + 60)) {
			pe->pe_state = UBI_PEB_CORRUPT;
			u->ul_ncorrupt++;
			continue;
		}
		pe->pe_ec = be64(buf + 8);
		if (u->ul_vid_offset == 0) {
			u->ul_vid_offset = be32(buf + 16);
			u->ul_data_offset = be32(buf + 20);
			u->ul_image_seq = be32(buf + 24);
		}
		error = ubi_read(u, p, u->ul_vid_offset / u->ul_page_size, buf);
		if (error == UBI_READ_ERASED) {
			pe->pe_state = UBI_PEB_FREE;	/* EC header only */
			u->ul_nfree++;
			continue;
		}
		const uint8_t *v = buf + u->ul_vid_offset % u->ul_page_size;
		if (error != 0 || be32(v) != UBI_VID_MAGIC ||
		    ubi_crc32(UBI_CRC32_INIT, v, UBI_HDR_SIZE - 4) !=
		    be32(v + 60)) {
			pe->pe_state = UBI_PEB_CORRUPT;
			u->ul_ncorrupt++;
			continue;
		}
		pe->pe_state = UBI_PEB_USED;
		pe->pe_vol_type = v[5];
		pe->pe_copy_flag = v[6];
		pe->pe_vol_id = be32(v + 8);
		pe->pe_lnum = be32(v + 12);
		pe->pe_data_size = be32(v + 20);
		pe->pe_data_crc = be32(v + 32);
		pe->pe_sqnum = be64(v + 40);
		if (pe->pe_sqnum > u->ul_max_sqnum)
			u->ul_max_sqnum = pe->pe_sqnum;
		u->ul_nused++;
	}
	if (u->ul_vid_offset == 0)
		return ENOENT;			/* no UBI here */
	u->ul_leb_size = u->ul_pages_per_peb * u->ul_page_size -
	    u->ul_data_offset;
	drop_torn_copies(u);
	return ubi_lite_read_vtbl(u);
}

/* UBI's CRC of the first len bytes of a PEB's data; -1 on a read error */
static int
peb_data_crc(struct ubi_lite *u, uint32_t p, uint32_t len, uint32_t *crc)
{
	uint32_t c = UBI_CRC32_INIT;

	for (uint32_t off = 0; off < len; off += u->ul_page_size) {
		const uint32_t n = MIN(u->ul_page_size, len - off);
		int error = ubi_read(u, p, (u->ul_data_offset + off) /
		    u->ul_page_size, u->ul_buf);

		if (error == UBI_READ_ERASED)
			memset(u->ul_buf, 0xff, u->ul_page_size);
		else if (error != 0)
			return -1;
		c = ubi_crc32(c, u->ul_buf, n);
	}
	*crc = c;
	return 0;
}

/*
 * Two PEBs for one LEB: an atomic LEB change was cut short, after the new
 * copy's VID header was written but before the old copy was erased.  The
 * newer one wins, as in ubi_lite_find(), unless it carries a data CRC
 * (copy_flag) that its data does not match: then the power failed while
 * it was being written, and the older copy is the good one.  This is what
 * Linux' compare_lebs() does.  The torn copy is marked corrupt.
 */
static void
drop_torn_copies(struct ubi_lite *u)
{
	for (uint32_t p = 0; p < u->ul_npebs; p++) {
		struct ubi_peb *pe = &u->ul_peb[p];
		bool older = false;
		uint32_t crc;

		if (pe->pe_state != UBI_PEB_USED || !pe->pe_copy_flag)
			continue;
		for (uint32_t q = 0; q < u->ul_npebs && !older; q++) {
			const struct ubi_peb *qe = &u->ul_peb[q];

			older = q != p && qe->pe_state == UBI_PEB_USED &&
			    qe->pe_vol_id == pe->pe_vol_id &&
			    qe->pe_lnum == pe->pe_lnum &&
			    qe->pe_sqnum < pe->pe_sqnum;
		}
		if (!older || pe->pe_data_size > u->ul_leb_size)
			continue;
		if (peb_data_crc(u, p, pe->pe_data_size, &crc) != 0 ||
		    crc != pe->pe_data_crc) {
			pe->pe_state = UBI_PEB_CORRUPT;
			u->ul_nused--;
			u->ul_ncorrupt++;
		}
	}
}

/* The PEB holding (vol_id, lnum), or -1 if the LEB is unmapped. */
int
ubi_lite_find(struct ubi_lite *u, uint32_t vol_id, uint32_t lnum)
{
	int best = -1;

	for (uint32_t p = 0; p < u->ul_npebs; p++) {
		const struct ubi_peb *pe = &u->ul_peb[p];

		if (pe->pe_state != UBI_PEB_USED || pe->pe_vol_id != vol_id ||
		    pe->pe_lnum != lnum)
			continue;
		if (best < 0 || pe->pe_sqnum > u->ul_peb[best].pe_sqnum)
			best = p;
	}
	return best;
}

/* Read len bytes at offset off of a LEB; unmapped LEBs read as 0xff. */
int
ubi_lite_read_leb(struct ubi_lite *u, uint32_t vol_id, uint32_t lnum,
    uint32_t off, uint8_t *dst, uint32_t len)
{
	const int p = ubi_lite_find(u, vol_id, lnum);

	if (off + len > u->ul_leb_size)
		return EINVAL;
	if (p < 0) {
		memset(dst, 0xff, len);
		return 0;
	}
	while (len > 0) {
		const uint32_t pos = u->ul_data_offset + off;
		const uint32_t pg = pos / u->ul_page_size;
		const uint32_t in = pos % u->ul_page_size;
		const uint32_t n = MIN(len, u->ul_page_size - in);
		int error = ubi_read(u, p, pg, u->ul_buf);

		if (error == UBI_READ_ERASED)
			memset(u->ul_buf, 0xff, u->ul_page_size);
		else if (error != 0)
			return error;
		memcpy(dst, u->ul_buf + in, n);
		dst += n;
		off += n;
		len -= n;
	}
	return 0;
}

/*
 * The volume table: from the layout volume's LEB 0, or LEB 1 if LEB 0 is
 * missing or has a record that is not valid (a write of the table cut
 * short), as Linux' process_lvol() does.
 */
int
ubi_lite_read_vtbl(struct ubi_lite *u)
{
	uint8_t rec[UBI_VTBL_RECORD_SIZE];
	uint32_t copy;

	memset(u->ul_vol, 0, sizeof(u->ul_vol));
	for (copy = 0; copy < UBI_LAYOUT_LEBS; copy++)
		if (vtbl_copy_valid(u, copy))
			break;
	if (copy == UBI_LAYOUT_LEBS)
		return ENOENT;
	for (uint32_t i = 0; i < UBI_LITE_MAXVOL; i++) {
		struct ubi_vol *vol = &u->ul_vol[i];

		if (ubi_lite_read_leb(u, UBI_LAYOUT_VOLUME_ID, copy,
		    i * UBI_VTBL_RECORD_SIZE, rec, sizeof(rec)) != 0)
			return EIO;
		vol->v_reserved_pebs = be32(rec);
		if (vol->v_reserved_pebs == 0)
			continue;
		vol->v_type = rec[12];
		const u_int nl = MIN((uint32_t)(rec[14] << 8 | rec[15]),
		    sizeof(vol->v_name) - 1);
		memcpy(vol->v_name, rec + 16, nl);
		vol->v_name[nl] = '\0';
	}
	u->ul_vtbl_copy = copy;
	return 0;
}

/* volume id by name, or -1 */
int
ubi_lite_lookup(struct ubi_lite *u, const char *name)
{
	for (int i = 0; i < UBI_LITE_MAXVOL; i++)
		if (u->ul_vol[i].v_reserved_pebs != 0 &&
		    strcmp(u->ul_vol[i].v_name, name) == 0)
			return i;
	return -1;
}

/* LEBs of a volume that hold data */
uint32_t
ubi_lite_mapped(struct ubi_lite *u, uint32_t vol_id)
{
	uint32_t n = 0;

	for (uint32_t l = 0; l < u->ul_vol[vol_id].v_reserved_pebs; l++)
		if (ubi_lite_find(u, vol_id, l) >= 0)
			n++;
	return n;
}

/* --- writing ------------------------------------------------------- */

static void
put_be32(uint8_t *p, uint32_t v)
{
	p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

static void
put_be64(uint8_t *p, uint64_t v)
{
	put_be32(p, v >> 32);
	put_be32(p + 4, (uint32_t)v);
}

static int
ubi_program(struct ubi_lite *u, uint32_t peb, uint32_t pg, const uint8_t *buf)
{
	return u->ul_program_page(u->ul_arg,
	    (u->ul_first_peb + peb) * u->ul_pages_per_peb + pg, buf);
}

/* Erase a PEB and give it a fresh erase counter header (ec + 1). */
static int
ubi_erase_peb(struct ubi_lite *u, uint32_t p)
{
	struct ubi_peb *pe = &u->ul_peb[p];
	uint8_t *h = u->ul_buf;
	int error;

	if ((error = u->ul_erase_block(u->ul_arg, u->ul_first_peb + p)) != 0) {
		pe->pe_state = UBI_PEB_CORRUPT;
		return error;
	}
	pe->pe_ec++;
	memset(h, 0xff, u->ul_page_size);
	memset(h, 0, UBI_HDR_SIZE);
	put_be32(h, UBI_EC_MAGIC);
	h[4] = 1;				/* version */
	put_be64(h + 8, pe->pe_ec);
	put_be32(h + 16, u->ul_vid_offset);
	put_be32(h + 20, u->ul_data_offset);
	put_be32(h + 24, u->ul_image_seq);
	put_be32(h + 60, ubi_crc32(UBI_CRC32_INIT, h, UBI_HDR_SIZE - 4));
	if ((error = ubi_program(u, p, 0, h)) != 0) {
		pe->pe_state = UBI_PEB_CORRUPT;
		return error;
	}
	pe->pe_state = UBI_PEB_FREE;
	return 0;
}

/* the free PEB with the lowest erase counter, or -1 */
static int
ubi_pick_free(struct ubi_lite *u)
{
	int best = -1;

	for (uint32_t p = 0; p < u->ul_npebs; p++)
		if (u->ul_peb[p].pe_state == UBI_PEB_FREE &&
		    (best < 0 || u->ul_peb[p].pe_ec < u->ul_peb[best].pe_ec))
			best = p;
	return best;
}

static int ubi_write_leb(struct ubi_lite *, uint32_t, uint32_t, uint8_t,
    const uint8_t *, uint32_t);

/*
 * Replace the contents of a LEB of a dynamic volume, as UBI's "atomic LEB
 * change": write the data to a free PEB under a new VID header with a
 * higher sequence number, check it, then erase the old PEB.  If the power
 * fails in between, both copies exist and the scan picks the newer one.
 * len bytes from data (the rest of the LEB reads as 0xff).
 */
int
ubi_lite_write_leb(struct ubi_lite *u, uint32_t vol_id, uint32_t lnum,
    const uint8_t *data, uint32_t len)
{
	if (vol_id >= UBI_LITE_MAXVOL || u->ul_vol[vol_id].v_type != 1 ||
	    lnum >= u->ul_vol[vol_id].v_reserved_pebs || len > u->ul_leb_size)
		return EINVAL;
	return ubi_write_leb(u, vol_id, lnum, 0, data, len);
}

/*
 * ubi_lite_write_leb() without the checks, for user volumes (compat 0)
 * and the layout volume (compat UBI_COMPAT_REJECT: UBI implementations
 * that do not know an internal volume must refuse the device).
 */
static int
ubi_write_leb(struct ubi_lite *u, uint32_t vol_id, uint32_t lnum,
    uint8_t compat, const uint8_t *data, uint32_t len)
{
	const int old = ubi_lite_find(u, vol_id, lnum);
	uint8_t *h = u->ul_buf;
	int p, error;

	for (;;) {
		if ((p = ubi_pick_free(u)) < 0)
			return ENOSPC;
		/* a PEB that was free at scan time could have gone bad */
		if (u->ul_block_bad(u->ul_arg, u->ul_first_peb + p)) {
			u->ul_peb[p].pe_state = UBI_PEB_BAD;
			continue;
		}
		break;
	}
	struct ubi_peb *pe = &u->ul_peb[p];

	/* VID header, in the page at vid_offset */
	memset(h, 0xff, u->ul_page_size);
	uint8_t *v = h + u->ul_vid_offset % u->ul_page_size;
	memset(v, 0, UBI_HDR_SIZE);
	put_be32(v, UBI_VID_MAGIC);
	v[4] = 1;				/* version */
	v[5] = 1;				/* dynamic */
	v[7] = compat;
	put_be32(v + 8, vol_id);
	put_be32(v + 12, lnum);
	const uint32_t data_crc = len > 0 ?
	    ubi_crc32(UBI_CRC32_INIT, data, len) : 0;
	if (len > 0) {
		/*
		 * copy_flag, data size and CRC: if the power fails before
		 * the old copy is erased, the scan can tell whether this
		 * one was written whole (drop_torn_copies()).
		 */
		v[6] = 1;
		put_be32(v + 20, len);
		put_be32(v + 32, data_crc);
	}
	put_be64(v + 40, u->ul_max_sqnum + 1);
	put_be32(v + 60, ubi_crc32(UBI_CRC32_INIT, v, UBI_HDR_SIZE - 4));
	error = ubi_program(u, p, u->ul_vid_offset / u->ul_page_size, h);

	/* the data, a page at a time */
	for (uint32_t off = 0; error == 0 && off < len; off += u->ul_page_size) {
		const uint32_t n = MIN(u->ul_page_size, len - off);
		memset(h, 0xff, u->ul_page_size);
		memcpy(h, data + off, n);
		error = ubi_program(u, p, (u->ul_data_offset + off) /
		    u->ul_page_size, h);
	}
	/* read it back */
	for (uint32_t off = 0; error == 0 && off < len; off += u->ul_page_size) {
		const uint32_t n = MIN(u->ul_page_size, len - off);
		error = ubi_read(u, p, (u->ul_data_offset + off) /
		    u->ul_page_size, h);
		if (error == UBI_READ_ERASED) {
			/* a page of data that is all 0xff reads as erased */
			memset(h, 0xff, u->ul_page_size);
			error = 0;
		}
		u->ul_fail_peb = p;
		u->ul_fail_off = off;
		u->ul_fail_error = error;
		if (error == 0 && memcmp(h, data + off, n) != 0) {
			for (uint32_t i = 0; i < n; i++)
				if (h[i] != data[off + i]) {
					u->ul_fail_off = off + i;
					u->ul_fail_want = data[off + i];
					u->ul_fail_got = h[i];
					break;
				}
			error = EIO;
		}
	}
	if (error != 0) {
		/* this PEB is half written: wipe it, try nothing else now */
		(void)ubi_erase_peb(u, p);
		return error;
	}
	u->ul_max_sqnum++;
	pe->pe_state = UBI_PEB_USED;
	pe->pe_vol_type = 1;
	pe->pe_copy_flag = len > 0;
	pe->pe_vol_id = vol_id;
	pe->pe_lnum = lnum;
	pe->pe_data_size = len;
	pe->pe_data_crc = data_crc;
	pe->pe_sqnum = u->ul_max_sqnum;

	/* the old copy goes back to the free pool */
	if (old >= 0)
		(void)ubi_erase_peb(u, old);
	return 0;
}

/* --- the volume table ------------------------------------------- */

/*
 * The whole table, as UBI writes it: one record per possible volume (128
 * fit in a LEB), padded to whole pages.  We keep only UBI_LITE_MAXVOL of
 * them in ul_vol, but read and write all, so records we do not otherwise
 * look at are kept as they are.
 */
static uint32_t
vtbl_slots(const struct ubi_lite *u)
{
	return MIN(u->ul_leb_size / UBI_VTBL_RECORD_SIZE, UBI_MAX_VOLUMES);
}

static uint32_t
vtbl_size(const struct ubi_lite *u)
{
	return roundup(vtbl_slots(u) * UBI_VTBL_RECORD_SIZE, u->ul_page_size);
}

static bool
vtbl_rec_ok(const uint8_t *rec)
{
	return ubi_crc32(UBI_CRC32_INIT, rec, UBI_VTBL_RECORD_SIZE - 4) ==
	    be32(rec + UBI_VTBL_RECORD_SIZE - 4);
}

static void
vtbl_rec_crc(uint8_t *rec)
{
	put_be32(rec + UBI_VTBL_RECORD_SIZE - 4,
	    ubi_crc32(UBI_CRC32_INIT, rec, UBI_VTBL_RECORD_SIZE - 4));
}

/*
 * A copy of the table UBI would accept: present, every record's CRC
 * right, and every unused record exactly the empty one (Linux'
 * vtbl_check()).
 */
static bool
vtbl_copy_valid(struct ubi_lite *u, uint32_t copy)
{
	uint8_t rec[UBI_VTBL_RECORD_SIZE], empty[UBI_VTBL_RECORD_SIZE];

	if (ubi_lite_find(u, UBI_LAYOUT_VOLUME_ID, copy) < 0)
		return false;
	memset(empty, 0, sizeof(empty));
	vtbl_rec_crc(empty);
	for (uint32_t i = 0; i < vtbl_slots(u); i++) {
		if (ubi_lite_read_leb(u, UBI_LAYOUT_VOLUME_ID, copy,
		    i * UBI_VTBL_RECORD_SIZE, rec, sizeof(rec)) != 0 ||
		    !vtbl_rec_ok(rec) ||
		    (be32(rec) == 0 && memcmp(rec, empty, sizeof(rec)) != 0))
			return false;
	}
	return true;
}

/*
 * Read the table into buf (room for one LEB): the copy ubi_lite_read_vtbl()
 * chose, which UBI would accept, so a damaged copy is never written back.
 */
static int
vtbl_load(struct ubi_lite *u, uint8_t *buf)
{
	if (!vtbl_copy_valid(u, u->ul_vtbl_copy))
		return EIO;
	return ubi_lite_read_leb(u, UBI_LAYOUT_VOLUME_ID, u->ul_vtbl_copy, 0,
	    buf, vtbl_size(u));
}

/*
 * Write the table into both copies of the layout volume, copy 0 first: at
 * attach, UBI trusts copy 0 and repairs copy 1 from it, so a power failure
 * at any point leaves either the old table or the new one.  Then update
 * ul_vol from it.
 */
static int
vtbl_store(struct ubi_lite *u, const uint8_t *buf)
{
	int error;

	for (uint32_t copy = 0; copy < UBI_LAYOUT_LEBS; copy++) {
		error = ubi_write_leb(u, UBI_LAYOUT_VOLUME_ID, copy,
		    UBI_COMPAT_REJECT, buf, vtbl_size(u));
		if (error != 0)
			return error;
	}
	return ubi_lite_read_vtbl(u);
}

/* erase every PEB that carries vol_id; EIO if one could not be erased */
static int
erase_volume_pebs(struct ubi_lite *u, uint32_t vol_id)
{
	uint32_t nerr = 0;

	for (uint32_t p = 0; p < u->ul_npebs; p++) {
		const struct ubi_peb *pe = &u->ul_peb[p];

		if (pe->pe_state == UBI_PEB_USED && pe->pe_vol_id == vol_id &&
		    ubi_erase_peb(u, p) != 0)
			nerr++;
	}
	return nerr == 0 ? 0 : EIO;
}

/*
 * Remove a volume, as Linux' ubirmvol does: replace its record with an
 * empty one, write the table, then erase the volume's PEBs.  If the power
 * fails before they are erased, UBI's attach finds PEBs of a volume the
 * table does not have and erases them itself.
 *
 * An empty record is all zeros with a valid CRC: UBI rejects a table where
 * it is anything else.  buf: room for one LEB.
 */
int
ubi_lite_remove(struct ubi_lite *u, uint32_t vol_id, uint8_t *buf)
{
	int error;

	if (vol_id >= UBI_LITE_MAXVOL || u->ul_vol[vol_id].v_reserved_pebs == 0)
		return ENOENT;
	if ((error = vtbl_load(u, buf)) != 0)
		return error;
	uint8_t *rec = buf + vol_id * UBI_VTBL_RECORD_SIZE;
	if (!vtbl_rec_ok(rec) || be32(rec) == 0)
		return EIO;	/* not the table read at attach */
	memset(rec, 0, UBI_VTBL_RECORD_SIZE);
	vtbl_rec_crc(rec);
	if ((error = vtbl_store(u, buf)) != 0)
		return error;
	return erase_volume_pebs(u, vol_id);
}

/*
 * Create a dynamic volume of nlebs LEBs; returns its id in *idp.
 *
 * UBI refuses to attach a device whose volumes reserve more PEBs than it
 * has, after its own reserves: the two layout PEBs, one for
 * wear-leveling, and a pool for replacing bad blocks.  How big that pool
 * is depends on the UBI version (U-Boot 2012.07: 1% of the PEBs, 8 here;
 * Linux since 3.7: 20 per 1024 PEBs of the whole chip, about 20), so
 * this counts the larger, and keeps a margin besides.
 *
 * PEBs that still carry the new id (a removal interrupted before its
 * erases; UBI's attach would have erased them) are erased first, so they
 * do not appear as the new volume's data.
 */
#define UBI_BEB_RESERVE		20
#define UBI_CREATE_MARGIN	8

int
ubi_lite_create(struct ubi_lite *u, const char *name, uint32_t nlebs,
    uint32_t *idp, uint8_t *buf)
{
	const size_t nl = strlen(name);
	uint8_t empty[UBI_VTBL_RECORD_SIZE];
	uint32_t reserved = 0, id = UBI_LITE_MAXVOL;
	int error;

	if (nl == 0 || nl > UBI_VOL_NAME_MAX || nlebs == 0)
		return EINVAL;
	if (ubi_lite_lookup(u, name) >= 0)
		return EEXIST;
	if ((error = vtbl_load(u, buf)) != 0)
		return error;
	memset(empty, 0, sizeof(empty));
	vtbl_rec_crc(empty);
	for (uint32_t i = 0; i < vtbl_slots(u); i++) {
		const uint8_t *rec = buf + i * UBI_VTBL_RECORD_SIZE;

		if (!vtbl_rec_ok(rec))
			return EIO;
		reserved += be32(rec);
		if (id == UBI_LITE_MAXVOL && i < UBI_LITE_MAXVOL &&
		    memcmp(rec, empty, sizeof(empty)) == 0)
			id = i;
	}
	if (id == UBI_LITE_MAXVOL)
		return ENOSPC;
	const uint32_t good = u->ul_npebs - u->ul_nbad - u->ul_ncorrupt;
	const uint32_t need = reserved + nlebs + UBI_LAYOUT_LEBS + 1 +
	    UBI_BEB_RESERVE + UBI_CREATE_MARGIN;
	if (need > good)
		return ENOSPC;
	if ((error = erase_volume_pebs(u, id)) != 0)
		return error;

	uint8_t *rec = buf + id * UBI_VTBL_RECORD_SIZE;
	memset(rec, 0, UBI_VTBL_RECORD_SIZE);
	put_be32(rec, nlebs);			/* reserved_pebs */
	put_be32(rec + 4, 1);			/* alignment */
	put_be32(rec + 8, 0);			/* data_pad */
	rec[12] = 1;				/* dynamic */
	rec[13] = 0;				/* upd_marker */
	rec[14] = nl >> 8;
	rec[15] = nl & 0xff;
	memcpy(rec + 16, name, nl);
	vtbl_rec_crc(rec);
	if ((error = vtbl_store(u, buf)) != 0)
		return error;
	*idp = id;
	return 0;
}

/*
 * Swap the names of two volumes, in one write of the table: there is no
 * moment when both or neither have a name.  This is how a new system is
 * made the one U-Boot boots ("kernel") while the old one is kept.
 */
int
ubi_lite_swap_names(struct ubi_lite *u, uint32_t a, uint32_t b, uint8_t *buf)
{
	uint8_t tmp[2 + UBI_VOL_NAME_MAX + 1];
	int error;

	if (a >= UBI_LITE_MAXVOL || b >= UBI_LITE_MAXVOL || a == b ||
	    u->ul_vol[a].v_reserved_pebs == 0 ||
	    u->ul_vol[b].v_reserved_pebs == 0)
		return EINVAL;
	if ((error = vtbl_load(u, buf)) != 0)
		return error;
	uint8_t *ra = buf + a * UBI_VTBL_RECORD_SIZE;
	uint8_t *rb = buf + b * UBI_VTBL_RECORD_SIZE;
	if (!vtbl_rec_ok(ra) || !vtbl_rec_ok(rb))
		return EIO;
	/* name_len (2 bytes) and name (128) */
	memcpy(tmp, ra + 14, sizeof(tmp));
	memcpy(ra + 14, rb + 14, sizeof(tmp));
	memcpy(rb + 14, tmp, sizeof(tmp));
	vtbl_rec_crc(ra);
	vtbl_rec_crc(rb);
	return vtbl_store(u, buf);
}
