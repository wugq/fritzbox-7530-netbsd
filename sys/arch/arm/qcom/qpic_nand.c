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
 * Qualcomm QPIC NAND controller, version 1.4 (IPQ40xx), driven by the CPU
 * through its registers -- no BAM DMA.  The boot loader leaves the block
 * in BAM mode, where the CPU may only read the "operational" registers;
 * after a reset of the block (by the OS glue) BAM mode is off and plain
 * register writes work.  Each operation: program the command, address
 * and configuration registers, write EXEC_CMD, poll FLASH_STATUS; page
 * data passes through the 528-byte buffer at FLASH_BUF_ACC one codeword
 * at a time.
 *
 * The page layout is the one U-Boot and Linux use (Linux' qcom_nandc.c,
 * read for reference only), with 4-bit BCH ECC per 512-byte step: a 2 KB
 * page is four codewords holding 516, 516, 516 and 500 bytes of data;
 * the last one also has 16 free spare bytes.  The ECC configuration is
 * the boot loader's, saved before the reset (qpic_nand_save()).
 *
 * This file knows nothing about the OS: registers go through qn_read /
 * qn_write, delays through qn_delay (see qpic_nandvar.h).
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>

#include <arm/qcom/qpic_nandvar.h>

#define NAND_FLASH_CMD		0x000
#define  OP_PAGE_READ		0x2
#define  OP_PAGE_READ_WITH_ECC	0x3
#define  OP_PROGRAM_PAGE	0x6
#define  OP_BLOCK_ERASE		0xa
#define  OP_FETCH_ID		0xb
#define  PAGE_ACC		__BIT(4)
#define  LAST_PAGE		__BIT(5)
#define NAND_ADDR0		0x004
#define NAND_ADDR1		0x008
#define NAND_FLASH_CHIP_SELECT	0x00c
#define NAND_EXEC_CMD		0x010
#define NAND_FLASH_STATUS	0x014
#define  FS_OPER_STATUS		__BITS(3, 0)
#define  FS_OP_ERR		__BIT(4)
#define  FS_READY_BSY_N		__BIT(5)
#define  FS_MPU_ERR		__BIT(8)	/* write protected */
#define  FS_DEVICE_STS_ERR	__BIT(16)	/* the chip reported a failure */
#define NAND_BUFFER_STATUS	0x018
#define  BS_UNCORRECTABLE	__BIT(8)
#define  BS_CORRECTED		__BITS(4, 0)
#define NAND_DEV0_CFG0		0x020
#define  CFG0_CW_PER_PAGE	__BITS(8, 6)	/* codewords - 1 */
#define NAND_DEV0_CFG1		0x024
#define NAND_DEV0_ECC_CFG	0x028
#define  ECC_CFG_ECC_DISABLE	__BIT(0)
#define NAND_READ_ID		0x040
#define NAND_READ_STATUS	0x044
#define NAND_DEV_CMD0		0x0a0
#define NAND_DEV_CMD1		0x0a4
#define NAND_DEV_CMD2		0x0a8
#define NAND_DEV_CMD_VLD	0x0ac
#define NAND_ERASED_CW_DETECT_CFG 0x0e8
#define  ERASED_DET_CLR		0x3	/* reset detection, mask ECC */
#define  ERASED_DET_SET		0x2	/* active, mask ECC */
#define NAND_ERASED_CW_DETECT_STATUS 0x0ec
#define  CODEWORD_ERASED	(__BIT(6) | __BIT(4))
#define NAND_EBI2_ECC_BUF_CFG	0x0f0
#define FLASH_BUF_ACC		0x100

/*
 * Raw access (no ECC): whole 528-byte codewords, as Linux' cfg0_raw /
 * cfg1_raw: codeword size 528, 5 address cycles; recovery cycles 7, bad
 * block byte 17 in the spare area, ECC off.
 */
#define CFG0_RAW		(528 << 9 | 5 << 27)
#define CFG1_RAW		(7 << 2 | 17 << 6 | 1 << 16 | 2 << 17 | 1 << 0)

#define CW_SIZE			528
#define CW_DATA			516
#define BBM_OFFSET		(2048 - CW_SIZE * 3)	/* in the last raw cw */

static const uint32_t qpic_saved_regs[QPIC_NSAVED] = {
	NAND_DEV0_CFG0, NAND_DEV0_CFG1, NAND_DEV0_ECC_CFG,
	NAND_EBI2_ECC_BUF_CFG, NAND_DEV_CMD0, NAND_DEV_CMD1, NAND_DEV_CMD2,
	NAND_DEV_CMD_VLD,
};
enum { S_CFG0, S_CFG1, S_ECC_CFG, S_ECC_BUF };

#define RD(qn, r)	(qn)->qn_read((qn)->qn_arg, (r))
#define WR(qn, r, v)	(qn)->qn_write((qn)->qn_arg, (r), (v))

void
qpic_nand_save(struct qpic_nand *qn)
{
	for (u_int i = 0; i < QPIC_NSAVED; i++)
		qn->qn_saved[i] = RD(qn, qpic_saved_regs[i]);
}

void
qpic_nand_restore(struct qpic_nand *qn)
{
	for (u_int i = 0; i < QPIC_NSAVED; i++)
		WR(qn, qpic_saved_regs[i], qn->qn_saved[i]);
	WR(qn, NAND_FLASH_CHIP_SELECT, 0);
}

/* EXEC_CMD and wait until the operation is over; returns FLASH_STATUS */
static int
qpic_exec(struct qpic_nand *qn, uint32_t *stp, u_int timeout_us)
{
	uint32_t st = 0;

	WR(qn, NAND_EXEC_CMD, 1);
	for (u_int i = 0; i < timeout_us; i++) {
		st = RD(qn, NAND_FLASH_STATUS);
		if (__SHIFTOUT(st, FS_OPER_STATUS) == 0 && (st & FS_READY_BSY_N))
			break;
		qn->qn_delay(1);
	}
	*stp = st;
	qn->qn_status = st;
	if (__SHIFTOUT(st, FS_OPER_STATUS) != 0 || !(st & FS_READY_BSY_N))
		return ETIMEDOUT;
	return 0;
}

static void
qpic_clear_status(struct qpic_nand *qn)
{
	WR(qn, NAND_FLASH_STATUS, FS_READY_BSY_N);
	WR(qn, NAND_READ_STATUS, 0xc0);
}

int
qpic_nand_read_id(struct qpic_nand *qn, uint32_t *id)
{
	uint32_t st;
	int error;

	WR(qn, NAND_FLASH_CMD, OP_FETCH_ID);
	WR(qn, NAND_ADDR0, 0);
	WR(qn, NAND_ADDR1, 0);
	if ((error = qpic_exec(qn, &st, 1000)) != 0)
		return error;
	*id = RD(qn, NAND_READ_ID);
	return 0;
}

static void
qpic_page_addr(struct qpic_nand *qn, uint32_t page)
{
	/* column 0 in the low 16 bits, then the row (page) */
	WR(qn, NAND_ADDR0, page << 16);
	WR(qn, NAND_ADDR1, page >> 16);
}

static void
qpic_ecc_config(struct qpic_nand *qn)
{
	WR(qn, NAND_DEV0_CFG0, qn->qn_saved[S_CFG0]);	/* 4 codewords */
	WR(qn, NAND_DEV0_CFG1, qn->qn_saved[S_CFG1]);
	WR(qn, NAND_DEV0_ECC_CFG, qn->qn_saved[S_ECC_CFG]);
	WR(qn, NAND_EBI2_ECC_BUF_CFG, qn->qn_saved[S_ECC_BUF]);
}

static void
qpic_buf_in(struct qpic_nand *qn, uint8_t *dst, u_int n)
{
	for (u_int i = 0; i < n; i += 4) {
		const uint32_t w = RD(qn, FLASH_BUF_ACC + i);
		memcpy(dst + i, &w, MIN(4, n - i));
	}
}

/* n bytes from src into the codeword buffer at offset at (4-aligned) */
static void
qpic_buf_out(struct qpic_nand *qn, u_int at, const uint8_t *src, u_int n)
{
	for (u_int i = 0; i < n; i += 4) {
		uint32_t w = 0xffffffff;
		memcpy(&w, src + i, MIN(4, n - i));
		WR(qn, FLASH_BUF_ACC + at + i, w);
	}
}

/*
 * Read one page with ECC into buf (QPIC_PAGE_SIZE bytes).  Returns 0, or
 * QPIC_ERASED for a never-written page (buf is all 0xff), EIO for an
 * uncorrectable error.  *corrected (if not NULL) counts the bits fixed.
 */
int
qpic_nand_read_page(struct qpic_nand *qn, uint32_t page, uint8_t *buf,
    u_int *corrected)
{
	u_int off = 0, fixed = 0, erased = 0;
	uint32_t st, bs;
	int error = 0;

	qpic_page_addr(qn, page);
	qpic_ecc_config(qn);
	WR(qn, NAND_ERASED_CW_DETECT_CFG, ERASED_DET_CLR);
	WR(qn, NAND_ERASED_CW_DETECT_CFG, ERASED_DET_SET);
	for (u_int cw = 0; cw < QPIC_CW_PER_PAGE; cw++) {
		const u_int n = cw < QPIC_CW_PER_PAGE - 1 ? CW_DATA :
		    QPIC_PAGE_SIZE - CW_DATA * (QPIC_CW_PER_PAGE - 1);

		WR(qn, NAND_FLASH_CMD, OP_PAGE_READ_WITH_ECC | PAGE_ACC |
		    LAST_PAGE);
		if (qpic_exec(qn, &st, 1000) != 0)
			return ETIMEDOUT;
		bs = RD(qn, NAND_BUFFER_STATUS);
		if ((st & FS_OP_ERR) || (bs & BS_UNCORRECTABLE)) {
			/* an erased codeword has no valid ECC */
			if (RD(qn, NAND_ERASED_CW_DETECT_STATUS) &
			    CODEWORD_ERASED) {
				memset(buf + off, 0xff, n);
				erased++;
				off += n;
				continue;
			}
			error = EIO;
		}
		fixed += __SHIFTOUT(bs, BS_CORRECTED);
		qpic_buf_in(qn, buf + off, n);
		off += n;
	}
	qpic_clear_status(qn);
	if (corrected != NULL)
		*corrected = fixed;
	if (error == 0 && erased == QPIC_CW_PER_PAGE)
		return QPIC_ERASED;
	return error;
}

/* Program one page with ECC from buf (QPIC_PAGE_SIZE bytes). */
int
qpic_nand_program_page(struct qpic_nand *qn, uint32_t page,
    const uint8_t *buf)
{
	static const uint8_t ff[16] = {
	    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
	u_int off = 0;
	uint32_t st;

	qpic_page_addr(qn, page);
	qpic_ecc_config(qn);
	for (u_int cw = 0; cw < QPIC_CW_PER_PAGE; cw++) {
		const u_int n = cw < QPIC_CW_PER_PAGE - 1 ? CW_DATA :
		    QPIC_PAGE_SIZE - CW_DATA * (QPIC_CW_PER_PAGE - 1);

		qpic_buf_out(qn, 0, buf + off, n);
		if (n < CW_DATA)	/* the last one: then 16 free spare bytes */
			qpic_buf_out(qn, n, ff, CW_DATA - n);
		WR(qn, NAND_FLASH_CMD, OP_PROGRAM_PAGE | PAGE_ACC | LAST_PAGE);
		if (qpic_exec(qn, &st, 5000) != 0)
			return ETIMEDOUT;
		if (st & (FS_OP_ERR | FS_MPU_ERR))
			return EIO;
		off += n;
	}
	qpic_clear_status(qn);
	return (st & FS_DEVICE_STS_ERR) ? EIO : 0;
}

/* Erase one block (QPIC_PAGES_PER_BLOCK pages). */
int
qpic_nand_erase_block(struct qpic_nand *qn, uint32_t block)
{
	uint32_t st;
	int error;

	WR(qn, NAND_FLASH_CMD, OP_BLOCK_ERASE | PAGE_ACC | LAST_PAGE);
	WR(qn, NAND_ADDR0, block * QPIC_PAGES_PER_BLOCK);	/* row only */
	WR(qn, NAND_ADDR1, 0);
	WR(qn, NAND_DEV0_CFG0, CFG0_RAW & ~CFG0_CW_PER_PAGE);
	WR(qn, NAND_DEV0_CFG1, CFG1_RAW);
	error = qpic_exec(qn, &st, 20000);
	qpic_clear_status(qn);
	if (error != 0)
		return error;
	return (st & (FS_OP_ERR | FS_MPU_ERR | FS_DEVICE_STS_ERR)) ? EIO : 0;
}

/*
 * Factory bad block marker: the first spare byte of the block's first
 * page, which in QPIC's codeword layout lands inside the last codeword
 * (raw offset BBM_OFFSET).  Read that codeword without ECC.
 */
bool
qpic_nand_block_bad(struct qpic_nand *qn, uint32_t block)
{
	uint8_t cw[CW_SIZE];
	uint32_t st;

	qpic_page_addr(qn, block * QPIC_PAGES_PER_BLOCK);
	WR(qn, NAND_DEV0_CFG0, CFG0_RAW | __SHIFTIN(QPIC_CW_PER_PAGE - 1,
	    CFG0_CW_PER_PAGE));
	WR(qn, NAND_DEV0_CFG1, CFG1_RAW);
	WR(qn, NAND_DEV0_ECC_CFG, ECC_CFG_ECC_DISABLE);
	for (u_int c = 0; c < QPIC_CW_PER_PAGE; c++) {
		WR(qn, NAND_FLASH_CMD, OP_PAGE_READ | PAGE_ACC | LAST_PAGE);
		if (qpic_exec(qn, &st, 1000) != 0 || (st & FS_OP_ERR)) {
			qpic_clear_status(qn);
			return true;
		}
		if (c == QPIC_CW_PER_PAGE - 1)
			qpic_buf_in(qn, cw, sizeof(cw));
	}
	qpic_clear_status(qn);
	return cw[BBM_OFFSET] != 0xff;
}
