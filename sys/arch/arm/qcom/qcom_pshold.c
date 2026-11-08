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
 * Qualcomm PS_HOLD reset ("qcom,pshold").
 *
 * Driving PS_HOLD low (writing 0 to the register) makes the PMIC/SoC reset
 * the board.  Same mechanism as Linux drivers/power/reset/msm-poweroff.c;
 * written for the FRITZ!Box 7530 (IPQ4019) NetBSD experiment.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/device.h>
#include <sys/systm.h>

#include <dev/fdt/fdtvar.h>

static const struct device_compatible_entry compat_data[] = {
	{ .compat = "qcom,pshold" },
	DEVICE_COMPAT_EOL
};

struct qcom_pshold_softc {
	device_t		sc_dev;
	bus_space_tag_t		sc_bst;
	bus_space_handle_t	sc_bsh;
};

static void
qcom_pshold_reset(device_t dev)
{
	struct qcom_pshold_softc * const sc = device_private(dev);

	bus_space_write_4(sc->sc_bst, sc->sc_bsh, 0, 0);
	delay(10000000);	/* reset takes effect almost at once */
	aprint_error_dev(dev, "PS_HOLD reset failed\n");
}

static const struct fdtbus_power_controller_func qcom_pshold_funcs = {
	.reset = qcom_pshold_reset,
};

static int
qcom_pshold_match(device_t parent, cfdata_t cf, void *aux)
{
	struct fdt_attach_args * const faa = aux;

	return of_compatible_match(faa->faa_phandle, compat_data);
}

static void
qcom_pshold_attach(device_t parent, device_t self, void *aux)
{
	struct qcom_pshold_softc * const sc = device_private(self);
	struct fdt_attach_args * const faa = aux;
	const int phandle = faa->faa_phandle;
	bus_addr_t addr;
	bus_size_t size;

	if (fdtbus_get_reg(phandle, 0, &addr, &size) != 0) {
		aprint_error(": couldn't get registers\n");
		return;
	}
	sc->sc_dev = self;
	sc->sc_bst = faa->faa_bst;
	if (bus_space_map(sc->sc_bst, addr, size, 0, &sc->sc_bsh) != 0) {
		aprint_error(": couldn't map registers\n");
		return;
	}

	aprint_naive("\n");
	aprint_normal(": PS_HOLD reset\n");

	fdtbus_register_power_controller(self, phandle, &qcom_pshold_funcs);
}

CFATTACH_DECL_NEW(qcom_pshold, sizeof(struct qcom_pshold_softc),
	qcom_pshold_match, qcom_pshold_attach, NULL, NULL);
