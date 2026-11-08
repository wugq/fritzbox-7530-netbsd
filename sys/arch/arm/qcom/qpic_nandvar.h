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
 * QPIC NAND core (qpic_nand.c): OS-independent.  The OS glue fills in the
 * register accessors and a delay, resets the block between
 * qpic_nand_save() and qpic_nand_restore(), and calls the rest.
 */
#ifndef _ARM_QCOM_QPIC_NANDVAR_H_
#define _ARM_QCOM_QPIC_NANDVAR_H_

#define QPIC_PAGE_SIZE		2048
#define QPIC_PAGES_PER_BLOCK	64
#define QPIC_BLOCK_SIZE		(QPIC_PAGE_SIZE * QPIC_PAGES_PER_BLOCK)
#define QPIC_CW_PER_PAGE	4
#define QPIC_NSAVED		8

#define QPIC_ERASED		(-1)	/* read_page: the page was never written */

struct qpic_nand {
	void		*qn_arg;
	uint32_t	(*qn_read)(void *, uint32_t);
	void		(*qn_write)(void *, uint32_t, uint32_t);
	void		(*qn_delay)(unsigned int);	/* microseconds */
	uint32_t	qn_saved[QPIC_NSAVED];		/* boot loader's config */
	uint32_t	qn_status;	/* FLASH_STATUS of the last failure */
};

void	qpic_nand_save(struct qpic_nand *);
void	qpic_nand_restore(struct qpic_nand *);
int	qpic_nand_read_id(struct qpic_nand *, uint32_t *);
int	qpic_nand_read_page(struct qpic_nand *, uint32_t, uint8_t *, u_int *);
int	qpic_nand_program_page(struct qpic_nand *, uint32_t, const uint8_t *);
int	qpic_nand_erase_block(struct qpic_nand *, uint32_t);
bool	qpic_nand_block_bad(struct qpic_nand *, uint32_t);

#endif /* _ARM_QCOM_QPIC_NANDVAR_H_ */
