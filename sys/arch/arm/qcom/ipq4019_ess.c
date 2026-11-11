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
 * Qualcomm IPQ40xx ESS switch ("qca,ipq4019-qca8337n"), the built-in
 * 6-port switch in front of the EDMA engine (qcomedma):
 *
 *	EDMA (CPU) == port 0 ==+== switch ==+== ports 1-5 == QCA8075 PHYs
 *	                                     (over one PSGMII link)
 *
 * This driver owns everything outside the DMA engine, as FreeBSD's
 * ar40xx(4) does (sys/dev/etherswitch/ar40xx), which it follows:
 *   - the ESS reset (it resets the switch and the EDMA engine), done
 *     without breaking the PSGMII link to the PHY chip;
 *   - the PSGMII self-test: loop frames through every port, reset the
 *     link again if one does not pass;
 *   - setting up the switch from scratch: CPU port, forwarding, VLAN
 *     lookup.  The EDMA driver attaches later and finds a clean engine;
 *   - polling the port link states (log messages, hw.qcomess0.ports);
 *   - hw.qcomess0.wan_port: make one port the WAN (802.1Q VLAN 2);
 *   - hw.qcomess0.isolate: ports 1-5 talk only to the CPU (routing tests;
 *     from attach on with options QCOM_ESS_ISOLATE, see ess_attach);
 *   - the ports' MIB counters as evcnts ("vmstat -e | grep qcomess");
 * Not done (yet): per-port network interfaces, a device-tree or ioctl
 * interface for VLANs.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include "opt_qcom_ess.h"

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/callout.h>
#include <sys/device.h>
#include <sys/evcnt.h>
#include <sys/kmem.h>
#include <sys/mutex.h>
#include <sys/sysctl.h>
#include <sys/syslog.h>
#include <sys/systm.h>

#include <net/if_ether.h>

#include <dev/mii/mii.h>

#include <dev/fdt/fdtvar.h>

#define ESS_NPORTS		6	/* 0 = CPU, 1-5 = PHYs 0-4 */
#define ESS_PORTS_ALL		0x3f
#define ESS_SAVE_SIZE		0x1000	/* U-Boot comparison window */

/* switch registers (ar40xx_reg.h) */
#define SW_MODULE_EN		0x030
#define  MODULE_EN_MIB		__BIT(0)
#define SW_MIB_FUNC		0x034
#define  MIB_BUSY		__BIT(17)
#define  MIB_CPU_KEEP		__BIT(20)	/* reading doesn't clear */
#define  MIB_FUNC		__BITS(26, 24)
#define   MIB_FUNC_FLUSH	1		/* clear all counters */
#define   MIB_FUNC_CAPTURE	3		/* snapshot all counters */
#define SW_MIB(p, off)		(0x1000 + (p) * 0x100 + (off))
#define SW_MAX_FRAME_SIZE	0x078
#define  MAX_FRAME_SIZE_MTU	__BITS(13, 0)
#define SW_PORT_STATUS(p)	(0x07c + (p) * 4)
#define  PORT_SPEED		__BITS(1, 0)	/* 0: 10, 1: 100, 2: 1000 */
#define   PORT_SPEED_1000	2
#define  PORT_TX_EN		__BIT(2)
#define  PORT_RX_EN		__BIT(3)
#define  PORT_TXFLOW		__BIT(4)
#define  PORT_RXFLOW		__BIT(5)
#define  PORT_DUPLEX		__BIT(6)
#define  PORT_TXHALF_FLOW	__BIT(7)
#define  PORT_LINK_UP		__BIT(8)
#define  PORT_AUTO_LINK_EN	__BIT(9)
#define  PORT_FLOW_CONTROL	__BIT(12)
#define SW_PORT_HEADER(p)	(0x09c + (p) * 4)
#define SW_EEE_CTRL		0x100
#define SW_PORT_VLAN0(p)	(0x420 + (p) * 8)
#define  VLAN0_DEF_SVID		__BITS(11, 0)	/* PVID */
#define  VLAN0_DEF_CVID		__BITS(27, 16)
#define SW_PORT_VLAN1(p)	(0x424 + (p) * 8)
#define  VLAN1_PORT_VLAN_PROP	__BIT(6)
#define  VLAN1_OUT_MODE		__BITS(13, 12)
#define   OUT_MODE_UNMOD	0	/* as the VTU entry says */
#define   OUT_MODE_UNTOUCH	3	/* leave tags as they are */
#define SW_VTU_FUNC0		0x610	/* VLAN table entry to load */
#define  VTU_EG_MODE(p)		__BITS(5 + (p) * 2, 4 + (p) * 2)
#define   EG_UNTAG		1
#define   EG_TAG		2
#define   EG_NOT_MEMBER		3
#define  VTU_IVL		__BIT(19)	/* independent VLAN learning */
#define  VTU_VALID		__BIT(20)
#define SW_VTU_FUNC1		0x614
#define  VTU_OP_FLUSH		1
#define  VTU_OP_LOAD		2
#define  VTU_VID		__BITS(27, 16)
#define  VTU_BUSY		__BIT(31)
#define SW_ATU_FUNC		0x60c
#define  ATU_FUNC_OP_FLUSH	0x1
#define  ATU_FUNC_OP_FLUSH_UC	0x5	/* unicast entries of one port */
#define  ATU_FUNC_PORT		__BITS(11, 8)
#define  ATU_FUNC_BUSY		__BIT(31)
#define SW_FWD_CTRL0		0x620
#define  FWD_CTRL0_CPU_PORT_EN	__BIT(10)
#define  FWD_CTRL0_MIRROR_PORT	__BITS(7, 4)	/* 0xf: no mirror port */
#define SW_FWD_CTRL1		0x624	/* flood masks: unicast, multi, broad */
#define SW_PORT_LOOKUP(p)	(0x660 + (p) * 0xc)
#define  LOOKUP_MEMBER		__BITS(6, 0)	/* ports it may send to */
#define  LOOKUP_IN_MODE		__BITS(9, 8)
#define   IN_PORT_ONLY		0	/* ignore VLAN tags */
#define   IN_SECURE		3	/* drop VIDs the port isn't in */
#define  LOOKUP_STATE		__BITS(18, 16)
#define   STATE_FORWARD		4
#define  LOOKUP_LEARN		__BIT(20)
#define  LOOKUP_LOOPBACK		__BIT(21)	/* send frames back out */
#define SW_PORT_FLOWCTRL_THRESH(p) (0x9b0 + (p) * 4)

/* ESS side of the PSGMII link (the "psgmii_phy" register block) */
#define PSGMII_CALIB_STATUS	0x0a0	/* bit 0 = calibrated */
#define PSGMII_MODE_CONTROL	0x1b4
#define PSGMII_TX_CONTROL	0x288
#define PSGMII_CALIB_TRIES	100	/* x 2 ms, as FreeBSD/QSDK */

/*
 * MDIO: the bus to the PHYs.  The IPQ4019's MDIO controller ("qcom,ipq4019-
 * mdio", 0x90000) does one clause 22 access at a time: address register
 * (PHY << 8 | reg), data register, command register with a busy bit.
 * Register usage as FreeBSD's qcom_mdio_ipq4018.c.
 */
#define MDIO_ADDR		0x44
#define MDIO_WRITE		0x48
#define MDIO_READ		0x4c
#define MDIO_CMD		0x50
#define  MDIO_CMD_BUSY		__BIT(16)
#define  MDIO_CMD_START		__BIT(8)
#define  MDIO_CMD_READ		0
#define  MDIO_CMD_WRITE		1

/* MMD (clause 45 register) access through clause 22 registers 13/14 */
#define MII_MMD_CTRL		0x0d
#define MII_MMD_DATA		0x0e
#define  MMD_CTRL_DATA		0x4000	/* next access to 14 is data */

/* the QCA8075 PHY answers for its PSGMII interface at MDIO address 5 */
#define MALIBU_PSGMII_PHY	5

/*
 * PSGMII self-test (ar40xx_hw_psgmii.c).  The QCA8075's ports 0-4 (switch
 * ports 1-5) are at MDIO addresses 0-4; with broadcast on, all of them also
 * take writes to address 0x1f.  Registers as FreeBSD and Linux use them
 * (from Qualcomm's SDK, not in a public data sheet):
 */
#define ESS_NPHYS		5
#define MALIBU_BCAST		0x1f	/* all PHYs, writes only */
#define MII_PHY_SPEC_STATUS	0x11
#define  PHY_SPEC_LINK		__BIT(10)	/* link, real time */
#define MII_PHY_SPEC_CTRL	0x10
#define MALIBU_MMD		7	/* vendor MMD */
#define MMD7_PKTGEN_CTRL	0x8020	/* 0xa000: start the generator */
#define MMD7_PKTGEN_COUNT	0x8021	/* frames to send */
#define MMD7_MDIO_BCAST		0x8028	/* 0x801f: take address 0x1f too */
#define MMD7_PKTCHK_CTRL	0x8029	/* 3: count frames that come back */
#define MMD7_RX_OK_HI		0x802a
#define MMD7_RX_OK_LO		0x802b
#define MMD7_RX_ERR		0x802c
#define MMD7_TX_OK_HI		0x802d
#define MMD7_TX_OK_LO		0x802e
#define MMD7_TX_ERR		0x802f
#define MMD7_PKTGEN_SIZE	0x8062	/* frame length: 0x5e0 = 1504 */
#define PSGMII_TEST_PKTS	0x1000
#define PSGMII_TEST_TRIES	20	/* FreeBSD: 100 */

/*
 * Per-port MIB counters (ar40xx_hw_mib.c).  32-bit counters, except the
 * byte counters (two registers).  The hardware's counters run free; the
 * driver adds the change since the last look to an evcnt every second,
 * so "vmstat -e" shows them (group "qcomess0 port3", ...).
 */
static const struct ess_mib {
	uint16_t	off;
	uint8_t		wide;	/* 64 bit */
	const char	*name;
} ess_mibs[] = {
	{ 0x00, 0, "RxBroad" },		{ 0x04, 0, "RxPause" },
	{ 0x08, 0, "RxMulti" },		{ 0x0c, 0, "RxFcsErr" },
	{ 0x10, 0, "RxAlignErr" },	{ 0x14, 0, "RxRunt" },
	{ 0x18, 0, "RxFragment" },	{ 0x1c, 0, "Rx64Byte" },
	{ 0x20, 0, "Rx128Byte" },	{ 0x24, 0, "Rx256Byte" },
	{ 0x28, 0, "Rx512Byte" },	{ 0x2c, 0, "Rx1024Byte" },
	{ 0x30, 0, "Rx1518Byte" },	{ 0x34, 0, "RxMaxByte" },
	{ 0x38, 0, "RxTooLong" },	{ 0x3c, 1, "RxGoodByte" },
	{ 0x44, 1, "RxBadByte" },	{ 0x4c, 0, "RxOverFlow" },
	{ 0x50, 0, "Filtered" },	{ 0x54, 0, "TxBroad" },
	{ 0x58, 0, "TxPause" },		{ 0x5c, 0, "TxMulti" },
	{ 0x60, 0, "TxUnderRun" },	{ 0x64, 0, "Tx64Byte" },
	{ 0x68, 0, "Tx128Byte" },	{ 0x6c, 0, "Tx256Byte" },
	{ 0x70, 0, "Tx512Byte" },	{ 0x74, 0, "Tx1024Byte" },
	{ 0x78, 0, "Tx1518Byte" },	{ 0x7c, 0, "TxMaxByte" },
	{ 0x80, 0, "TxOverSize" },	{ 0x84, 1, "TxByte" },
	{ 0x8c, 0, "TxCollision" },	{ 0x90, 0, "TxAbortCol" },
	{ 0x94, 0, "TxMultiCol" },	{ 0x98, 0, "TxSingleCol" },
	{ 0x9c, 0, "TxExcDefer" },	{ 0xa0, 0, "TxDefer" },
	{ 0xa4, 0, "TxLateCol" },
};
#define ESS_NMIBS	__arraycount(ess_mibs)

struct ess_softc {
	device_t		sc_dev;
	int			sc_phandle;
	bus_space_tag_t		sc_bst;
	bus_space_handle_t	sc_swh;		/* switch */
	bus_space_handle_t	sc_psgmiih;	/* ESS side of PSGMII */
	bus_space_handle_t	sc_mdioh;	/* MDIO controller */
	bool			sc_mdio_ok;
	kmutex_t		sc_lock;
	callout_t		sc_tick;
	uint32_t		sc_link;	/* bitmap of ports with link */
	int			sc_isolate;
	int			sc_wan;		/* WAN port (802.1Q mode), or 0 */
	struct sysctllog	*sc_sysctllog;
	char			sc_mibgrp[ESS_NPORTS][20];
	struct evcnt		sc_mib_ev[ESS_NPORTS][ESS_NMIBS];
	uint64_t		sc_mib_last[ESS_NPORTS][ESS_NMIBS];
};

#define SW_RD(sc, r)	bus_space_read_4((sc)->sc_bst, (sc)->sc_swh, (r))
#define SW_WR(sc, r, v)	bus_space_write_4((sc)->sc_bst, (sc)->sc_swh, (r), (v))
#define PS_RD(sc, r)	bus_space_read_4((sc)->sc_bst, (sc)->sc_psgmiih, (r))
#define PS_WR(sc, r, v)	bus_space_write_4((sc)->sc_bst, (sc)->sc_psgmiih, (r), (v))

static int	ess_match(device_t, cfdata_t, void *);
static void	ess_atu_flush(struct ess_softc *, int);
static void	ess_attach(device_t, device_t, void *);

static const struct device_compatible_entry compat_data[] = {
	{ .compat = "qca,ipq4019-qca8337n" },
	DEVICE_COMPAT_EOL
};

CFATTACH_DECL_NEW(qcom_ess, sizeof(struct ess_softc),
    ess_match, ess_attach, NULL, NULL);

/* --- MDIO ----------------------------------------------------------- */

static int
ess_mdio_wait(struct ess_softc *sc)
{
	for (u_int i = 0; i < 100; i++) {
		if ((bus_space_read_4(sc->sc_bst, sc->sc_mdioh, MDIO_CMD) &
		    MDIO_CMD_BUSY) == 0)
			return 0;
		delay(10);
	}
	aprint_error_dev(sc->sc_dev, "MDIO timeout\n");
	return ETIMEDOUT;
}

static int
ess_mdio_read(struct ess_softc *sc, u_int phy, u_int reg)
{
	if (ess_mdio_wait(sc) != 0)
		return -1;
	bus_space_write_4(sc->sc_bst, sc->sc_mdioh, MDIO_ADDR,
	    ((phy & 0xff) << 8) | (reg & 0xff));
	bus_space_write_4(sc->sc_bst, sc->sc_mdioh, MDIO_CMD,
	    MDIO_CMD_START | MDIO_CMD_READ);
	if (ess_mdio_wait(sc) != 0)
		return -1;
	return bus_space_read_4(sc->sc_bst, sc->sc_mdioh, MDIO_READ) & 0xffff;
}

static void
ess_mdio_write(struct ess_softc *sc, u_int phy, u_int reg, uint16_t val)
{
	if (ess_mdio_wait(sc) != 0)
		return;
	bus_space_write_4(sc->sc_bst, sc->sc_mdioh, MDIO_ADDR,
	    ((phy & 0xff) << 8) | (reg & 0xff));
	bus_space_write_4(sc->sc_bst, sc->sc_mdioh, MDIO_WRITE, val);
	bus_space_write_4(sc->sc_bst, sc->sc_mdioh, MDIO_CMD,
	    MDIO_CMD_START | MDIO_CMD_WRITE);
	(void)ess_mdio_wait(sc);
}

static int
ess_mmd_read(struct ess_softc *sc, u_int phy, u_int mmd, u_int reg)
{
	ess_mdio_write(sc, phy, MII_MMD_CTRL, mmd);
	ess_mdio_write(sc, phy, MII_MMD_DATA, reg);
	ess_mdio_write(sc, phy, MII_MMD_CTRL, MMD_CTRL_DATA | mmd);
	return ess_mdio_read(sc, phy, MII_MMD_DATA);
}

static void
ess_mmd_write(struct ess_softc *sc, u_int phy, u_int mmd, u_int reg,
    uint16_t val)
{
	ess_mdio_write(sc, phy, MII_MMD_CTRL, mmd);
	ess_mdio_write(sc, phy, MII_MMD_DATA, reg);
	ess_mdio_write(sc, phy, MII_MMD_CTRL, MMD_CTRL_DATA | mmd);
	ess_mdio_write(sc, phy, MII_MMD_DATA, val);
}

/* --- ESS reset ------------------------------------------------------ */

/* Poll the PHY's PSGMII PLL; ms waited, or -1 on timeout. */
static int
ess_psgmii_wait_mmd(struct ess_softc *sc)
{
	for (u_int i = 0; i < PSGMII_CALIB_TRIES; i++) {
		const int v = ess_mmd_read(sc, MALIBU_PSGMII_PHY, 1, 0x28);
		if (v >= 0 && (v & 1))
			return i * 2;
		delay(2000);
	}
	return -1;
}

/* Poll the ESS side's PSGMII calibration; ms waited, or -1 on timeout. */
static int
ess_psgmii_wait_ess(struct ess_softc *sc)
{
	for (u_int i = 0; i < PSGMII_CALIB_TRIES; i++) {
		if (PS_RD(sc, PSGMII_CALIB_STATUS) & 1)
			return i * 2;
		delay(2000);
	}
	return -1;
}

static void
ess_reset_pulse(struct fdtbus_reset *rst)
{
	fdtbus_reset_assert(rst);
	delay(10000);
	fdtbus_reset_deassert(rst);
	delay(10000);	/* inner tables flush: 5-10 ms (ipqess) */
}

/*
 * Reset the ESS without breaking the PSGMII link to the PHY, as FreeBSD's
 * ar40xx_hw_malibu_psgmii_ess_reset() and QSDK U-Boot do:
 *   1. reset the PHY's end of the PSGMII link and wait for its PLL;
 *   2. freeze the PHY's receive clock recovery (CDR);
 *   3. reset the ESS;
 *   4. wait until the ESS end of the link has calibrated;
 *   5. release the PHY's CDR and its "RX 20 bit" mode.
 * Just pulsing the ESS reset left a random port dead in about one boot
 * out of three: link up, but no frames.
 */
static void
ess_reset(struct ess_softc *sc, struct fdtbus_reset *rst)
{
	int pll, cal;

	if (!sc->sc_mdio_ok) {
		aprint_error_dev(sc->sc_dev, "no MDIO: plain ESS reset, "
		    "a port may stay dead\n");
		ess_reset_pulse(rst);
		return;
	}
	ess_mdio_write(sc, MALIBU_PSGMII_PHY, 0x00, 0x005b);	/* RX 20 bit */
	ess_mdio_write(sc, MALIBU_PSGMII_PHY, 0x00, 0x001b);	/* reset */
	ess_mdio_write(sc, MALIBU_PSGMII_PHY, 0x00, 0x005b);	/* release */
	pll = ess_psgmii_wait_mmd(sc);
	ess_mdio_write(sc, MALIBU_PSGMII_PHY, 0x1a, 0x2230);	/* freeze CDR */

	ess_reset_pulse(rst);

	cal = ess_psgmii_wait_ess(sc);
	ess_mdio_write(sc, MALIBU_PSGMII_PHY, 0x1a, 0x3230);	/* release CDR */
	ess_mdio_write(sc, MALIBU_PSGMII_PHY, 0x00, 0x005f);	/* RX 20 bit off */

	aprint_normal_dev(sc->sc_dev, "PSGMII: PHY PLL %s, ESS calibration "
	    "%s\n", pll < 0 ? "TIMEOUT" : "ready",
	    cal < 0 ? "TIMEOUT" : "done");
	aprint_verbose_dev(sc->sc_dev, "PSGMII: PLL after %d ms, "
	    "calibration after %d ms\n", pll, cal);
}

/*
 * PSGMII self-test.  The reset above makes a dead port rarer, not
 * impossible: one boot in many still has a port whose PSGMII lane came
 * back misaligned (link up, no frames; seen on the WAN port on 11-10).
 * So test the link, as FreeBSD's ar40xx_hw_psgmii_self_test() and Linux's
 * ar40xx do, and reset it again until it passes:
 *
 *	PHY packet generator --PSGMII--> switch port (loopback) --PSGMII--+
 *	PHY packet checker   <-------------------------------------------+
 *
 * Each PHY is put in loopback at its copper side (so no cable is needed
 * and nothing goes out of the jack) and sends PSGMII_TEST_PKTS frames;
 * the switch port sends them straight back.  The test passes when every
 * PHY counted them all and no errors.  First each PHY alone, then all at
 * once (that loads the link as traffic does).
 *
 * The test leaves the PHYs reset with auto-negotiation on, so the links
 * come up again about 3 s later, as after power-on.
 */

/* Run the generator on phy (or MALIBU_BCAST: all); bitmap of failed PHYs. */
static uint32_t
ess_psgmii_test_run(struct ess_softc *sc, u_int phy)
{
	const bool all = phy == MALIBU_BCAST;
	uint32_t failed = 0;

	ess_mdio_write(sc, phy, MII_BMCR, BMCR_RESET | BMCR_AUTOEN);
	ess_mdio_write(sc, phy, MII_BMCR, BMCR_LOOP | BMCR_S1000 | BMCR_FDX);
	/* wait for the loopback link: 100 x 8 ms at most, as FreeBSD */
	for (u_int i = 0; i < 100; i++) {
		u_int p, n = all ? ESS_NPHYS : 1;

		for (p = 0; p < n; p++) {
			const int st = ess_mdio_read(sc, all ? p : phy,
			    MII_PHY_SPEC_STATUS);
			if (st < 0 || (st & PHY_SPEC_LINK) == 0)
				break;
		}
		if (p == n)
			break;
		delay(8000);
	}

	ess_mmd_write(sc, phy, MALIBU_MMD, MMD7_PKTCHK_CTRL, 0);
	ess_mmd_write(sc, phy, MALIBU_MMD, MMD7_PKTCHK_CTRL, 3);
	ess_mmd_write(sc, phy, MALIBU_MMD, MMD7_PKTGEN_CTRL, 0xa000);
	/* 4096 frames x 1524 bytes x 8 ns (125 MHz) = 50 ms */
	delay(60000);

	for (u_int p = 0; p < ESS_NPHYS; p++) {
		if (!all && p != phy)
			continue;
		const uint32_t tx = ess_mmd_read(sc, p, MALIBU_MMD,
		    MMD7_TX_OK_LO) | ess_mmd_read(sc, p, MALIBU_MMD,
		    MMD7_TX_OK_HI) << 16;
		const uint32_t txerr = ess_mmd_read(sc, p, MALIBU_MMD,
		    MMD7_TX_ERR);
		if (tx == PSGMII_TEST_PKTS && txerr == 0)
			continue;
		failed |= __BIT(p);
		const uint32_t rx = ess_mmd_read(sc, p, MALIBU_MMD,
		    MMD7_RX_OK_LO) | ess_mmd_read(sc, p, MALIBU_MMD,
		    MMD7_RX_OK_HI) << 16;
		aprint_normal_dev(sc->sc_dev, "PSGMII test (%s): port %u: "
		    "tx %u, %u errors; rx %u, %u errors\n", all ? "all" : "one",
		    p + 1, tx, txerr, rx,
		    ess_mmd_read(sc, p, MALIBU_MMD, MMD7_RX_ERR));
	}
	if (!all)	/* FreeBSD powers each one down again */
		ess_mdio_write(sc, phy, MII_BMCR, BMCR_AUTOEN | BMCR_PDOWN |
		    BMCR_S1000);
	return failed;
}

static void
ess_psgmii_selftest(struct ess_softc *sc, struct fdtbus_reset *rst)
{
	uint32_t failed = 0;
	u_int try;

	if (!sc->sc_mdio_ok)
		return;
	/* PHY 4 is a combo port: its MII registers are the copper side's */
	ess_mdio_write(sc, 4, 0x1f, 0x8500);
	for (u_int p = 0; p < ESS_NPHYS; p++)
		ess_mmd_write(sc, p, MALIBU_MMD, MMD7_MDIO_BCAST, 0x801f);
	ess_mdio_write(sc, MALIBU_BCAST, MII_BMCR, BMCR_AUTOEN | BMCR_PDOWN |
	    BMCR_S1000);
	ess_mmd_write(sc, MALIBU_BCAST, MALIBU_MMD, MMD7_PKTGEN_COUNT,
	    PSGMII_TEST_PKTS);
	ess_mmd_write(sc, MALIBU_BCAST, MALIBU_MMD, MMD7_PKTGEN_SIZE, 0x05e0);
	ess_mdio_write(sc, MALIBU_BCAST, MII_PHY_SPEC_CTRL, 0x6800); /* fix MDI */

	for (try = 0; try < PSGMII_TEST_TRIES; try++) {
		for (u_int p = 1; p <= ESS_NPHYS; p++)
			SW_WR(sc, SW_PORT_LOOKUP(p),
			    SW_RD(sc, SW_PORT_LOOKUP(p)) | LOOKUP_LOOPBACK);
		failed = 0;
		for (u_int p = 0; p < ESS_NPHYS; p++)
			failed |= ess_psgmii_test_run(sc, p);
		failed |= ess_psgmii_test_run(sc, MALIBU_BCAST);
		if (failed == 0)
			break;
		ess_reset(sc, rst);
	}

	/* back to normal: generator off, loopbacks off, PHYs reset with AN */
	ess_mmd_write(sc, MALIBU_BCAST, MALIBU_MMD, MMD7_PKTGEN_COUNT, 0);
	ess_mmd_write(sc, MALIBU_BCAST, MALIBU_MMD, MMD7_PKTCHK_CTRL, 0);
	ess_mmd_write(sc, MALIBU_BCAST, MALIBU_MMD, MMD7_PKTGEN_CTRL, 0);
	ess_mdio_write(sc, MALIBU_BCAST, MII_PHY_SPEC_CTRL, 0x6860);
	ess_mdio_write(sc, MALIBU_BCAST, MII_BMCR, BMCR_RESET | BMCR_AUTOEN |
	    BMCR_S1000);
	for (u_int p = 1; p <= ESS_NPHYS; p++)
		SW_WR(sc, SW_PORT_LOOKUP(p),
		    SW_RD(sc, SW_PORT_LOOKUP(p)) & ~LOOKUP_LOOPBACK);
	for (u_int p = 0; p < ESS_NPHYS; p++)
		ess_mmd_write(sc, p, MALIBU_MMD, MMD7_MDIO_BCAST, 0x001f);
	ess_atu_flush(sc, -1);	/* forget the test frames' address */

	if (failed)
		aprint_error_dev(sc->sc_dev, "PSGMII self-test: still failing "
		    "after %u resets (ports %#x), a port may stay dead\n",
		    try, failed << 1);
	else if (try > 0)
		aprint_normal_dev(sc->sc_dev, "PSGMII self-test: passed after "
		    "%u reset%s\n", try, try == 1 ? "" : "s");
	else
		aprint_normal_dev(sc->sc_dev, "PSGMII self-test: passed\n");
}

/* --- switch set-up -------------------------------------------------- */

static void
ess_atu_flush(struct ess_softc *sc, int port)	/* port < 0: all */
{
	for (u_int i = 0; i < 1000 && (SW_RD(sc, SW_ATU_FUNC) & ATU_FUNC_BUSY);
	    i++)
		delay(10);
	SW_WR(sc, SW_ATU_FUNC, ATU_FUNC_BUSY | (port < 0 ? ATU_FUNC_OP_FLUSH :
	    ATU_FUNC_OP_FLUSH_UC | __SHIFTIN(port, ATU_FUNC_PORT)));
}

static void
ess_vtu_op(struct ess_softc *sc, uint32_t op, uint32_t entry)
{
	for (u_int i = 0; i < 1000 && (SW_RD(sc, SW_VTU_FUNC1) & VTU_BUSY); i++)
		delay(10);
	if (op & VTU_OP_LOAD)
		SW_WR(sc, SW_VTU_FUNC0, entry);
	SW_WR(sc, SW_VTU_FUNC1, op | VTU_BUSY);
}

/* Load one VLAN table entry: member ports, and which of them untagged. */
static void
ess_vtu_load(struct ess_softc *sc, u_int vid, uint32_t ports, uint32_t untag)
{
	uint32_t e = VTU_VALID | VTU_IVL;

	for (u_int p = 0; p < ESS_NPORTS; p++)
		e |= __SHIFTIN((ports & __BIT(p)) == 0 ? EG_NOT_MEMBER :
		    (untag & __BIT(p)) ? EG_UNTAG : EG_TAG, VTU_EG_MODE(p));
	ess_vtu_op(sc, VTU_OP_LOAD | __SHIFTIN(vid, VTU_VID), e);
}

/*
 * Which ports belong together.  Two modes, as FreeBSD's ar40xx:
 *
 * wan_port = 0: port-based VLANs only (what U-Boot does).  Ports 0-5 form
 *   one bridge, tags are left alone.
 * wan_port = N: 802.1Q.  VLAN 1 = CPU and the other ports, all untagged,
 *   so qcomedma0 stays the LAN interface; VLAN 2 = port N untagged and the
 *   CPU tagged, i.e. the WAN is vlan(4) id 2 on qcomedma0.  Ingress
 *   "secure": a port drops frames of VLANs it isn't in.
 *
 * isolate (both modes): ports 1-5 may send only to the CPU port.
 * sc_lock held (or attach).
 */
#define ESS_LAN_VID	1
#define ESS_WAN_VID	2
static void
ess_apply_vlans(struct ess_softc *sc)
{
	const int wan = sc->sc_wan;
	const uint32_t wanbit = wan ? __BIT(wan) : 0;
	const uint32_t lanports = ESS_PORTS_ALL & ~wanbit;

	ess_vtu_op(sc, VTU_OP_FLUSH, 0);
	if (wan) {
		ess_vtu_load(sc, ESS_LAN_VID, lanports, lanports);
		ess_vtu_load(sc, ESS_WAN_VID, __BIT(0) | wanbit, wanbit);
	}
	for (u_int p = 0; p < ESS_NPORTS; p++) {
		uint32_t members, vlan0, vlan1, inmode;

		if (wan == 0) {
			members = ESS_PORTS_ALL;
			vlan0 = 0;
			vlan1 = __SHIFTIN(OUT_MODE_UNTOUCH, VLAN1_OUT_MODE);
			inmode = IN_PORT_ONLY;
		} else {
			const u_int pvid = (p == (u_int)wan) ? ESS_WAN_VID :
			    ESS_LAN_VID;
			members = p == 0 ? ESS_PORTS_ALL :
			    p == (u_int)wan ? __BIT(0) | wanbit : lanports;
			vlan0 = __SHIFTIN(pvid, VLAN0_DEF_SVID) |
			    __SHIFTIN(pvid, VLAN0_DEF_CVID);
			vlan1 = VLAN1_PORT_VLAN_PROP |
			    __SHIFTIN(OUT_MODE_UNMOD, VLAN1_OUT_MODE);
			inmode = IN_SECURE;
		}
		if (sc->sc_isolate && p != 0)
			members &= __BIT(0);
		members &= ~__BIT(p);
		SW_WR(sc, SW_PORT_VLAN0(p), vlan0);
		SW_WR(sc, SW_PORT_VLAN1(p), vlan1);
		SW_WR(sc, SW_PORT_LOOKUP(p), members | LOOKUP_LEARN |
		    __SHIFTIN(inmode, LOOKUP_IN_MODE) |
		    __SHIFTIN(STATE_FORWARD, LOOKUP_STATE));
	}
	ess_atu_flush(sc, -1);
}

/* After the reset and the self-test: as FreeBSD's ar40xx_attach() */
static void
ess_init(struct ess_softc *sc)
{
	/* PSGMII mode on the ESS side (ar40xx_hw_psgmii_set_mac_mode) */
	PS_WR(sc, PSGMII_MODE_CONTROL, 0x2200);
	PS_WR(sc, PSGMII_TX_CONTROL, 0x8380);

	/*
	 * PHY ports: MAC off until the switch sees the link (auto link,
	 * the reset default); no Atheros header on any port.
	 */
	for (u_int p = 1; p < ESS_NPORTS; p++)
		SW_WR(sc, SW_PORT_STATUS(p), PORT_AUTO_LINK_EN |
		    PORT_FLOW_CONTROL | PORT_TXHALF_FLOW);
	for (u_int p = 0; p < ESS_NPORTS; p++)
		SW_WR(sc, SW_PORT_HEADER(p), 0);

	/*
	 * globals (ar40xx_hw_init_globals).  No jumbo frames (the EDMA driver
	 * takes one 2 KB buffer per frame), but room for a VLAN tag.
	 */
	SW_WR(sc, SW_MAX_FRAME_SIZE, (SW_RD(sc, SW_MAX_FRAME_SIZE) &
	    ~MAX_FRAME_SIZE_MTU) | (ETHER_MAX_LEN + ETHER_VLAN_ENCAP_LEN));
	SW_WR(sc, SW_FWD_CTRL0, FWD_CTRL0_CPU_PORT_EN |
	    __SHIFTIN(0xf, FWD_CTRL0_MIRROR_PORT));
	SW_WR(sc, SW_FWD_CTRL1, ESS_PORTS_ALL | ESS_PORTS_ALL << 8 |
	    ESS_PORTS_ALL << 16);	/* flood unknown/multi/broadcast */
	/* without the MIB module ~70% of the frames are lost (found 11-07) */
	SW_WR(sc, SW_MODULE_EN, SW_RD(sc, SW_MODULE_EN) | MODULE_EN_MIB);
	SW_WR(sc, SW_EEE_CTRL, 0);		/* no Energy Efficient Ethernet */
	SW_WR(sc, SW_PORT_FLOWCTRL_THRESH(0), 0x60 << 16 | 0x90);

	ess_apply_vlans(sc);

	/* CPU port: fixed 1000 full duplex with flow control, then MAC on */
	const uint32_t cpu = PORT_TXFLOW | PORT_RXFLOW | PORT_TXHALF_FLOW |
	    PORT_DUPLEX | __SHIFTIN(PORT_SPEED_1000, PORT_SPEED);
	SW_WR(sc, SW_PORT_STATUS(0), cpu);
	delay(20);
	SW_WR(sc, SW_PORT_STATUS(0), cpu | PORT_TX_EN | PORT_RX_EN);
}

/*
 * Learning aid (boot -v): which registers end up different from what
 * U-Boot had set.  "save" was read before the reset.
 */
static void
ess_compare_uboot(struct ess_softc *sc, const uint32_t *save)
{
	u_int n = 0;

	for (bus_size_t r = 0; r < ESS_SAVE_SIZE; r += 4) {
		/* status/command registers, ATU/VTU/ACL indirect access */
		if (r == 0x000 || r == 0x034 || r == 0x03c ||
		    (r >= 0x400 && r <= 0x41c) || (r >= 0x600 && r <= 0x61c))
			continue;
		const uint32_t now = SW_RD(sc, r);
		if (now != save[r / 4]) {
			n++;
			aprint_verbose_dev(sc->sc_dev, "%#05lx: U-Boot %#x, "
			    "now %#x\n", (u_long)r, save[r / 4], now);
		}
	}
	aprint_verbose_dev(sc->sc_dev, "%u registers differ from U-Boot's\n",
	    n);
}

/* --- MIB counters ---------------------------------------------------- */

static void
ess_mib_op(struct ess_softc *sc, u_int op)
{
	uint32_t v = SW_RD(sc, SW_MIB_FUNC);

	SW_WR(sc, SW_MIB_FUNC, (v & ~MIB_FUNC) | __SHIFTIN(op, MIB_FUNC));
	for (u_int i = 0; i < 1000 && (SW_RD(sc, SW_MIB_FUNC) & MIB_BUSY); i++)
		delay(10);
}

static uint64_t
ess_mib_read(struct ess_softc *sc, u_int p, const struct ess_mib *mib)
{
	uint64_t v = SW_RD(sc, SW_MIB(p, mib->off));

	if (mib->wide)
		v |= (uint64_t)SW_RD(sc, SW_MIB(p, mib->off + 4)) << 32;
	return v;
}

/* Snapshot the counters and add what changed to the evcnts; sc_lock held. */
static void
ess_mib_update(struct ess_softc *sc)
{
	ess_mib_op(sc, MIB_FUNC_CAPTURE);
	for (u_int p = 0; p < ESS_NPORTS; p++)
		for (u_int i = 0; i < ESS_NMIBS; i++) {
			const uint64_t v = ess_mib_read(sc, p, &ess_mibs[i]);
			const uint64_t last = sc->sc_mib_last[p][i];

			/* 32-bit counters wrap: subtract modulo 2^32 */
			sc->sc_mib_ev[p][i].ev_count += ess_mibs[i].wide ?
			    v - last : (uint32_t)(v - last);
			sc->sc_mib_last[p][i] = v;
		}
}

static void
ess_mib_attach(struct ess_softc *sc)
{
	/* start from zero; keep the counters when they are read */
	ess_mib_op(sc, MIB_FUNC_FLUSH);
	SW_WR(sc, SW_MIB_FUNC, SW_RD(sc, SW_MIB_FUNC) | MIB_CPU_KEEP);
	for (u_int p = 0; p < ESS_NPORTS; p++) {
		snprintf(sc->sc_mibgrp[p], sizeof(sc->sc_mibgrp[p]),
		    "%s port%u", device_xname(sc->sc_dev), p);
		for (u_int i = 0; i < ESS_NMIBS; i++)
			evcnt_attach_dynamic(&sc->sc_mib_ev[p][i],
			    EVCNT_TYPE_MISC, NULL, sc->sc_mibgrp[p],
			    ess_mibs[i].name);
	}
}

/* --- link polling --------------------------------------------------- */

static const char *
ess_port_str(uint32_t st, char *buf, size_t len)
{
	static const char *const speed[] = { "10", "100", "1000", "?" };

	if ((st & PORT_LINK_UP) == 0)
		return "down";
	snprintf(buf, len, "%s%s", speed[__SHIFTOUT(st, PORT_SPEED)],
	    (st & PORT_DUPLEX) ? "FD" : "HD");
	return buf;
}

static void
ess_tick(void *arg)
{
	struct ess_softc * const sc = arg;
	char buf[16];

	mutex_enter(&sc->sc_lock);
	for (u_int p = 1; p < ESS_NPORTS; p++) {
		const uint32_t st = SW_RD(sc, SW_PORT_STATUS(p));
		const bool up = (st & PORT_LINK_UP) != 0;

		if (up == ((sc->sc_link & __BIT(p)) != 0))
			continue;
		sc->sc_link ^= __BIT(p);
		ess_atu_flush(sc, p);	/* stations may have moved */
		log(LOG_NOTICE, "%s: port %u: link %s%s\n",
		    device_xname(sc->sc_dev), p, up ? "up " : "down",
		    up ? ess_port_str(st, buf, sizeof(buf)) : "");
	}
	ess_mib_update(sc);
	mutex_exit(&sc->sc_lock);
	callout_schedule(&sc->sc_tick, hz);
}

/* --- sysctl --------------------------------------------------------- */

/* hw.qcomess0.ports: "1:down 2:1000FD ..." */
static int
ess_sysctl_ports(SYSCTLFN_ARGS)
{
	struct sysctlnode node = *rnode;
	struct ess_softc * const sc = rnode->sysctl_data;
	char str[80], buf[16];
	size_t o = 0;

	for (u_int p = 1; p < ESS_NPORTS; p++)
		o += snprintf(str + o, sizeof(str) - o, "%s%u:%s",
		    p > 1 ? " " : "", p,
		    ess_port_str(SW_RD(sc, SW_PORT_STATUS(p)), buf, sizeof(buf)));
	node.sysctl_data = str;
	return sysctl_lookup(SYSCTLFN_CALL(&node));
}

static int
ess_sysctl_isolate(SYSCTLFN_ARGS)
{
	struct sysctlnode node = *rnode;
	struct ess_softc * const sc = rnode->sysctl_data;
	int val = sc->sc_isolate, error;

	node.sysctl_data = &val;
	error = sysctl_lookup(SYSCTLFN_CALL(&node));
	if (error || newp == NULL)
		return error;
	mutex_enter(&sc->sc_lock);
	sc->sc_isolate = val != 0;
	ess_apply_vlans(sc);
	mutex_exit(&sc->sc_lock);
	return 0;
}

static int
ess_sysctl_wan(SYSCTLFN_ARGS)
{
	struct sysctlnode node = *rnode;
	struct ess_softc * const sc = rnode->sysctl_data;
	int val = sc->sc_wan, error;

	node.sysctl_data = &val;
	error = sysctl_lookup(SYSCTLFN_CALL(&node));
	if (error || newp == NULL)
		return error;
	if (val < 0 || val >= ESS_NPORTS)
		return EINVAL;
	mutex_enter(&sc->sc_lock);
	sc->sc_wan = val;
	ess_apply_vlans(sc);
	mutex_exit(&sc->sc_lock);
	return 0;
}

static int
ess_sysctl_dump(SYSCTLFN_ARGS)
{
	struct sysctlnode node = *rnode;
	struct ess_softc * const sc = rnode->sysctl_data;
	int val = 0, error;

	node.sysctl_data = &val;
	error = sysctl_lookup(SYSCTLFN_CALL(&node));
	if (error || newp == NULL || val == 0)
		return error;
	printf("%s: FWD_CTRL0 %#x FWD_CTRL1 %#x MODULE_EN %#x MAX_FRAME %#x\n",
	    device_xname(sc->sc_dev), SW_RD(sc, SW_FWD_CTRL0),
	    SW_RD(sc, SW_FWD_CTRL1), SW_RD(sc, SW_MODULE_EN),
	    SW_RD(sc, SW_MAX_FRAME_SIZE));
	for (u_int p = 0; p < ESS_NPORTS; p++)
		printf("%s: port %u: status %#x lookup %#x vlan %#x/%#x\n",
		    device_xname(sc->sc_dev), p, SW_RD(sc, SW_PORT_STATUS(p)),
		    SW_RD(sc, SW_PORT_LOOKUP(p)), SW_RD(sc, SW_PORT_VLAN0(p)),
		    SW_RD(sc, SW_PORT_VLAN1(p)));
	return 0;
}

static void
ess_sysctl_attach(struct ess_softc *sc)
{
	struct sysctllog **log = &sc->sc_sysctllog;
	const struct sysctlnode *node;

	if (sysctl_createv(log, 0, NULL, &node, 0, CTLTYPE_NODE,
	    device_xname(sc->sc_dev), SYSCTL_DESCR("ESS switch"),
	    NULL, 0, NULL, 0, CTL_HW, CTL_CREATE, CTL_EOL) != 0)
		return;
	sysctl_createv(log, 0, &node, NULL, CTLFLAG_READONLY, CTLTYPE_STRING,
	    "ports", SYSCTL_DESCR("link state of ports 1-5"),
	    ess_sysctl_ports, 0, (void *)sc, 0, CTL_CREATE, CTL_EOL);
	sysctl_createv(log, 0, &node, NULL, CTLFLAG_READWRITE, CTLTYPE_INT,
	    "isolate", SYSCTL_DESCR("ports 1-5 talk only to the CPU"),
	    ess_sysctl_isolate, 0, (void *)sc, 0, CTL_CREATE, CTL_EOL);
	sysctl_createv(log, 0, &node, NULL, CTLFLAG_READWRITE, CTLTYPE_INT,
	    "wan_port", SYSCTL_DESCR("0: all ports LAN; N: port N is the WAN, "
	    "VLAN 2 (tagged towards the CPU: vlan(4) id 2 on qcomedma0)"),
	    ess_sysctl_wan, 0, (void *)sc, 0, CTL_CREATE, CTL_EOL);
	sysctl_createv(log, 0, &node, NULL, CTLFLAG_READWRITE, CTLTYPE_INT,
	    "dump", SYSCTL_DESCR("write 1 to print the switch state"),
	    ess_sysctl_dump, 0, (void *)sc, 0, CTL_CREATE, CTL_EOL);
}

/* --- autoconf ------------------------------------------------------- */

static int
ess_match(device_t parent, cfdata_t cf, void *aux)
{
	struct fdt_attach_args * const faa = aux;

	return of_compatible_match(faa->faa_phandle, compat_data);
}

static void
ess_attach(device_t parent, device_t self, void *aux)
{
	struct ess_softc * const sc = device_private(self);
	struct fdt_attach_args * const faa = aux;
	const int phandle = faa->faa_phandle;
	struct fdtbus_reset *rst;
	struct clk *clk;
	bus_addr_t a;
	bus_size_t l;
	uint32_t *save;

	sc->sc_dev = self;
	sc->sc_phandle = phandle;
	sc->sc_bst = faa->faa_bst;
	if (fdtbus_get_reg(phandle, 0, &a, &l) != 0 ||
	    bus_space_map(sc->sc_bst, a, l, 0, &sc->sc_swh) != 0 ||
	    fdtbus_get_reg(phandle, 1, &a, &l) != 0 ||
	    bus_space_map(sc->sc_bst, a, l, 0, &sc->sc_psgmiih) != 0) {
		aprint_error(": couldn't map registers\n");
		return;
	}
	const int mdio = of_find_bycompat(OF_finddevice("/"),
	    "qcom,ipq4019-mdio");
	if (mdio > 0 && fdtbus_get_reg(mdio, 0, &a, &l) == 0 &&
	    bus_space_map(sc->sc_bst, a, l, 0, &sc->sc_mdioh) == 0)
		sc->sc_mdio_ok = true;
	if ((clk = fdtbus_clock_get(phandle, "ess")) == NULL ||
	    clk_enable(clk) != 0) {
		aprint_error(": couldn't enable clock\n");
		return;
	}
	if ((rst = fdtbus_reset_get(phandle, "ess")) == NULL) {
		aprint_error(": no \"ess\" reset\n");
		return;
	}
	mutex_init(&sc->sc_lock, MUTEX_DEFAULT, IPL_NONE);
	callout_init(&sc->sc_tick, CALLOUT_MPSAFE);
	callout_setfunc(&sc->sc_tick, ess_tick, sc);

	aprint_naive("\n");
	aprint_normal(": IPQ40xx ESS switch, %u ports\n", ESS_NPORTS);

	save = kmem_alloc(ESS_SAVE_SIZE, KM_SLEEP);
	bus_space_read_region_4(sc->sc_bst, sc->sc_swh, 0, save,
	    ESS_SAVE_SIZE / 4);
	ess_reset(sc, rst);
	ess_psgmii_selftest(sc, rst);
	fdtbus_reset_put(rst);
#ifdef QCOM_ESS_ISOLATE
	/*
	 * Start with the ports apart: until userland has made one of them
	 * the WAN (hw.qcomess0.wan_port), they would all be one network,
	 * and a provider's modem on the WAN port would be on the LAN while
	 * the router boots (its DHCP and router advertisements reaching
	 * the LAN hosts, past the firewall).  The flash edition's /etc/rc
	 * sets the WAN port first, then isolate=0.
	 */
	sc->sc_isolate = 1;
#endif
	ess_init(sc);
	ess_compare_uboot(sc, save);
	kmem_free(save, ESS_SAVE_SIZE);

	ess_mib_attach(sc);
	ess_sysctl_attach(sc);
	callout_schedule(&sc->sc_tick, hz);
}
