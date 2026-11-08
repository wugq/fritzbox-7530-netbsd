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
 * Qualcomm UARTDM v1.4 driver, written for an experiment booting NetBSD
 * on the AVM FRITZ!Box 7530 (IPQ4019).
 *
 * Structure follows arch/arm/amlogic/meson_uart.c.  Register usage follows
 * the U-Boot serial_msm.c / Linux msm_serial.c drivers.  The UART is assumed
 * to have been set up (clocks, baud rate) by the boot loader.
 *
 * Interrupt driven, as FreeBSD's uart_dev_msm.c (RX) and Linux'
 * msm_serial.c (TX):
 *
 * RX: single-character mode (DMEN_RX_SC_ENABLE): every character takes
 *     one FIFO word, so there is no packing, no stale event and no
 *     RX_TOTAL_SNAP bookkeeping (the polled driver had to read only after
 *     stale events and died when a burst overran the FIFO).  RFWR = 0:
 *     an RXLEV interrupt as soon as the FIFO holds a character.  The
 *     interrupt handler moves characters into a ring; a soft interrupt
 *     hands them to the tty layer.
 * TX: transfers of up to TX_CHUNK characters, packed four to a word:
 *     NCF_TX = count, then the words into TF.  TXLEV interrupts when the
 *     FIFO is down to TFWR words, and the next transfer goes in.  The
 *     console (kernel printf, ddb) still writes one character at a time,
 *     busy waiting; one spin lock keeps it and the transfers apart.
 */

#include "opt_console.h"

#include <sys/cdefs.h>
__KERNEL_RCSID(1, "$NetBSD$");

#define cn_trap()			\
	do {				\
		console_debugger();	\
		cn_trapped = 1;		\
	} while (/* CONSTCOND */ 0)

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/device.h>
#include <sys/conf.h>
#include <sys/intr.h>
#include <sys/systm.h>
#include <sys/time.h>
#include <sys/termios.h>
#include <sys/kauth.h>
#include <sys/lwp.h>
#include <sys/tty.h>
#include <sys/callout.h>
#include <sys/lock.h>

#include <ddb/db_active.h>

#include <dev/cons.h>

#include <dev/fdt/fdtvar.h>

#include <uvm/uvm_extern.h>

#include <machine/bootconfig.h>

#include <arm/locore.h>
#include <arm/cpufunc.h>

#include <arch/evbarm/fdt/platform.h>
#include <arm/qcom/qcom_uartreg.h>

static int	qcom_uart_match(device_t, cfdata_t, void *);
static void	qcom_uart_attach(device_t, device_t, void *);

static void	qcom_uart_backstop(void *);
static int	qcom_uart_intr(void *);
static void	qcom_uart_softintr(void *);

static int	qcom_uart_cngetc(dev_t);
static void	qcom_uart_cnputc(dev_t, int);
static void	qcom_uart_cnpollc(dev_t, int);

static void	qcom_uart_start(struct tty *);
static int	qcom_uart_param(struct tty *, struct termios *);

extern struct cfdriver qcomuart_cd;

static const struct device_compatible_entry compat_data[] = {
	{ .compat = "qcom,msm-uartdm-v1.4" },
	DEVICE_COMPAT_EOL
};

#define TX_CHUNK	128	/* characters per TX transfer (32 words) */
#define TX_WATERMARK	16	/* TXLEV when the FIFO is down to 16 words */
#define RX_RING		1024

struct qcom_uart_softc {
	device_t sc_dev;
	bus_space_tag_t	sc_bst;
	bus_space_handle_t sc_bsh;
	void *sc_ih;
	void *sc_si;
	callout_t sc_backstop;

	struct tty *sc_tty;

	int sc_ospeed;
	tcflag_t sc_cflag;

	uint32_t sc_imr;		/* interrupts enabled */
	/* TX: what is left of sc_buf; done flag for the soft interrupt */
	u_char *sc_tba;
	u_int sc_tbc;
	bool sc_tx_done;
	u_char sc_buf[1024];
	/* RX ring, filled by the interrupt handler */
	u_char sc_rbuf[RX_RING];
	u_int sc_rput, sc_rget;
	u_int sc_overruns, sc_rxfull;
};

/*
 * One lock for the hardware and the rings, shared by the console code,
 * the interrupt and the soft interrupt.  A simple spin lock, not a
 * kmutex: the console uses it from the first printf on.
 */
static __cpu_simple_lock_t qcom_uart_hwlock = __SIMPLELOCK_UNLOCKED;

static inline int
qcom_uart_lock(void)
{
	const int s = splserial();
	__cpu_simple_lock(&qcom_uart_hwlock);
	return s;
}

static inline void
qcom_uart_unlock(int s)
{
	__cpu_simple_unlock(&qcom_uart_hwlock);
	splx(s);
}

/* console: don't hang if the lock holder is stuck (panic, ddb) */
static inline int
qcom_uart_cnlock(bool *locked)
{
	const int s = splserial();
	*locked = false;
	for (u_int i = 0; i < 1000000; i++)
		if (__cpu_simple_lock_try(&qcom_uart_hwlock)) {
			*locked = true;
			break;
		}
	return s;
}

static inline void
qcom_uart_cnunlock(int s, bool locked)
{
	if (locked)
		__cpu_simple_unlock(&qcom_uart_hwlock);
	splx(s);
}

static int qcom_uart_console_phandle = -1;
static struct qcom_uart_softc qcom_uart_cnsc;
static struct cnm_state qcom_uart_cnm_state;

struct consdev qcom_uart_consdev = {
	.cn_getc = qcom_uart_cngetc,
	.cn_putc = qcom_uart_cnputc,
	.cn_pollc = qcom_uart_cnpollc,
	.cn_dev = NODEV,
	.cn_pri = CN_NORMAL,
};

static dev_type_open(qcom_uart_open);
static dev_type_open(qcom_uart_close);
static dev_type_read(qcom_uart_read);
static dev_type_write(qcom_uart_write);
static dev_type_ioctl(qcom_uart_ioctl);
static dev_type_tty(qcom_uart_tty);
static dev_type_poll(qcom_uart_poll);
static dev_type_stop(qcom_uart_stop);

const struct cdevsw qcomuart_cdevsw = {
	.d_open = qcom_uart_open,
	.d_close = qcom_uart_close,
	.d_read = qcom_uart_read,
	.d_write = qcom_uart_write,
	.d_ioctl = qcom_uart_ioctl,
	.d_stop = qcom_uart_stop,
	.d_tty = qcom_uart_tty,
	.d_poll = qcom_uart_poll,
	.d_mmap = nommap,
	.d_kqfilter = ttykqfilter,
	.d_discard = nodiscard,
	.d_flag = D_TTY
};

static int qcom_uart_cmajor = -1;

CFATTACH_DECL_NEW(qcom_uart, sizeof(struct qcom_uart_softc),
	qcom_uart_match, qcom_uart_attach, NULL, NULL);

#define RD4(bst, bsh, r)	bus_space_read_4((bst), (bsh), (r))
#define WR4(bst, bsh, r, v)	bus_space_write_4((bst), (bsh), (r), (v))

/* Low level: transmit one character (busy wait). */
static void
qcom_uart_putc_hw(bus_space_tag_t bst, bus_space_handle_t bsh, int c)
{
	u_int timo = 1000000;

	while ((RD4(bst, bsh, UARTDM_SR) & UARTDM_SR_TX_EMPTY) == 0 &&
	    (RD4(bst, bsh, UARTDM_ISR) & UARTDM_ISR_TX_READY) == 0) {
		if (--timo == 0)
			break;
	}
	WR4(bst, bsh, UARTDM_CR, UARTDM_CR_CMD_RESET_TX_READY);
	WR4(bst, bsh, UARTDM_NCF_TX, 1);
	(void)RD4(bst, bsh, UARTDM_NCF_TX);
	WR4(bst, bsh, UARTDM_TF, c & 0xff);
}

/* wait until the previous TX transfer is in the FIFO (or the FIFO empty) */
static void
qcom_uart_tx_wait(bus_space_tag_t bst, bus_space_handle_t bsh)
{
	for (u_int timo = 1000000; timo > 0; timo--)
		if ((RD4(bst, bsh, UARTDM_SR) & UARTDM_SR_TX_EMPTY) ||
		    (RD4(bst, bsh, UARTDM_ISR) & UARTDM_ISR_TX_READY))
			break;
	WR4(bst, bsh, UARTDM_CR, UARTDM_CR_CMD_RESET_TX_READY);
}

/* Low level: receive one character, or -1 if none (lock held). */
static int
qcom_uart_getc_hw(bus_space_tag_t bst, bus_space_handle_t bsh)
{
	if (RD4(bst, bsh, UARTDM_SR) & UARTDM_SR_OVERRUN)
		WR4(bst, bsh, UARTDM_CR, UARTDM_CR_CMD_RESET_ERR);
	if ((RD4(bst, bsh, UARTDM_SR) & UARTDM_SR_RX_READY) == 0)
		return -1;
	return RD4(bst, bsh, UARTDM_RF) & 0xff;	/* single-character mode */
}

static void
qcom_uart_hw_init(bus_space_tag_t bst, bus_space_handle_t bsh)
{
	/* no DMA; RX one character per word; interrupts off for now */
	WR4(bst, bsh, UARTDM_IMR, 0);
	WR4(bst, bsh, UARTDM_DMEN, UARTDM_DMEN_RX_SC_ENABLE);
	WR4(bst, bsh, UARTDM_RFWR, 0);
	WR4(bst, bsh, UARTDM_TFWR, TX_WATERMARK);
	WR4(bst, bsh, UARTDM_CR, UARTDM_CR_CMD_RESET_RX);
	WR4(bst, bsh, UARTDM_CR, UARTDM_CR_CMD_RESET_ERR);
	WR4(bst, bsh, UARTDM_CR, UARTDM_CR_RX_ENABLE | UARTDM_CR_TX_ENABLE);
}

/*
 * Put the next transfer of sc_buf into the TX FIFO (lock held); with
 * nothing left, stop the TXLEV interrupt and tell the soft interrupt.
 */
static void
qcom_uart_tx_fill(struct qcom_uart_softc *sc)
{
	bus_space_tag_t bst = sc->sc_bst;
	bus_space_handle_t bsh = sc->sc_bsh;

	if (sc->sc_tbc == 0) {
		if (sc->sc_imr & UARTDM_ISR_TXLEV) {
			sc->sc_imr &= ~UARTDM_ISR_TXLEV;
			WR4(bst, bsh, UARTDM_IMR, sc->sc_imr);
			sc->sc_tx_done = true;
		}
		return;
	}
	const u_int n = MIN(sc->sc_tbc, TX_CHUNK);
	qcom_uart_tx_wait(bst, bsh);
	WR4(bst, bsh, UARTDM_NCF_TX, n);
	(void)RD4(bst, bsh, UARTDM_NCF_TX);
	for (u_int i = 0; i < n; i += 4) {
		uint32_t w = 0;
		for (u_int k = 0; k < 4 && i + k < n; k++)
			w |= (uint32_t)sc->sc_tba[i + k] << (8 * k);
		/* room in the FIFO (normally there is: TXLEV said so) */
		for (u_int timo = 100000; timo > 0; timo--)
			if (RD4(bst, bsh, UARTDM_SR) & UARTDM_SR_TX_READY)
				break;
		WR4(bst, bsh, UARTDM_TF, w);
	}
	sc->sc_tba += n;
	sc->sc_tbc -= n;
	if ((sc->sc_imr & UARTDM_ISR_TXLEV) == 0) {
		sc->sc_imr |= UARTDM_ISR_TXLEV;
		WR4(bst, bsh, UARTDM_IMR, sc->sc_imr);
	}
}

static int
qcom_uart_intr(void *arg)
{
	struct qcom_uart_softc * const sc = arg;
	bus_space_tag_t bst = sc->sc_bst;
	bus_space_handle_t bsh = sc->sc_bsh;
	bool kick = false;
	int c;

	const int s = qcom_uart_lock();
	const uint32_t misr = RD4(bst, bsh, UARTDM_MISR);
	while ((c = qcom_uart_getc_hw(bst, bsh)) != -1) {
		const u_int next = (sc->sc_rput + 1) % RX_RING;
		if (next == sc->sc_rget) {
			sc->sc_rxfull++;	/* tty side too slow: drop */
			continue;
		}
		sc->sc_rbuf[sc->sc_rput] = c;
		sc->sc_rput = next;
		kick = true;
	}
	if (misr & UARTDM_ISR_TXLEV)
		qcom_uart_tx_fill(sc);
	if (sc->sc_tx_done)
		kick = true;
	qcom_uart_unlock(s);

	if (kick)
		softint_schedule(sc->sc_si);
	return misr != 0;
}

/* tty work, out of the hard interrupt: received characters, TX done */
static void
qcom_uart_softintr(void *arg)
{
	struct qcom_uart_softc * const sc = arg;
	struct tty * const tp = sc->sc_tty;
	u_char buf[64];
	u_int n;
	bool done;

	do {
		int s = qcom_uart_lock();
		for (n = 0; n < sizeof(buf) && sc->sc_rget != sc->sc_rput; n++) {
			buf[n] = sc->sc_rbuf[sc->sc_rget];
			sc->sc_rget = (sc->sc_rget + 1) % RX_RING;
		}
		done = sc->sc_tx_done;
		sc->sc_tx_done = false;
		qcom_uart_unlock(s);

		for (u_int i = 0; i < n; i++) {
			int cn_trapped = 0;
			cn_check_magic(tp->t_dev, buf[i], qcom_uart_cnm_state);
			if (cn_trapped)
				continue;
			tp->t_linesw->l_rint(buf[i], tp);
		}
		if (done) {
			/*
			 * t_state and the output queue belong to the tty
			 * layer: only with tty_lock (as ttwrite() calls
			 * ttstart()).  Without it qcom_uart_start()'s q_to_b()
			 * raced a writer's b_to_q() and corrupted the clist.
			 */
			mutex_spin_enter(&tty_lock);
			CLR(tp->t_state, TS_BUSY | TS_FLUSH);
			tp->t_linesw->l_start(tp);
			mutex_spin_exit(&tty_lock);
		}
	} while (n == sizeof(buf));
}

/* a slow safety net, in case an interrupt is ever lost */
static void
qcom_uart_backstop(void *arg)
{
	struct qcom_uart_softc * const sc = arg;

	qcom_uart_intr(sc);
	callout_schedule(&sc->sc_backstop, hz);
}

static int
qcom_uart_match(device_t parent, cfdata_t cf, void *aux)
{
	struct fdt_attach_args * const faa = aux;

	return of_compatible_match(faa->faa_phandle, compat_data);
}

static void
qcom_uart_attach(device_t parent, device_t self, void *aux)
{
	struct qcom_uart_softc * const sc = device_private(self);
	struct fdt_attach_args * const faa = aux;
	const int phandle = faa->faa_phandle;
	bus_addr_t addr;
	bus_size_t size;
	struct tty *tp;
	int major, minor, error;

	if (fdtbus_get_reg(phandle, 0, &addr, &size) != 0) {
		aprint_error(": couldn't get registers\n");
		return;
	}

	sc->sc_dev = self;
	sc->sc_bst = faa->faa_bst;

	if (qcom_uart_console_phandle == phandle) {
		sc->sc_bsh = qcom_uart_cnsc.sc_bsh;
	} else {
		error = bus_space_map(sc->sc_bst, addr, size, 0, &sc->sc_bsh);
		if (error != 0) {
			aprint_error(": couldn't map registers\n");
			return;
		}
		qcom_uart_hw_init(sc->sc_bst, sc->sc_bsh);
	}

	callout_init(&sc->sc_backstop, CALLOUT_MPSAFE);
	callout_setfunc(&sc->sc_backstop, qcom_uart_backstop, sc);
	sc->sc_si = softint_establish(SOFTINT_SERIAL | SOFTINT_MPSAFE,
	    qcom_uart_softintr, sc);

	if (qcom_uart_cmajor == -1) {
		int bmajor = -1, cmajor = -1;
		error = devsw_attach("qcomuart", NULL, &bmajor,
		    &qcomuart_cdevsw, &cmajor);
		if (error) {
			aprint_error(": couldn't allocate major number\n");
			return;
		}
		qcom_uart_cmajor = cmajor;
	}

	major = cdevsw_lookup_major(&qcomuart_cdevsw);
	minor = device_unit(self);

	tp = sc->sc_tty = tty_alloc();
	tp->t_oproc = qcom_uart_start;
	tp->t_param = qcom_uart_param;
	tp->t_dev = makedev(major, minor);
	tp->t_sc = sc;
	tty_attach(tp);

	aprint_naive("\n");
	if (qcom_uart_console_phandle == phandle) {
		cn_tab->cn_dev = tp->t_dev;
		aprint_normal(": console");
	}
	aprint_normal(": Qualcomm UARTDM v1.4\n");

	char intrstr[128];
	if (!fdtbus_intr_str(phandle, 0, intrstr, sizeof(intrstr)) ||
	    (sc->sc_ih = fdtbus_intr_establish_xname(phandle, 0, IPL_SERIAL,
	    FDT_INTR_MPSAFE, qcom_uart_intr, sc, device_xname(self))) == NULL) {
		aprint_error_dev(self, "couldn't establish interrupt, "
		    "polling once a second only\n");
	} else
		aprint_normal_dev(self, "interrupting on %s\n", intrstr);

	/* RX interrupts on from now on (it is the console) */
	const int s = qcom_uart_lock();
	sc->sc_imr = UARTDM_ISR_RXLEV;
	WR4(sc->sc_bst, sc->sc_bsh, UARTDM_IMR, sc->sc_imr);
	qcom_uart_unlock(s);
	callout_schedule(&sc->sc_backstop, hz);
}

static int
qcom_uart_cngetc(dev_t dev)
{
	struct qcom_uart_softc *sc = &qcom_uart_cnsc;
	bool locked;
	int s, c;

	s = qcom_uart_cnlock(&locked);
	c = qcom_uart_getc_hw(sc->sc_bst, sc->sc_bsh);
	qcom_uart_cnunlock(s, locked);
	if (c != -1 && !db_active) {
		int cn_trapped __unused = 0;
		cn_check_magic(dev, c, qcom_uart_cnm_state);
	}

	return c;
}

static void
qcom_uart_cnputc(dev_t dev, int c)
{
	struct qcom_uart_softc *sc = &qcom_uart_cnsc;
	bool locked;
	int s;

	s = qcom_uart_cnlock(&locked);
	qcom_uart_putc_hw(sc->sc_bst, sc->sc_bsh, c);
	qcom_uart_cnunlock(s, locked);
}

static void
qcom_uart_cnpollc(dev_t dev, int on)
{
}

static int
qcom_uart_open(dev_t dev, int flag, int mode, lwp_t *l)
{
	struct qcom_uart_softc *sc =
	    device_lookup_private(&qcomuart_cd, minor(dev));
	struct tty *tp;

	if (sc == NULL)
		return ENXIO;
	tp = sc->sc_tty;

	if (kauth_authorize_device_tty(l->l_cred,
	    KAUTH_DEVICE_TTY_OPEN, tp) != 0) {
		return EBUSY;
	}

	if ((tp->t_state & TS_ISOPEN) == 0 && tp->t_wopen == 0) {
		tp->t_dev = dev;
		ttychars(tp);
		tp->t_iflag = TTYDEF_IFLAG;
		tp->t_oflag = TTYDEF_OFLAG;
		tp->t_cflag = TTYDEF_CFLAG;
		tp->t_lflag = TTYDEF_LFLAG;
		tp->t_ispeed = tp->t_ospeed = 115200;
		ttsetwater(tp);
	}
	tp->t_state |= TS_CARR_ON;

	return tp->t_linesw->l_open(dev, tp);
}

static int
qcom_uart_close(dev_t dev, int flag, int mode, lwp_t *l)
{
	struct qcom_uart_softc *sc =
	    device_lookup_private(&qcomuart_cd, minor(dev));
	struct tty *tp = sc->sc_tty;

	tp->t_linesw->l_close(tp, flag);
	ttyclose(tp);


	return 0;
}

static int
qcom_uart_read(dev_t dev, struct uio *uio, int flag)
{
	struct qcom_uart_softc *sc =
	    device_lookup_private(&qcomuart_cd, minor(dev));
	struct tty *tp = sc->sc_tty;

	return tp->t_linesw->l_read(tp, uio, flag);
}

static int
qcom_uart_write(dev_t dev, struct uio *uio, int flag)
{
	struct qcom_uart_softc *sc =
	    device_lookup_private(&qcomuart_cd, minor(dev));
	struct tty *tp = sc->sc_tty;

	return tp->t_linesw->l_write(tp, uio, flag);
}

static int
qcom_uart_poll(dev_t dev, int events, lwp_t *l)
{
	struct qcom_uart_softc *sc =
	    device_lookup_private(&qcomuart_cd, minor(dev));
	struct tty *tp = sc->sc_tty;

	return tp->t_linesw->l_poll(tp, events, l);
}

static int
qcom_uart_ioctl(dev_t dev, u_long cmd, void *data, int flag, lwp_t *l)
{
	struct qcom_uart_softc *sc =
	    device_lookup_private(&qcomuart_cd, minor(dev));
	struct tty *tp = sc->sc_tty;
	int error;

	error = tp->t_linesw->l_ioctl(tp, cmd, data, flag, l);
	if (error != EPASSTHROUGH)
		return error;

	return ttioctl(tp, cmd, data, flag, l);
}

static struct tty *
qcom_uart_tty(dev_t dev)
{
	struct qcom_uart_softc *sc =
	    device_lookup_private(&qcomuart_cd, minor(dev));

	return sc->sc_tty;
}

static void
qcom_uart_stop(struct tty *tp, int flag)
{
}

/*
 * t_oproc.  The tty layer calls it mostly with tty_lock held (ttwrite);
 * the TX-done path below takes tty_lock before calling it.
 */
static void
qcom_uart_start(struct tty *tp)
{
	struct qcom_uart_softc *sc = tp->t_sc;
	int s;

	if (ISSET(tp->t_state, TS_BUSY | TS_TIMEOUT | TS_TTSTOP) ||
	    !ttypull(tp))
		return;
	const u_int n = q_to_b(&tp->t_outq, sc->sc_buf, sizeof(sc->sc_buf));
	SET(tp->t_state, TS_BUSY);

	/* the interrupt handler sends the rest and clears TS_BUSY */
	s = qcom_uart_lock();
	sc->sc_tba = sc->sc_buf;
	sc->sc_tbc = n;
	qcom_uart_tx_fill(sc);
	qcom_uart_unlock(s);
}

static int
qcom_uart_param(struct tty *tp, struct termios *t)
{
	/* baud rate etc. are left as programmed by the boot loader */
	tp->t_ispeed = t->c_ispeed;
	tp->t_ospeed = t->c_ospeed;
	tp->t_cflag = t->c_cflag;

	return 0;
}

static int
qcom_uart_console_match(int phandle)
{
	return of_compatible_match(phandle, compat_data);
}

static void
qcom_uart_console_consinit(struct fdt_attach_args *faa, u_int uart_freq)
{
	struct qcom_uart_softc *sc = &qcom_uart_cnsc;
	const int phandle = faa->faa_phandle;
	bus_addr_t addr;
	bus_size_t size;
	int error;

	fdtbus_get_reg(phandle, 0, &addr, &size);

	sc->sc_bst = faa->faa_bst;
	sc->sc_ospeed = 115200;
	sc->sc_cflag = fdtbus_get_stdout_flags();

	error = bus_space_map(sc->sc_bst, addr, size, 0, &sc->sc_bsh);
	if (error != 0)
		panic("failed to map console, error = %d", error);

	qcom_uart_hw_init(sc->sc_bst, sc->sc_bsh);

	cn_tab = &qcom_uart_consdev;
	cn_init_magic(&qcom_uart_cnm_state);
	cn_set_magic("\047\001");

	qcom_uart_console_phandle = phandle;
}

static const struct fdt_console qcom_uart_console = {
	.match = qcom_uart_console_match,
	.consinit = qcom_uart_console_consinit,
};

FDT_CONSOLE(qcom_uart, &qcom_uart_console);

/*
 * Early console for VERBOSE_INIT_ARM: options EARLYCONS=qcom, CONSADDR=...
 * The default arm platform devmap maps the stdout UART page at
 * KERNEL_IO_VBASE (see arch/arm/fdt/arm_platform.c).
 */
void qcom_platform_early_putchar(char);

void __noasan
qcom_platform_early_putchar(char c)
{
#ifdef CONSADDR
	const bus_addr_t uart_base = CONSADDR;
	volatile uint32_t *u = cpu_earlydevice_va_p() ?
	    (volatile uint32_t *)((uart_base - DEVMAP_ALIGN(uart_base)) +
		KERNEL_IO_VBASE) :
	    (volatile uint32_t *)uart_base;
	u_int timo = 1000000;

	while ((u[UARTDM_SR / 4] & UARTDM_SR_TX_EMPTY) == 0 &&
	    (u[UARTDM_ISR / 4] & UARTDM_ISR_TX_READY) == 0) {
		if (--timo == 0)
			break;
	}
	u[UARTDM_CR / 4] = UARTDM_CR_CMD_RESET_TX_READY;
	u[UARTDM_NCF_TX / 4] = 1;
	(void)u[UARTDM_NCF_TX / 4];
	u[UARTDM_TF / 4] = (uint8_t)c;
	dsb(sy);
#endif
}
