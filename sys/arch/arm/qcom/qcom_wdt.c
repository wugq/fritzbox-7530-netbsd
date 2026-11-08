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
 * Qualcomm KPSS watchdog ("qcom,kpss-wdt"), minimal: keeps a hardware
 * watchdog running and pets it from a callout, so a hung or panicked
 * kernel resets the board by itself.  Register layout from Linux
 * drivers/watchdog/qcom-wdt.c (reg_offset_data_kpss).  Written for the
 * FRITZ!Box 7530 (IPQ4019) NetBSD experiment.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/callout.h>
#include <sys/device.h>
#include <sys/systm.h>

#include <dev/fdt/fdtvar.h>

#define WDT_RST		0x04
#define WDT_EN		0x08
#define WDT_STS		0x0c
#define WDT_BARK_TIME	0x10
#define WDT_BITE_TIME	0x14

#define WDT_RATE	32768	/* sleep clock */
#define WDT_TIMEOUT	60	/* seconds */
#define WDT_PET		5	/* seconds */

static const struct device_compatible_entry compat_data[] = {
	{ .compat = "qcom,kpss-wdt" },
	DEVICE_COMPAT_EOL
};

struct qcom_wdt_softc {
	device_t		sc_dev;
	bus_space_tag_t		sc_bst;
	bus_space_handle_t	sc_bsh;
	callout_t		sc_ch;
};

#define WR4(sc, r, v)	bus_space_write_4((sc)->sc_bst, (sc)->sc_bsh, (r), (v))

static void
qcom_wdt_pet(void *arg)
{
	struct qcom_wdt_softc * const sc = arg;

	WR4(sc, WDT_RST, 1);
	callout_schedule(&sc->sc_ch, WDT_PET * hz);
}

static int
qcom_wdt_match(device_t parent, cfdata_t cf, void *aux)
{
	struct fdt_attach_args * const faa = aux;

	return of_compatible_match(faa->faa_phandle, compat_data);
}

static void
qcom_wdt_attach(device_t parent, device_t self, void *aux)
{
	struct qcom_wdt_softc * const sc = device_private(self);
	struct fdt_attach_args * const faa = aux;
	bus_addr_t addr;
	bus_size_t size;

	if (fdtbus_get_reg(faa->faa_phandle, 0, &addr, &size) != 0 ||
	    bus_space_map(faa->faa_bst, addr, size, 0, &sc->sc_bsh) != 0) {
		aprint_error(": couldn't map registers\n");
		return;
	}
	sc->sc_dev = self;
	sc->sc_bst = faa->faa_bst;

	WR4(sc, WDT_EN, 0);
	WR4(sc, WDT_RST, 1);
	WR4(sc, WDT_BARK_TIME, WDT_TIMEOUT * WDT_RATE);
	WR4(sc, WDT_BITE_TIME, WDT_TIMEOUT * WDT_RATE);
	WR4(sc, WDT_EN, 1);

	callout_init(&sc->sc_ch, CALLOUT_MPSAFE);
	callout_setfunc(&sc->sc_ch, qcom_wdt_pet, sc);
	callout_schedule(&sc->sc_ch, WDT_PET * hz);

	aprint_naive("\n");
	aprint_normal(": watchdog, %d s timeout, kernel petted\n", WDT_TIMEOUT);
}

CFATTACH_DECL_NEW(qcom_wdt, sizeof(struct qcom_wdt_softc),
	qcom_wdt_match, qcom_wdt_attach, NULL, NULL);
