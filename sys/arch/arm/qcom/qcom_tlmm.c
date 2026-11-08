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
 * Qualcomm IPQ40xx TLMM ("qcom,ipq4019-pinctrl"): the pin controller with
 * the 100 GPIOs.  Each pin has a 4 KB register page:
 *   +0 config: bits 0-1 pull, 2-4 function (0 = GPIO), 6-8 drive strength,
 *              9 output enable
 *   +4 in/out: bit 0 input level, bit 1 output level
 * (as FreeBSD's qcom_tlmm_ipq4018_reg.h and Linux pinctrl-msm).
 *
 * Provides: GPIOs to other drivers through the device tree (gpio-keys for
 * the buttons, gpio-leds for the LEDs, ...); "gpio-hog" children, pins set
 * once at attach (the USB 3 VBUS switch, GPIO 49, is one); and a gpiobus
 * for gpioctl(8).  Pin function selection (pinctrl) and GPIO interrupts
 * are not done: the firmware sets the functions, gpio-keys polls.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/device.h>
#include <sys/gpio.h>
#include <sys/kmem.h>
#include <sys/mutex.h>
#include <sys/systm.h>

#include <dev/fdt/fdtvar.h>
#include <dev/gpio/gpiovar.h>

#define TLMM_NPINS		100
#define TLMM_PIN(p, r)		((p) * 0x1000 + (r))
#define TLMM_CFG		0x0
#define  TLMM_CFG_PULL		__BITS(1, 0)
#define   TLMM_PULL_NONE	0
#define   TLMM_PULL_DOWN	1
#define   TLMM_PULL_UP		2
#define  TLMM_CFG_FUNC		__BITS(4, 2)
#define  TLMM_CFG_OE		__BIT(9)
#define TLMM_IO			0x4
#define  TLMM_IO_IN		__BIT(0)
#define  TLMM_IO_OUT		__BIT(1)

struct qcom_tlmm_softc {
	device_t		sc_dev;
	int			sc_phandle;
	bus_space_tag_t		sc_bst;
	bus_space_handle_t	sc_bsh;
	kmutex_t		sc_lock;
	struct gpio_chipset_tag	sc_gp;
	gpio_pin_t		sc_pins[TLMM_NPINS];
};

struct qcom_tlmm_pin {
	struct qcom_tlmm_softc	*pin_sc;
	u_int			pin_num;
	bool			pin_actlo;
};

#define RD4(sc, r)	bus_space_read_4((sc)->sc_bst, (sc)->sc_bsh, (r))
#define WR4(sc, r, v)	bus_space_write_4((sc)->sc_bst, (sc)->sc_bsh, (r), (v))

static int	qcom_tlmm_match(device_t, cfdata_t, void *);
static void	qcom_tlmm_attach(device_t, device_t, void *);

static const struct device_compatible_entry compat_data[] = {
	{ .compat = "qcom,ipq4019-pinctrl" },
	DEVICE_COMPAT_EOL
};

CFATTACH_DECL_NEW(qcom_tlmm, sizeof(struct qcom_tlmm_softc),
    qcom_tlmm_match, qcom_tlmm_attach, NULL, NULL);

/* Make pin a GPIO with the given direction and bias; sc_lock held. */
static void
qcom_tlmm_ctl(struct qcom_tlmm_softc *sc, u_int pin, int flags)
{
	uint32_t cfg = RD4(sc, TLMM_PIN(pin, TLMM_CFG));

	cfg &= ~TLMM_CFG_FUNC;			/* function 0: GPIO */
	if (flags & GPIO_PIN_OUTPUT)
		cfg |= TLMM_CFG_OE;
	else if (flags & GPIO_PIN_INPUT)
		cfg &= ~TLMM_CFG_OE;
	if (flags & (GPIO_PIN_PULLUP | GPIO_PIN_PULLDOWN | GPIO_PIN_PUSHPULL)) {
		cfg &= ~TLMM_CFG_PULL;
		if (flags & GPIO_PIN_PULLUP)
			cfg |= __SHIFTIN(TLMM_PULL_UP, TLMM_CFG_PULL);
		else if (flags & GPIO_PIN_PULLDOWN)
			cfg |= __SHIFTIN(TLMM_PULL_DOWN, TLMM_CFG_PULL);
	}
	WR4(sc, TLMM_PIN(pin, TLMM_CFG), cfg);
}

static int
qcom_tlmm_pin_read(void *priv, int pin)
{
	struct qcom_tlmm_softc * const sc = priv;

	return (RD4(sc, TLMM_PIN(pin, TLMM_IO)) & TLMM_IO_IN) ? 1 : 0;
}

static void
qcom_tlmm_pin_write(void *priv, int pin, int val)
{
	struct qcom_tlmm_softc * const sc = priv;

	mutex_enter(&sc->sc_lock);
	const uint32_t io = RD4(sc, TLMM_PIN(pin, TLMM_IO));
	WR4(sc, TLMM_PIN(pin, TLMM_IO),
	    val ? (io | TLMM_IO_OUT) : (io & ~TLMM_IO_OUT));
	mutex_exit(&sc->sc_lock);
}

static void
qcom_tlmm_pin_ctl(void *priv, int pin, int flags)
{
	struct qcom_tlmm_softc * const sc = priv;

	mutex_enter(&sc->sc_lock);
	qcom_tlmm_ctl(sc, pin, flags);
	mutex_exit(&sc->sc_lock);
}

/* --- GPIOs for other drivers: gpios = <&tlmm pin flags> -------------- */

static void *
qcom_tlmm_acquire(device_t dev, const void *data, size_t len, int flags)
{
	struct qcom_tlmm_softc * const sc = device_private(dev);
	const u_int *cells = data;
	struct qcom_tlmm_pin *gpin;

	if (len != 12)		/* phandle, pin, flags */
		return NULL;
	const u_int pin = be32toh(cells[1]);
	if (pin >= TLMM_NPINS)
		return NULL;

	mutex_enter(&sc->sc_lock);
	qcom_tlmm_ctl(sc, pin, flags);
	mutex_exit(&sc->sc_lock);

	gpin = kmem_zalloc(sizeof(*gpin), KM_SLEEP);
	gpin->pin_sc = sc;
	gpin->pin_num = pin;
	gpin->pin_actlo = (be32toh(cells[2]) & 1) != 0;	/* GPIO_ACTIVE_LOW */
	return gpin;
}

static void
qcom_tlmm_release(device_t dev, void *priv)
{
	struct qcom_tlmm_pin * const gpin = priv;

	qcom_tlmm_pin_ctl(gpin->pin_sc, gpin->pin_num, GPIO_PIN_INPUT);
	kmem_free(gpin, sizeof(*gpin));
}

static int
qcom_tlmm_read(device_t dev, void *priv, bool raw)
{
	struct qcom_tlmm_pin * const gpin = priv;
	int val = qcom_tlmm_pin_read(gpin->pin_sc, gpin->pin_num);

	if (!raw && gpin->pin_actlo)
		val = !val;
	return val;
}

static void
qcom_tlmm_write(device_t dev, void *priv, int val, bool raw)
{
	struct qcom_tlmm_pin * const gpin = priv;

	if (!raw && gpin->pin_actlo)
		val = !val;
	qcom_tlmm_pin_write(gpin->pin_sc, gpin->pin_num, val);
}

static const struct fdtbus_gpio_controller_func qcom_tlmm_gpio_funcs = {
	.acquire = qcom_tlmm_acquire,
	.release = qcom_tlmm_release,
	.read = qcom_tlmm_read,
	.write = qcom_tlmm_write,
};

/*
 * "gpio-hog" children of the controller node: pins put in a fixed state
 * at attach, e.g. on the FRITZ!Box 7530
 *	usb-power { gpio-hog; gpios = <49 0>; output-high; };
 */
static void
qcom_tlmm_hogs(struct qcom_tlmm_softc *sc)
{
	for (int child = OF_child(sc->sc_phandle); child; child = OF_peer(child)) {
		u_int cells[2];
		const char *name;
		int flags, val = 0;

		if (!of_hasprop(child, "gpio-hog"))
			continue;
		if (of_getprop_uint32_array(child, "gpios", cells, 2) != 0 ||
		    cells[0] >= TLMM_NPINS)
			continue;
		if (of_hasprop(child, "output-high")) {
			flags = GPIO_PIN_OUTPUT;
			val = 1;
		} else if (of_hasprop(child, "output-low"))
			flags = GPIO_PIN_OUTPUT;
		else
			flags = GPIO_PIN_INPUT;
		if (cells[1] & 1)			/* active low */
			val = !val;
		if (flags & GPIO_PIN_OUTPUT)
			qcom_tlmm_pin_write(sc, cells[0], val);	/* level first */
		qcom_tlmm_pin_ctl(sc, cells[0], flags);
		name = fdtbus_get_string(child, "line-name");
		aprint_normal_dev(sc->sc_dev, "GPIO %u %s (%s)\n", cells[0],
		    flags & GPIO_PIN_OUTPUT ? (val ? "high" : "low") : "input",
		    name != NULL ? name : fdtbus_get_string(child, "name"));
	}
}

static int
qcom_tlmm_match(device_t parent, cfdata_t cf, void *aux)
{
	struct fdt_attach_args * const faa = aux;

	return of_compatible_match(faa->faa_phandle, compat_data);
}

static void
qcom_tlmm_attach(device_t parent, device_t self, void *aux)
{
	struct qcom_tlmm_softc * const sc = device_private(self);
	struct fdt_attach_args * const faa = aux;
	struct gpiobus_attach_args gba;
	bus_addr_t addr;
	bus_size_t size;

	sc->sc_dev = self;
	sc->sc_phandle = faa->faa_phandle;
	sc->sc_bst = faa->faa_bst;
	if (fdtbus_get_reg(sc->sc_phandle, 0, &addr, &size) != 0 ||
	    bus_space_map(sc->sc_bst, addr, size, 0, &sc->sc_bsh) != 0) {
		aprint_error(": couldn't map registers\n");
		return;
	}
	mutex_init(&sc->sc_lock, MUTEX_DEFAULT, IPL_VM);

	aprint_naive("\n");
	aprint_normal(": IPQ40xx TLMM, %u GPIOs\n", TLMM_NPINS);

	fdtbus_register_gpio_controller(self, sc->sc_phandle,
	    &qcom_tlmm_gpio_funcs);
	qcom_tlmm_hogs(sc);

	/* gpiobus, for gpioctl(8) */
	sc->sc_gp.gp_cookie = sc;
	sc->sc_gp.gp_pin_read = qcom_tlmm_pin_read;
	sc->sc_gp.gp_pin_write = qcom_tlmm_pin_write;
	sc->sc_gp.gp_pin_ctl = qcom_tlmm_pin_ctl;
	for (u_int pin = 0; pin < TLMM_NPINS; pin++) {
		sc->sc_pins[pin].pin_num = pin;
		sc->sc_pins[pin].pin_caps = GPIO_PIN_INPUT | GPIO_PIN_OUTPUT |
		    GPIO_PIN_PULLUP | GPIO_PIN_PULLDOWN;
		sc->sc_pins[pin].pin_state = qcom_tlmm_pin_read(sc, pin);
	}
	memset(&gba, 0, sizeof(gba));
	gba.gba_gc = &sc->sc_gp;
	gba.gba_pins = sc->sc_pins;
	gba.gba_npins = TLMM_NPINS;
	config_found(self, &gba, NULL, CFARGS_NONE);
}
