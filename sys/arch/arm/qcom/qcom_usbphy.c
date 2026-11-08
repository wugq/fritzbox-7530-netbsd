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
 * Qualcomm IPQ40xx USB PHYs: "qcom,usb-hs-ipq4019-phy" (USB 2 part) and
 * "qcom,usb-ss-ipq4019-phy" (USB 3 part).  Bringing one up is only a
 * reset sequence through the GCC (as FreeBSD's ipq4018_usb_{hs,ss}_phy.c
 * and Linux' phy-qcom-ipq4019-usb.c): assert the resets, wait, release
 * them.  The DWC3 controller driver (dwc3_fdt) enables its "phys" when it
 * attaches.  Before this driver the boot scripts poked the same GCC
 * register (0x181e038) from U-Boot.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/device.h>
#include <sys/systm.h>

#include <dev/fdt/fdtvar.h>

enum qcom_usbphy_type { USBPHY_HS = 1, USBPHY_SS };

static const struct device_compatible_entry compat_data[] = {
	{ .compat = "qcom,usb-hs-ipq4019-phy", .value = USBPHY_HS },
	{ .compat = "qcom,usb-ss-ipq4019-phy", .value = USBPHY_SS },
	DEVICE_COMPAT_EOL
};

struct qcom_usbphy_softc {
	device_t		sc_dev;
	enum qcom_usbphy_type	sc_type;
	struct fdtbus_reset	*sc_por;	/* power-on reset */
	struct fdtbus_reset	*sc_srif;	/* HS only */
};

static int	qcom_usbphy_match(device_t, cfdata_t, void *);
static void	qcom_usbphy_attach(device_t, device_t, void *);

CFATTACH_DECL_NEW(qcom_usbphy, sizeof(struct qcom_usbphy_softc),
    qcom_usbphy_match, qcom_usbphy_attach, NULL, NULL);

static void *
qcom_usbphy_acquire(device_t dev, const void *data, size_t len)
{
	return device_private(dev);	/* #phy-cells = <0> */
}

static void
qcom_usbphy_release(device_t dev, void *priv)
{
}

static int
qcom_usbphy_enable(device_t dev, void *priv, bool enable)
{
	struct qcom_usbphy_softc * const sc = priv;

	fdtbus_reset_assert(sc->sc_por);
	delay(10000);
	if (sc->sc_srif != NULL) {
		fdtbus_reset_assert(sc->sc_srif);
		delay(10000);
	}
	if (!enable)
		return 0;
	if (sc->sc_srif != NULL) {
		fdtbus_reset_deassert(sc->sc_srif);
		delay(10000);
	}
	fdtbus_reset_deassert(sc->sc_por);
	delay(10000);
	return 0;
}

static const struct fdtbus_phy_controller_func qcom_usbphy_funcs = {
	.acquire = qcom_usbphy_acquire,
	.release = qcom_usbphy_release,
	.enable = qcom_usbphy_enable,
};

static int
qcom_usbphy_match(device_t parent, cfdata_t cf, void *aux)
{
	struct fdt_attach_args * const faa = aux;

	return of_compatible_match(faa->faa_phandle, compat_data);
}

static void
qcom_usbphy_attach(device_t parent, device_t self, void *aux)
{
	struct qcom_usbphy_softc * const sc = device_private(self);
	struct fdt_attach_args * const faa = aux;
	const int phandle = faa->faa_phandle;

	sc->sc_dev = self;
	sc->sc_type = of_compatible_lookup(phandle, compat_data)->value;
	sc->sc_por = fdtbus_reset_get(phandle, "por_rst");
	if (sc->sc_type == USBPHY_HS)
		sc->sc_srif = fdtbus_reset_get(phandle, "srif_rst");
	if (sc->sc_por == NULL ||
	    (sc->sc_type == USBPHY_HS && sc->sc_srif == NULL)) {
		aprint_error(": couldn't get resets\n");
		return;
	}

	aprint_naive("\n");
	aprint_normal(": IPQ40xx USB %s PHY\n",
	    sc->sc_type == USBPHY_HS ? "2.0 (HS)" : "3.0 (SS)");

	fdtbus_register_phy_controller(self, phandle, &qcom_usbphy_funcs);
}
