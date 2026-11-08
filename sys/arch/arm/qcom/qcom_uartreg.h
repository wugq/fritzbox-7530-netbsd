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

/* Qualcomm UARTDM v1.4 registers (from Linux msm_serial.c / U-Boot serial_msm.c) */
#ifndef _ARM_QCOM_UARTREG_H
#define _ARM_QCOM_UARTREG_H

#define UARTDM_MR1		0x00
#define UARTDM_MR2		0x04
#define UARTDM_IPR		0x18	/* stale timeout */
#define UARTDM_TFWR		0x1c	/* TX FIFO watermark (words) */
#define UARTDM_RFWR		0x20	/* RX FIFO watermark (words) */
#define UARTDM_DMRX		0x34
#define UARTDM_DMEN		0x3c
#define  UARTDM_DMEN_RX_SC_ENABLE __BIT(5)	/* one character per FIFO word */
#define UARTDM_NCF_TX		0x40
#define UARTDM_RXFS		0x50
#define  UARTDM_RXFS_BUF_SHIFT	7
#define  UARTDM_RXFS_BUF_MASK	0x7
#define UARTDM_CSR		0xa0
#define UARTDM_SR		0xa4
#define  UARTDM_SR_RX_READY	__BIT(0)
#define  UARTDM_SR_RX_FULL	__BIT(1)
#define  UARTDM_SR_TX_READY	__BIT(2)
#define  UARTDM_SR_TX_EMPTY	__BIT(3)
#define  UARTDM_SR_OVERRUN	__BIT(4)
#define UARTDM_CR		0xa8
#define  UARTDM_CR_RX_ENABLE		__BIT(0)
#define  UARTDM_CR_TX_ENABLE		__BIT(2)
#define  UARTDM_CR_CMD_RESET_ERR	(3 << 4)
#define  UARTDM_CR_CMD_RESET_STALE_INT	(8 << 4)
#define  UARTDM_CR_CMD_RESET_TX_READY	(3 << 8)
#define  UARTDM_CR_CMD_FORCE_STALE	(4 << 8)
#define  UARTDM_CR_CMD_STALE_EVENT_ENABLE (5 << 8)
#define UARTDM_MISR		0xac	/* masked interrupt status */
#define UARTDM_IMR		0xb0
#define UARTDM_ISR		0xb4
/* IMR, ISR and MISR bits */
#define  UARTDM_ISR_TXLEV	__BIT(0)	/* TX FIFO at/below TFWR */
#define  UARTDM_ISR_RXSTALE	__BIT(3)
#define  UARTDM_ISR_RXLEV	__BIT(4)	/* RX FIFO above RFWR */
#define UARTDM_RX_TOTAL_SNAP	0xbc
#define  UARTDM_CR_CMD_RESET_RX	(1 << 4)
#define  UARTDM_ISR_TX_READY	__BIT(7)
#define UARTDM_TF		0x100
#define UARTDM_RF		0x140

#endif
