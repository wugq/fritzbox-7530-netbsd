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
 * Qualcomm IPQ40xx Global Clock Controller ("qcom,gcc-ipq4019"), partial.
 *
 * The GCC holds the SoC's clock tree (PLLs, dividers, muxes, gates) and the
 * block resets.  Other drivers find it through the device tree:
 *   resets = <&gcc ESS_RESET>;     -> fdtbus_reset_get()
 *   clocks = <&gcc GCC_ESS_CLK>;   -> fdtbus_clock_get()
 *
 * What this driver does:
 *  - resets: all of them.  Each is one bit in a GCC register; asserting
 *    sets it.  Table from FreeBSD's qcom_gcc_ipq4018_reset.c.
 *  - clocks: only the "branch" clocks, the on/off gates at the leaves of
 *    the tree, which is what peripheral drivers ask for (enable the clock
 *    of my block).  Each has an enable bit, either in its own control
 *    register (CBCR) or in a shared "vote" register (when the CPU shares
 *    the clock with other masters such as TrustZone, each votes and the
 *    clock runs while anyone does), and a CLK_OFF status bit (31) in its
 *    CBCR.  Table from FreeBSD's qcom_gcc_ipq4018_clock.c.
 *    Rates are not modelled (get_rate returns 0) and the PLLs, dividers
 *    and muxes are left as the firmware programmed them, so CPU frequency
 *    scaling (which needs the APSS clock) is not possible yet.
 *    disable is a no-op: without reference counts across the consumers
 *    and the firmware, turning a gate off is riskier than leaving it on.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/device.h>
#include <sys/kmem.h>
#include <sys/mutex.h>
#include <sys/systm.h>

#include <machine/cpu_counter.h>

#include <dev/clk/clk_backend.h>
#include <dev/fdt/fdtvar.h>

#define CBCR_CLK_OFF	__BIT(31)

struct qcom_gcc_reset {
	uint32_t	reg;
	uint8_t		bit;
};

struct qcom_gcc_branch {
	u_int		id;		/* dt-bindings clock number */
	const char	*name;
	uint32_t	enable_reg;	/* CBCR, or the vote register */
	uint8_t		enable_bit;
	uint32_t	halt_reg;	/* CBCR with the CLK_OFF bit */
	bool		voted;
};

/*
 * Reset number (dt-bindings/clock/qcom,gcc-ipq4019.h) -> register, bit.
 * Generated from FreeBSD's sys/dev/qcom_gcc/qcom_gcc_ipq4018_reset.c.
 */
static const struct qcom_gcc_reset qcom_gcc_resets[] = {
	[ 0] = { 0x1f008,  5 },	/* WIFI0_CPU_INIT_RESET */
	[ 1] = { 0x1f008,  4 },	/* WIFI0_RADIO_SRIF_RESET */
	[ 2] = { 0x1f008,  3 },	/* WIFI0_RADIO_WARM_RESET */
	[ 3] = { 0x1f008,  2 },	/* WIFI0_RADIO_COLD_RESET */
	[ 4] = { 0x1f008,  1 },	/* WIFI0_CORE_WARM_RESET */
	[ 5] = { 0x1f008,  0 },	/* WIFI0_CORE_COLD_RESET */
	[ 6] = { 0x20008,  5 },	/* WIFI1_CPU_INIT_RESET */
	[ 7] = { 0x20008,  4 },	/* WIFI1_RADIO_SRIF_RESET */
	[ 8] = { 0x20008,  3 },	/* WIFI1_RADIO_WARM_RESET */
	[ 9] = { 0x20008,  2 },	/* WIFI1_RADIO_COLD_RESET */
	[10] = { 0x20008,  1 },	/* WIFI1_CORE_WARM_RESET */
	[11] = { 0x20008,  0 },	/* WIFI1_CORE_COLD_RESET */
	[12] = { 0x1e038,  5 },	/* USB3_UNIPHY_PHY_ARES */
	[13] = { 0x1e038,  4 },	/* USB3_HSPHY_POR_ARES */
	[14] = { 0x1e038,  2 },	/* USB3_HSPHY_S_ARES */
	[15] = { 0x1e01c,  4 },	/* USB2_HSPHY_POR_ARES */
	[16] = { 0x1e01c,  2 },	/* USB2_HSPHY_S_ARES */
	[17] = { 0x1d010, 11 },	/* PCIE_PHY_AHB_ARES */
	[18] = { 0x1d010, 10 },	/* PCIE_AHB_ARES */
	[19] = { 0x1d010,  9 },	/* PCIE_PWR_ARES */
	[20] = { 0x1d010,  8 },	/* PCIE_PIPE_STICKY_ARES */
	[21] = { 0x1d010,  7 },	/* PCIE_AXI_M_STICKY_ARES */
	[22] = { 0x1d010,  6 },	/* PCIE_PHY_ARES */
	[23] = { 0x1d010,  5 },	/* PCIE_PARF_XPU_ARES */
	[24] = { 0x1d010,  4 },	/* PCIE_AXI_S_XPU_ARES */
	[25] = { 0x1d010,  3 },	/* PCIE_AXI_M_VMIDMT_ARES */
	[26] = { 0x1d010,  2 },	/* PCIE_PIPE_ARES */
	[27] = { 0x1d010,  1 },	/* PCIE_AXI_S_ARES */
	[28] = { 0x1d010,  0 },	/* PCIE_AXI_M_ARES */
	[29] = { 0x12008,  0 },	/* ESS_RESET */
	[30] = { 0x01000,  0 },	/* GCC_BLSP1_BCR */
	[31] = { 0x02000,  0 },	/* GCC_BLSP1_QUP1_BCR */
	[32] = { 0x02038,  0 },	/* GCC_BLSP1_UART1_BCR */
	[33] = { 0x03008,  0 },	/* GCC_BLSP1_QUP2_BCR */
	[34] = { 0x03028,  0 },	/* GCC_BLSP1_UART2_BCR */
	[35] = { 0x04000,  0 },	/* GCC_BIMC_BCR */
	[36] = { 0x05000,  0 },	/* GCC_TLMM_BCR */
	[37] = { 0x0e000,  0 },	/* GCC_IMEM_BCR */
	[38] = { 0x12008,  0 },	/* GCC_ESS_BCR */
	[39] = { 0x13000,  0 },	/* GCC_PRNG_BCR */
	[40] = { 0x13008,  0 },	/* GCC_BOOT_ROM_BCR */
	[41] = { 0x16000,  0 },	/* GCC_CRYPTO_BCR */
	[42] = { 0x18000,  0 },	/* GCC_SDCC1_BCR */
	[43] = { 0x1a000,  0 },	/* GCC_SEC_CTRL_BCR */
	[44] = { 0x1b008,  0 },	/* GCC_AUDIO_BCR */
	[45] = { 0x1c000,  0 },	/* GCC_QPIC_BCR */
	[46] = { 0x1d000,  0 },	/* GCC_PCIE_BCR */
	[47] = { 0x1e008,  0 },	/* GCC_USB2_BCR */
	[48] = { 0x1e018,  0 },	/* GCC_USB2_PHY_BCR */
	[49] = { 0x1e024,  0 },	/* GCC_USB3_BCR */
	[50] = { 0x1e034,  0 },	/* GCC_USB3_PHY_BCR */
	[51] = { 0x21000,  0 },	/* GCC_SYSTEM_NOC_BCR */
	[52] = { 0x2102c,  0 },	/* GCC_PCNOC_BCR */
	[53] = { 0x21038,  0 },	/* GCC_DCD_BCR */
	[54] = { 0x21064,  0 },	/* GCC_SNOC_BUS_TIMEOUT0_BCR */
	[55] = { 0x2106c,  0 },	/* GCC_SNOC_BUS_TIMEOUT1_BCR */
	[56] = { 0x21074,  0 },	/* GCC_SNOC_BUS_TIMEOUT2_BCR */
	[57] = { 0x2107c,  0 },	/* GCC_SNOC_BUS_TIMEOUT3_BCR */
	[58] = { 0x21084,  0 },	/* GCC_PCNOC_BUS_TIMEOUT0_BCR */
	[59] = { 0x2108c,  0 },	/* GCC_PCNOC_BUS_TIMEOUT1_BCR */
	[60] = { 0x21094,  0 },	/* GCC_PCNOC_BUS_TIMEOUT2_BCR */
	[61] = { 0x2109c,  0 },	/* GCC_PCNOC_BUS_TIMEOUT3_BCR */
	[62] = { 0x210a4,  0 },	/* GCC_PCNOC_BUS_TIMEOUT4_BCR */
	[63] = { 0x210ac,  0 },	/* GCC_PCNOC_BUS_TIMEOUT5_BCR */
	[64] = { 0x210b4,  0 },	/* GCC_PCNOC_BUS_TIMEOUT6_BCR */
	[65] = { 0x210bc,  0 },	/* GCC_PCNOC_BUS_TIMEOUT7_BCR */
	[66] = { 0x210c4,  0 },	/* GCC_PCNOC_BUS_TIMEOUT8_BCR */
	[67] = { 0x210cc,  0 },	/* GCC_PCNOC_BUS_TIMEOUT9_BCR */
	[68] = { 0x22000,  0 },	/* GCC_TCSR_BCR */
	[70] = { 0x24000,  0 },	/* GCC_MPM_BCR */
	[71] = { 0x25000,  0 },	/* GCC_SPDM_BCR */
	[77] = { 0x1200c,  5 },	/* ESS_PSGMII_ARES (from Linux) */
};

/*
 * Branch clocks: { id, name, enable register, bit, halt register, voted }.
 * Generated from FreeBSD's sys/dev/qcom_gcc/qcom_gcc_ipq4018_clock.c.
 */
static const struct qcom_gcc_branch qcom_gcc_branches[] = {
	{  10, "gcc_apss_ahb_clk", 0x06000, 14, 0x19004, true  },
	{  19, "gcc_audio_ahb_clk", 0x1b010,  0, 0x1b010, false },
	{  20, "gcc_audio_pwm_clk", 0x1b00c,  0, 0x1b00c, false },
	{  21, "gcc_blsp1_ahb_clk", 0x06000, 10, 0x01008, true  },
	{  22, "gcc_blsp1_qup1_i2c_apps_clk", 0x02008,  0, 0x02008, false },
	{  23, "gcc_blsp1_qup1_spi_apps_clk", 0x02004,  0, 0x02004, false },
	{  24, "gcc_blsp1_qup2_i2c_apps_clk", 0x03010,  0, 0x03010, false },
	{  25, "gcc_blsp1_qup2_spi_apps_clk", 0x0300c,  0, 0x0300c, false },
	{  26, "gcc_blsp1_uart1_apps_clk", 0x0203c,  0, 0x0203c, false },
	{  27, "gcc_blsp1_uart2_apps_clk", 0x0302c,  0, 0x0302c, false },
	{  28, "gcc_dcd_xo_clk", 0x2103c,  0, 0x2103c, false },
	{  29, "gcc_gp1_clk", 0x08000,  0, 0x08000, false },
	{  30, "gcc_gp2_clk", 0x09000,  0, 0x09000, false },
	{  31, "gcc_gp3_clk", 0x0a000,  0, 0x0a000, false },
	{  32, "gcc_boot_rom_ahb_clk", 0x1300c,  0, 0x1300c, false },
	{  33, "gcc_crypto_ahb_clk", 0x06000,  0, 0x16024, true  },
	{  34, "gcc_crypto_axi_clk", 0x06000,  1, 0x16020, true  },
	{  35, "gcc_crypto_clk", 0x06000,  2, 0x1601c, true  },
	{  36, "gcc_ess_clk", 0x12010,  0, 0x12010, false },
	{  37, "gcc_imem_axi_clk", 0x06000, 17, 0x0e004, true  },
	{  38, "gcc_imem_cfg_ahb_clk", 0x0e008,  0, 0x0e008, false },
	{  39, "gcc_pcie_ahb_clk", 0x1d00c,  0, 0x1d00c, false },
	{  40, "gcc_pcie_axi_m_clk", 0x1d004,  0, 0x1d004, false },
	{  41, "gcc_pcie_axi_s_clk", 0x1d008,  0, 0x1d008, false },
	{  42, "pcnoc_clk_src", 0x21030,  0, 0x21030, false },
	{  43, "gcc_prng_ahb_clk", 0x06000,  8, 0x13004, true  },
	{  44, "gcc_qpic_ahb_clk", 0x1c008,  0, 0x1c008, false },
	{  45, "gcc_qpic_clk", 0x1c004,  0, 0x1c004, false },
	{  46, "gcc_sdcc1_ahb_clk", 0x18010,  0, 0x18010, false },
	{  47, "gcc_sdcc1_apps_clk", 0x1800c,  0, 0x1800c, false },
	{  52, "gcc_tlmm_ahb_clk", 0x06000,  5, 0x05004, true  },
	{  53, "gcc_usb2_master_clk", 0x1e00c,  0, 0x1e00c, false },
	{  54, "gcc_usb2_sleep_clk", 0x1e010,  0, 0x1e010, false },
	{  55, "gcc_usb2_mock_utmi_clk", 0x1e014,  0, 0x1e014, false },
	{  56, "gcc_usb3_master_clk", 0x1e028,  0, 0x1e028, false },
	{  57, "gcc_usb3_sleep_clk", 0x1e02c,  0, 0x1e02c, false },
	{  58, "gcc_usb3_mock_utmi_clk", 0x1e030,  0, 0x1e030, false },
	{  59, "gcc_wcss2g_clk", 0x1f00c,  0, 0x1f00c, false },
	{  60, "gcc_wcss2g_ref_clk", 0x1f00c,  0, 0x1f00c, false },
	{  61, "gcc_wcss2g_rtc_clk", 0x1f010,  0, 0x1f010, false },
	{  62, "gcc_wcss5g_clk", 0x1f00c,  0, 0x2000c, false },
	{  63, "gcc_wcss5g_ref_clk", 0x1f00c,  0, 0x2000c, false },
	{  64, "gcc_wcss5g_rtc_clk", 0x1f010,  0, 0x20010, false },
};

#define QCOM_GCC_NCLKIDS	128	/* > highest branch id */

struct qcom_gcc_clk {
	struct clk			base;
	const struct qcom_gcc_branch	*def;
};

struct qcom_gcc_softc {
	device_t		sc_dev;
	bus_space_tag_t		sc_bst;
	bus_space_handle_t	sc_bsh;
	kmutex_t		sc_lock;	/* read-modify-write of GCC regs */
	struct clk_domain	sc_clkdom;
	struct qcom_gcc_clk	sc_clks[QCOM_GCC_NCLKIDS];
	struct clk		sc_apps;	/* CPU clock, see below */
};

/*
 * The CPU clock: GCC_APPS_CLK_SRC (id 9), an RCG ("root clock generator":
 * a glitch-free source mux plus a divider), as Linux' gcc-ipq4019.c.
 *   CMD_RCGR 0x1900c: bit 0 UPDATE (latch CFG), bit 31 ROOT_OFF
 *   CFG_RCGR 0x19010: bits 10:8 source, bits 4:0 divider (2 * div - 1)
 * Sources: 0 XO 48 MHz, 1 DDRPLLAPSS (the CPU PLL), 2 FEPLL500,
 * 3 FEPLL200.  The device tree's OPP table has 48, 200, 500 and 716 MHz;
 * 716 comes from DDRPLLAPSS with its post divider (0x2e020 bits 7:4) at 3.
 * No voltage scaling: the board has no CPU regulator (Linux has none
 * either).
 */
#define GCC_APPS_CLK_ID		9	/* GCC_APPS_CLK_SRC */
#define APPS_CMD_RCGR		0x1900c
#define  RCG_UPDATE		__BIT(0)
#define APPS_CFG_RCGR		0x19010
#define  RCG_SRC_SEL		__BITS(10, 8)
#define  RCG_SRC_DIV		__BITS(4, 0)
#define APPS_SRC_XO		0
#define APPS_SRC_DDRPLL		1
#define APPS_SRC_FEPLL500	2
#define APPS_SRC_FEPLL200	3
#define APSS_DDRPLL_DIV		0x2e020	/* VCO dividers + CPU post divider */
#define  DDRPLL_CDIV		__BITS(7, 4)
#define  DDRPLL_FDBKDIV		__BITS(23, 16)
#define  DDRPLL_REFCLKDIV	__BITS(28, 24)
#define XO_HZ			48000000u

#define RD4(sc, r)	bus_space_read_4((sc)->sc_bst, (sc)->sc_bsh, (r))
#define WR4(sc, r, v)	bus_space_write_4((sc)->sc_bst, (sc)->sc_bsh, (r), (v))

static int	qcom_gcc_match(device_t, cfdata_t, void *);
static void	qcom_gcc_attach(device_t, device_t, void *);

static const struct device_compatible_entry compat_data[] = {
	{ .compat = "qcom,gcc-ipq4019" },
	DEVICE_COMPAT_EOL
};

CFATTACH_DECL_NEW(qcom_gcc, sizeof(struct qcom_gcc_softc),
    qcom_gcc_match, qcom_gcc_attach, NULL, NULL);

static void
qcom_gcc_setbit(struct qcom_gcc_softc *sc, uint32_t reg, u_int bit, bool on)
{
	mutex_enter(&sc->sc_lock);
	const uint32_t v = RD4(sc, reg);
	WR4(sc, reg, on ? (v | __BIT(bit)) : (v & ~__BIT(bit)));
	mutex_exit(&sc->sc_lock);
}

/* --- resets ---------------------------------------------------------- */

static void *
qcom_gcc_reset_acquire(device_t dev, const void *data, size_t len)
{
	if (len != 4)
		return NULL;
	const u_int id = be32dec(data);
	if (id >= __arraycount(qcom_gcc_resets) ||
	    qcom_gcc_resets[id].reg == 0)
		return NULL;
	return __UNCONST(&qcom_gcc_resets[id]);
}

static void
qcom_gcc_reset_release(device_t dev, void *priv)
{
}

static int
qcom_gcc_reset_assert(device_t dev, void *priv)
{
	const struct qcom_gcc_reset * const r = priv;

	qcom_gcc_setbit(device_private(dev), r->reg, r->bit, true);
	return 0;
}

static int
qcom_gcc_reset_deassert(device_t dev, void *priv)
{
	const struct qcom_gcc_reset * const r = priv;

	qcom_gcc_setbit(device_private(dev), r->reg, r->bit, false);
	return 0;
}

static const struct fdtbus_reset_controller_func qcom_gcc_reset_funcs = {
	.acquire = qcom_gcc_reset_acquire,
	.release = qcom_gcc_reset_release,
	.reset_assert = qcom_gcc_reset_assert,
	.reset_deassert = qcom_gcc_reset_deassert,
};

/* --- clocks ---------------------------------------------------------- */

static struct clk *
qcom_gcc_clk_get(void *priv, const char *name)
{
	struct qcom_gcc_softc * const sc = priv;

	if (strcmp(name, "apps_clk_src") == 0)
		return &sc->sc_apps;
	for (u_int i = 0; i < QCOM_GCC_NCLKIDS; i++)
		if (sc->sc_clks[i].def != NULL &&
		    strcmp(sc->sc_clks[i].def->name, name) == 0)
			return &sc->sc_clks[i].base;
	return NULL;
}

static void
qcom_gcc_clk_put(void *priv, struct clk *clk)
{
}

/* DDRPLLAPSS output (Linux clk_cpu_div_recalc_rate) */
static u_int
qcom_gcc_ddrpll_rate(struct qcom_gcc_softc *sc)
{
	const uint32_t v = RD4(sc, APSS_DDRPLL_DIV);
	const u_int ref = __SHIFTOUT(v, DDRPLL_REFCLKDIV);
	const u_int fb = __SHIFTOUT(v, DDRPLL_FDBKDIV);
	const u_int cdiv = __SHIFTOUT(v, DDRPLL_CDIV);

	if (ref == 0)
		return 0;
	const uint64_t vco = (uint64_t)XO_HZ / ref * 2 * fb;
	/* the post divider is non-linear and has halves: work in 2x */
	const u_int pre_div2 = cdiv > 10 ? (cdiv + 1) * 2 : cdiv + 12;
	return vco * 2 / pre_div2;
}

static u_int
qcom_gcc_apps_get_rate(struct qcom_gcc_softc *sc)
{
	const uint32_t cfg = RD4(sc, APPS_CFG_RCGR);
	const u_int hid = __SHIFTOUT(cfg, RCG_SRC_DIV);
	u_int rate;

	switch (__SHIFTOUT(cfg, RCG_SRC_SEL)) {
	case APPS_SRC_XO:	rate = XO_HZ; break;
	case APPS_SRC_DDRPLL:	rate = qcom_gcc_ddrpll_rate(sc); break;
	case APPS_SRC_FEPLL500:	rate = 500000000; break;
	case APPS_SRC_FEPLL200:	rate = 200000000; break;
	default:		return 0;
	}
	/* divider field 2 * div - 1; 0 means bypass */
	return hid == 0 ? rate : (u_int)((uint64_t)rate * 2 / (hid + 1));
}

static int
qcom_gcc_apps_set_rate(struct qcom_gcc_softc *sc, u_int rate)
{
	u_int src;

	switch (rate) {
	case 48000000:	src = APPS_SRC_XO; break;
	case 200000000:	src = APPS_SRC_FEPLL200; break;
	case 500000000:	src = APPS_SRC_FEPLL500; break;
	case 716000000:	src = APPS_SRC_DDRPLL; break;
	default:	return EINVAL;
	}
	mutex_enter(&sc->sc_lock);
	if (src == APPS_SRC_DDRPLL &&
	    __SHIFTOUT(RD4(sc, APSS_DDRPLL_DIV), DDRPLL_CDIV) != 3) {
		/*
		 * The PLL's post divider has no status bit: never change it
		 * while the CPUs run from it.  Park on FEPLL500 (Linux' "safe
		 * parent") first.
		 */
		if (__SHIFTOUT(RD4(sc, APPS_CFG_RCGR), RCG_SRC_SEL) ==
		    APPS_SRC_DDRPLL) {
			WR4(sc, APPS_CFG_RCGR, __SHIFTIN(APPS_SRC_FEPLL500,
			    RCG_SRC_SEL) | __SHIFTIN(1, RCG_SRC_DIV));
			WR4(sc, APPS_CMD_RCGR, RD4(sc, APPS_CMD_RCGR) |
			    RCG_UPDATE);
			for (u_int i = 0; i < 1000 &&
			    (RD4(sc, APPS_CMD_RCGR) & RCG_UPDATE); i++)
				delay(1);
		}
		const uint32_t v = RD4(sc, APSS_DDRPLL_DIV);
		WR4(sc, APSS_DDRPLL_DIV, (v & ~DDRPLL_CDIV) |
		    __SHIFTIN(3, DDRPLL_CDIV));
		delay(1);
	}
	/* new source, divider 1; UPDATE latches it, glitch-free */
	WR4(sc, APPS_CFG_RCGR, __SHIFTIN(src, RCG_SRC_SEL) |
	    __SHIFTIN(1, RCG_SRC_DIV));
	WR4(sc, APPS_CMD_RCGR, RD4(sc, APPS_CMD_RCGR) | RCG_UPDATE);
	u_int i;
	for (i = 0; i < 1000 && (RD4(sc, APPS_CMD_RCGR) & RCG_UPDATE); i++)
		delay(1);
	mutex_exit(&sc->sc_lock);
	return i < 1000 ? 0 : ETIMEDOUT;
}

static u_int
qcom_gcc_clk_get_rate(void *priv, struct clk *clk)
{
	struct qcom_gcc_softc * const sc = priv;

	if (clk == &sc->sc_apps)
		return qcom_gcc_apps_get_rate(sc);
	return 0;	/* branch clocks: rates are not modelled */
}

static int
qcom_gcc_clk_set_rate(void *priv, struct clk *clk, u_int rate)
{
	struct qcom_gcc_softc * const sc = priv;

	if (clk == &sc->sc_apps)
		return qcom_gcc_apps_set_rate(sc, rate);
	return ENXIO;
}

static int
qcom_gcc_clk_enable(void *priv, struct clk *clk)
{
	struct qcom_gcc_softc * const sc = priv;
	if (clk == &sc->sc_apps)
		return 0;		/* the CPUs run on it */
	const struct qcom_gcc_branch * const b =
	    ((struct qcom_gcc_clk *)clk)->def;

	if ((RD4(sc, b->halt_reg) & CBCR_CLK_OFF) == 0)
		return 0;		/* already running */
	qcom_gcc_setbit(sc, b->enable_reg, b->enable_bit, true);
	for (u_int i = 0; i < 200; i++) {
		if ((RD4(sc, b->halt_reg) & CBCR_CLK_OFF) == 0)
			return 0;
		delay(1);
	}
	device_printf(sc->sc_dev, "%s didn't start\n", b->name);
	return ETIMEDOUT;
}

static int
qcom_gcc_clk_disable(void *priv, struct clk *clk)
{
	return 0;	/* left running, see the comment at the top */
}

static const struct clk_funcs qcom_gcc_clk_funcs = {
	.get = qcom_gcc_clk_get,
	.put = qcom_gcc_clk_put,
	.get_rate = qcom_gcc_clk_get_rate,
	.set_rate = qcom_gcc_clk_set_rate,
	.enable = qcom_gcc_clk_enable,
	.disable = qcom_gcc_clk_disable,
};

static struct clk *
qcom_gcc_clock_decode(device_t dev, int cc_phandle, const void *data,
    size_t len)
{
	struct qcom_gcc_softc * const sc = device_private(dev);

	if (len != 4)
		return NULL;
	const u_int id = be32dec(data);
	if (id == GCC_APPS_CLK_ID)
		return &sc->sc_apps;
	if (id >= QCOM_GCC_NCLKIDS || sc->sc_clks[id].def == NULL)
		return NULL;	/* not a branch clock: not supported yet */
	return &sc->sc_clks[id].base;
}

static const struct fdtbus_clock_controller_func qcom_gcc_clock_funcs = {
	.decode = qcom_gcc_clock_decode,
};

/* --- autoconf -------------------------------------------------------- */

static int
qcom_gcc_match(device_t parent, cfdata_t cf, void *aux)
{
	struct fdt_attach_args * const faa = aux;

	return of_compatible_match(faa->faa_phandle, compat_data);
}

static void
qcom_gcc_attach(device_t parent, device_t self, void *aux)
{
	struct qcom_gcc_softc * const sc = device_private(self);
	struct fdt_attach_args * const faa = aux;
	const int phandle = faa->faa_phandle;
	bus_addr_t addr;
	bus_size_t size;

	sc->sc_dev = self;
	sc->sc_bst = faa->faa_bst;
	if (fdtbus_get_reg(phandle, 0, &addr, &size) != 0 ||
	    bus_space_map(sc->sc_bst, addr, size, 0, &sc->sc_bsh) != 0) {
		aprint_error(": couldn't map registers\n");
		return;
	}
	mutex_init(&sc->sc_lock, MUTEX_DEFAULT, IPL_HIGH);

	sc->sc_clkdom.name = device_xname(self);
	sc->sc_clkdom.funcs = &qcom_gcc_clk_funcs;
	sc->sc_clkdom.priv = sc;
	for (u_int i = 0; i < __arraycount(qcom_gcc_branches); i++) {
		const struct qcom_gcc_branch * const b = &qcom_gcc_branches[i];
		KASSERT(b->id < QCOM_GCC_NCLKIDS);
		sc->sc_clks[b->id].def = b;
		sc->sc_clks[b->id].base.domain = &sc->sc_clkdom;
		sc->sc_clks[b->id].base.name = b->name;
		clk_attach(&sc->sc_clks[b->id].base);
	}
	sc->sc_apps.domain = &sc->sc_clkdom;
	sc->sc_apps.name = "apps_clk_src";
	clk_attach(&sc->sc_apps);

	aprint_naive("\n");
	aprint_normal(": IPQ40xx clock controller (%zu resets, %zu gates)\n",
	    __arraycount(qcom_gcc_resets), __arraycount(qcom_gcc_branches));

	fdtbus_register_reset_controller(self, phandle, &qcom_gcc_reset_funcs);
	fdtbus_register_clock_controller(self, phandle, &qcom_gcc_clock_funcs);
	/*
	 * The CPU clock (an RCG behind the A7 PLL) isn't driven yet, so the
	 * CPUs run at whatever the boot loader left.  Measure it: CPU cycles
	 * (PMU cycle counter) over 20 ms of the 48 MHz generic timer.
	 * (cpu_hascounter() says no: it looks at cpu_cc_freq, which the
	 * cpufreq driver would fill in.)
	 */
	{
		const uint32_t c0 = cpu_counter32();
		delay(20000);
		const uint32_t c1 = cpu_counter32();
		if (c1 != c0)
			aprint_normal_dev(self, "CPU clock: %u MHz (measured), "
			    "%u MHz (from the registers)\n", (c1 - c0) / 20000,
			    qcom_gcc_apps_get_rate(sc) / 1000000);
	}
}
