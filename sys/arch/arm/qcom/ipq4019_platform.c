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
 * Qualcomm IPQ4019 platform (AVM FRITZ!Box 7530 NetBSD experiment).
 *
 * Same as the default arm platform (arch/arm/fdt/arm_platform.c) plus:
 *  - a static device mapping of 0x0b000000-0x0b0fffff (GIC, the per-CPU
 *    ACC/SAW power controllers and the L2 SAW), so they can be used while
 *    the secondary CPUs are started, before autoconf;
 *  - the "qcom,kpss-acc-v2" CPU enable method (SMP).
 *
 * The SMP bring-up follows Linux arch/arm/mach-qcom/platsmp.c
 * (kpssv2_release_secondary) and drivers/firmware/qcom/qcom_scm*.c
 * (legacy atomic SCM call QCOM_SCM_BOOT_SET_ADDR).
 */

#include "opt_console.h"
#include "opt_multiprocessor.h"

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/cpu.h>
#include <sys/device.h>
#include <sys/termios.h>

#include <dev/fdt/fdtvar.h>

#include <uvm/uvm_extern.h>

#include <machine/bootconfig.h>

#include <arm/cpufunc.h>
#include <arm/locore.h>

#include <arm/cortex/gtmr_var.h>
#include <arm/cortex/gic_reg.h>

#include <arm/fdt/arm_fdtvar.h>

#include <libfdt.h>

#include <arch/evbarm/fdt/platform.h>

extern struct arm32_bus_dma_tag arm_generic_dma_tag;
extern struct bus_space arm_generic_bs_tag;

#define IPQ4019_UART1_BASE	0x078af000
#define IPQ4019_APSS_BASE	0x0b000000	/* GIC, ACC, SAW */
#define IPQ4019_APSS_SIZE	0x00100000
#define IPQ4019_GICD_BASE	0x0b000000

#define IPQ4019_UART_VBASE	KERNEL_IO_VBASE
#define IPQ4019_APSS_VBASE	(KERNEL_IO_VBASE + 0x00100000)

static void
ipq4019_platform_init_attach_args(struct fdt_attach_args *faa)
{
	faa->faa_bst = &arm_generic_bs_tag;
	faa->faa_dmat = &arm_generic_dma_tag;
}

static void
ipq4019_platform_device_register(device_t self, void *aux)
{
}

static const struct pmap_devmap *
ipq4019_platform_devmap(void)
{
	/*
	 * The UART must be first and at KERNEL_IO_VBASE: the early console
	 * (qcom_platform_early_putchar) assumes that layout.
	 */
	static const struct pmap_devmap devmap[] = {
		DEVMAP_ENTRY(IPQ4019_UART_VBASE,
		    DEVMAP_ALIGN(IPQ4019_UART1_BASE), PAGE_SIZE),
		DEVMAP_ENTRY(IPQ4019_APSS_VBASE,
		    IPQ4019_APSS_BASE, IPQ4019_APSS_SIZE),
		DEVMAP_ENTRY_END
	};

	return devmap;
}

static u_int
ipq4019_platform_uart_freq(void)
{
	return 0;
}

static const struct fdt_platform ipq4019_platform = {
	.fp_devmap = ipq4019_platform_devmap,
	.fp_bootstrap = arm_fdt_cpu_bootstrap,
	.fp_init_attach_args = ipq4019_platform_init_attach_args,
	.fp_device_register = ipq4019_platform_device_register,
	/*
	 * No fp_reset: reboot drives PS_HOLD low through the qcom_pshold
	 * driver (fdtbus_power_reset() is tried first).  There is no PSCI
	 * firmware here, psci_fdt_reset() would only print an error.
	 */
	.fp_delay = gtmr_delay,
	.fp_uart_freq = ipq4019_platform_uart_freq,
	.fp_mpstart = arm_fdt_cpu_mpstart,
};

FDT_PLATFORM(ipq4019, "qcom,ipq4019", &ipq4019_platform);

#ifdef MULTIPROCESSOR

/* ACC (APCS) registers, per CPU */
#define APCS_CPU_PWR_CTL	0x04
#define  CORE_PWRD_UP		__BIT(7)
#define  COREPOR_RST		__BIT(5)
#define  CLAMP			__BIT(0)
#define APC_PWR_GATE_CTL	0x14
#define  BHS_CNT_SHIFT		24
#define  LDO_PWR_DWN_SHIFT	16
#define  LDO_BYP_SHIFT		8
#define  BHS_SEG_SHIFT		1
#define  BHS_EN			__BIT(0)
/* SAW2 (L2) */
#define APCS_SAW2_2_VCTL	0x1c

/* Legacy atomic SCM call: QCOM_SCM_SVC_BOOT / QCOM_SCM_BOOT_SET_ADDR, 2 args */
#define SCM_LEGACY_FNID(s, c)	(((s) << 10) | ((c) & 0x3ff))
#define SCM_LEGACY_ATOMIC_ID(s, c, n) \
	((SCM_LEGACY_FNID(s, c) << 12) | (0x2 << 8) | __BIT(5) | ((n) & 0xf))
#define QCOM_SCM_SVC_BOOT	0x01
#define QCOM_SCM_BOOT_SET_ADDR	0x01
/* cold boot flag bits for CPU 1, 2, 3 (CPU 0 has none) */
static const uint32_t ipq4019_cold_bits[4] = { 0, __BIT(0), __BIT(3), __BIT(5) };

/* Busy wait on the virtual counter; usable before the timer attaches. */
static void
ipq4019_udelay(u_int us)
{
	const uint64_t ticks = (uint64_t)us * 48;	/* 48 MHz XO */
	uint64_t start, now;

	isb();
	start = armreg_cntv_ct_read();
	do {
		isb();
		now = armreg_cntv_ct_read();
	} while (now - start < ticks);
}

static vaddr_t
ipq4019_apss_va(int phandle, int idx)
{
	bus_addr_t addr;

	if (phandle <= 0 || fdtbus_get_reg(phandle, idx, &addr, NULL) != 0)
		return 0;
	if (addr < IPQ4019_APSS_BASE ||
	    addr >= IPQ4019_APSS_BASE + IPQ4019_APSS_SIZE)
		return 0;
	return IPQ4019_APSS_VBASE + (addr - IPQ4019_APSS_BASE);
}

#define W4(va, off, v)	do {						\
	*(volatile uint32_t *)((va) + (off)) = (v);			\
	dsb(sy);							\
} while (0)

/*
 * One call into the secure monitor (TrustZone firmware, "SCM"): the
 * "legacy atomic" convention of Linux' qcom_scm-legacy.c.  The command id
 * goes in r0, up to three arguments in r1-r3, then "smc #0" traps into
 * the monitor; the result comes back in r0, r1-r3 and r12 may be
 * clobbered.  (This used to borrow PSCI's psci_call_smc(), which is the
 * same instruction but tied the platform code to the PSCI driver.)
 */
static int
ipq4019_scm_call(uint32_t id, uint32_t a1, uint32_t a2, uint32_t a3)
{
	register uint32_t r0 __asm("r0") = id;
	register uint32_t r1 __asm("r1") = a1;
	register uint32_t r2 __asm("r2") = a2;
	register uint32_t r3 __asm("r3") = a3;

	__asm __volatile(
	    ".arch_extension sec\n"
	    "smc	#0"
	    : "+r" (r0), "+r" (r1), "+r" (r2), "+r" (r3)
	    :
	    : "r12", "memory");
	return (int)r0;
}

static int
cpu_enable_qcom_kpss_acc_v2(int phandle)
{
	static bool scm_done;
	uint64_t mpidr;
	paddr_t entry;
	vaddr_t acc, l2saw;
	uint32_t val;
	u_int cpu;
	int ret;

	if (fdtbus_get_reg64(phandle, 0, &mpidr, NULL) != 0)
		return ENXIO;
	cpu = mpidr & 0xff;
	if (cpu == 0 || cpu > 3)
		return ENXIO;

	acc = ipq4019_apss_va(fdtbus_get_phandle(phandle, "qcom,acc"), 0);
	const int l2 = fdtbus_get_phandle(phandle, "next-level-cache");
	l2saw = ipq4019_apss_va(fdtbus_get_phandle(l2, "qcom,saw"), 0);
	if (acc == 0 || l2saw == 0) {
		printf("%s: cpu%u: no ACC/L2 SAW\n", __func__, cpu);
		return ENXIO;
	}

	/* Tell the secure firmware where CPUs 1-3 start (once, for all). */
	if (!scm_done) {
		if (!pmap_extract(pmap_kernel(), (vaddr_t)cpu_mpstart, &entry))
			return ENXIO;
		cpu_dcache_wbinv_all();
		ret = ipq4019_scm_call(
		    SCM_LEGACY_ATOMIC_ID(QCOM_SCM_SVC_BOOT,
			QCOM_SCM_BOOT_SET_ADDR, 2),
		    0,
		    ipq4019_cold_bits[1] | ipq4019_cold_bits[2] |
			ipq4019_cold_bits[3],
		    entry);
		printf("%s: SCM set cold boot addr %#lx -> %d\n", __func__,
		    (u_long)entry, ret);
		if (ret != 0)
			return EIO;
		scm_done = true;
	}

	/* Turn on the BHS, turn off LDO bypass and power down the LDO */
	val = (64 << BHS_CNT_SHIFT) | (0x3f << LDO_PWR_DWN_SHIFT) | BHS_EN;
	W4(acc, APC_PWR_GATE_CTL, val);
	ipq4019_udelay(1);

	/* Turn on BHS segments */
	val |= 0x3f << BHS_SEG_SHIFT;
	W4(acc, APC_PWR_GATE_CTL, val);
	ipq4019_udelay(1);

	/* Turn on the bypass so that the BHS supplies power */
	val |= 0x3f << LDO_BYP_SHIFT;
	W4(acc, APC_PWR_GATE_CTL, val);

	/* Enable max phases */
	W4(l2saw, APCS_SAW2_2_VCTL, 0x10003);
	ipq4019_udelay(50);

	val = COREPOR_RST | CLAMP;
	W4(acc, APCS_CPU_PWR_CTL, val);
	ipq4019_udelay(2);

	val &= ~CLAMP;
	W4(acc, APCS_CPU_PWR_CTL, val);
	ipq4019_udelay(2);

	val &= ~COREPOR_RST;
	W4(acc, APCS_CPU_PWR_CTL, val);

	val |= CORE_PWRD_UP;
	W4(acc, APCS_CPU_PWR_CTL, val);

	/*
	 * Like Linux, also send the CPU a software interrupt so a boot monitor
	 * waiting in WFI re-reads the boot address.  The GIC is not set up by
	 * NetBSD yet; enable the (non-secure) distributor first.
	 */
	vaddr_t gicd = IPQ4019_APSS_VBASE + (IPQ4019_GICD_BASE - IPQ4019_APSS_BASE);
	W4(gicd, GICD_CTRL, *(volatile uint32_t *)(gicd + GICD_CTRL) | 1);
	W4(gicd, GICD_SGIR, __SHIFTIN(__BIT(cpu), GICD_SGIR_TargetList) |
	    GICD_SGIR_TargetListFilter_List);

	return 0;
}
ARM_CPU_METHOD(qcom_kpss_acc_v2, "qcom,kpss-acc-v2", cpu_enable_qcom_kpss_acc_v2);

#endif /* MULTIPROCESSOR */
