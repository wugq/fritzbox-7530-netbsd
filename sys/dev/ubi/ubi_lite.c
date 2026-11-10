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
 * remove a volume.  No volume creation, no wear-leveling moves, no
 * fastmap.
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
		pe->pe_sqnum = be64(v + 40);
		if (pe->pe_sqnum > u->ul_max_sqnum)
			u->ul_max_sqnum = pe->pe_sqnum;
		u->ul_nused++;
	}
	if (u->ul_vid_offset == 0)
		return ENOENT;			/* no UBI here */
	u->ul_leb_size = u->ul_pages_per_peb * u->ul_page_size -
	    u->ul_data_offset;
	return ubi_lite_read_vtbl(u);
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

/* The volume table: from the layout volume's LEB 0 (or LEB 1). */
int
ubi_lite_read_vtbl(struct ubi_lite *u)
{
	uint8_t rec[UBI_VTBL_RECORD_SIZE];
	int found = 0;

	memset(u->ul_vol, 0, sizeof(u->ul_vol));
	for (uint32_t copy = 0; copy < 2 && !found; copy++) {
		if (ubi_lite_find(u, UBI_LAYOUT_VOLUME_ID, copy) < 0)
			continue;
		for (uint32_t i = 0; i < UBI_LITE_MAXVOL; i++) {
			struct ubi_vol *vol = &u->ul_vol[i];

			if (ubi_lite_read_leb(u, UBI_LAYOUT_VOLUME_ID, copy,
			    i * UBI_VTBL_RECORD_SIZE, rec, sizeof(rec)) != 0)
				break;
			if (ubi_crc32(UBI_CRC32_INIT, rec, sizeof(rec) - 4) !=
			    be32(rec + sizeof(rec) - 4))
				continue;
			vol->v_reserved_pebs = be32(rec);
			if (vol->v_reserved_pebs == 0)
				continue;
			vol->v_type = rec[12];
			const u_int nl = MIN((uint32_t)(rec[14] << 8 | rec[15]),
			    sizeof(vol->v_name) - 1);
			memcpy(vol->v_name, rec + 16, nl);
			vol->v_name[nl] = '\0';
			found = 1;
		}
	}
	return found ? 0 : ENOENT;
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
	pe->pe_copy_flag = 0;
	pe->pe_vol_id = vol_id;
	pe->pe_lnum = lnum;
	pe->pe_data_size = 0;
	pe->pe_sqnum = u->ul_max_sqnum;

	/* the old copy goes back to the free pool */
	if (old >= 0)
		(void)ubi_erase_peb(u, old);
	return 0;
}

/*
 * Remove a volume, as Linux' ubirmvol does: clear its record in the
 * volume table, write the table into both copies of the layout volume
 * (copy 0 first: at attach, UBI trusts copy 0 and repairs copy 1 from
 * it), then erase the volume's PEBs.  If the power fails before they are
 * erased, UBI's attach finds PEBs of a volume the table does not have and
 * erases them itself.
 *
 * buf: room for one LEB.  The table is written whole, every record as
 * read, so records we do not otherwise look at (ids past UBI_LITE_MAXVOL)
 * are kept.  An unused record is all zeros with a valid CRC: UBI rejects
 * a table where it is anything else.
 */
int
ubi_lite_remove(struct ubi_lite *u, uint32_t vol_id, uint8_t *buf)
{
	const uint32_t slots = MIN(u->ul_leb_size / UBI_VTBL_RECORD_SIZE,
	    UBI_MAX_VOLUMES);
	const uint32_t size = roundup(slots * UBI_VTBL_RECORD_SIZE,
	    u->ul_page_size);
	uint32_t copy, nerr = 0;
	int error;

	if (vol_id >= UBI_LITE_MAXVOL || u->ul_vol[vol_id].v_reserved_pebs == 0)
		return ENOENT;
	for (copy = 0; copy < UBI_LAYOUT_LEBS; copy++)
		if (ubi_lite_find(u, UBI_LAYOUT_VOLUME_ID, copy) >= 0)
			break;
	if (copy == UBI_LAYOUT_LEBS)
		return ENOENT;
	error = ubi_lite_read_leb(u, UBI_LAYOUT_VOLUME_ID, copy, 0, buf, size);
	if (error != 0)
		return error;

	uint8_t *rec = buf + vol_id * UBI_VTBL_RECORD_SIZE;
	if (ubi_crc32(UBI_CRC32_INIT, rec, UBI_VTBL_RECORD_SIZE - 4) !=
	    be32(rec + UBI_VTBL_RECORD_SIZE - 4) || be32(rec) == 0)
		return EIO;	/* not the table read at attach */
	memset(rec, 0, UBI_VTBL_RECORD_SIZE);
	put_be32(rec + UBI_VTBL_RECORD_SIZE - 4,
	    ubi_crc32(UBI_CRC32_INIT, rec, UBI_VTBL_RECORD_SIZE - 4));

	for (copy = 0; copy < UBI_LAYOUT_LEBS; copy++) {
		error = ubi_write_leb(u, UBI_LAYOUT_VOLUME_ID, copy,
		    UBI_COMPAT_REJECT, buf, size);
		if (error != 0)
			return error;
	}
	u->ul_vol[vol_id].v_reserved_pebs = 0;

	for (uint32_t p = 0; p < u->ul_npebs; p++) {
		const struct ubi_peb *pe = &u->ul_peb[p];

		if (pe->pe_state == UBI_PEB_USED && pe->pe_vol_id == vol_id &&
		    ubi_erase_peb(u, p) != 0)
			nerr++;
	}
	return nerr == 0 ? 0 : EIO;
}
