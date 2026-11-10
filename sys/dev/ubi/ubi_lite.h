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

/* ubi_lite: OS-independent UBI subset (ubi_lite.c) */
#ifndef _DEV_UBI_UBI_LITE_H_
#define _DEV_UBI_UBI_LITE_H_

#define UBI_LITE_MAXVOL		32	/* volume table records we look at */
#define UBI_LAYOUT_VOLUME_ID	0x7fffefff
#define UBI_READ_ERASED		(-1)	/* ul_read_page: never written */

enum { UBI_PEB_EMPTY = 0, UBI_PEB_FREE, UBI_PEB_USED, UBI_PEB_BAD,
    UBI_PEB_CORRUPT };

struct ubi_peb {
	uint64_t	pe_ec;		/* erase counter */
	uint64_t	pe_sqnum;
	uint32_t	pe_vol_id;
	uint32_t	pe_lnum;
	uint32_t	pe_data_size;
	uint8_t		pe_state;
	uint8_t		pe_vol_type;
	uint8_t		pe_copy_flag;
};

struct ubi_vol {
	uint32_t	v_reserved_pebs;	/* 0: no such volume */
	uint8_t		v_type;			/* 1 dynamic, 2 static */
	char		v_name[64];
};

struct ubi_lite {
	/* filled in by the caller */
	void		*ul_arg;
	int		(*ul_read_page)(void *, uint32_t, uint8_t *);
	int		(*ul_program_page)(void *, uint32_t, const uint8_t *);
	int		(*ul_erase_block)(void *, uint32_t);
	bool		(*ul_block_bad)(void *, uint32_t);
	uint32_t	ul_first_peb;		/* in flash blocks */
	uint32_t	ul_npebs;
	uint32_t	ul_page_size;
	uint32_t	ul_pages_per_peb;
	struct ubi_peb	*ul_peb;		/* ul_npebs entries */
	uint8_t		*ul_buf;		/* one page */
	/* found by ubi_lite_scan() */
	uint32_t	ul_vid_offset, ul_data_offset, ul_leb_size;
	uint32_t	ul_image_seq;
	uint64_t	ul_max_sqnum;
	uint32_t	ul_nbad, ul_nfree, ul_nused, ul_ncorrupt;
	struct ubi_vol	ul_vol[UBI_LITE_MAXVOL];
	/* details of the last failed write, for the OS to log */
	uint32_t	ul_fail_peb, ul_fail_off;
	int		ul_fail_error;
	uint8_t		ul_fail_want, ul_fail_got;
};

uint32_t ubi_crc32(uint32_t, const uint8_t *, size_t);
int	ubi_lite_scan(struct ubi_lite *);
int	ubi_lite_read_vtbl(struct ubi_lite *);
int	ubi_lite_find(struct ubi_lite *, uint32_t, uint32_t);
int	ubi_lite_read_leb(struct ubi_lite *, uint32_t, uint32_t, uint32_t,
	    uint8_t *, uint32_t);
int	ubi_lite_lookup(struct ubi_lite *, const char *);
uint32_t ubi_lite_mapped(struct ubi_lite *, uint32_t);
int	ubi_lite_write_leb(struct ubi_lite *, uint32_t, uint32_t,
	    const uint8_t *, uint32_t);
int	ubi_lite_remove(struct ubi_lite *, uint32_t, uint8_t *);

#endif /* _DEV_UBI_UBI_LITE_H_ */
