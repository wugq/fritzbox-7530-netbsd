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
 * Qualcomm IPQ40xx QPIC NAND controller ("qcom,ipq4019-nand"): NetBSD
 * glue for the OS-independent pieces:
 *
 *	qpic_nand.c		the controller, driven by register writes
 *	dev/ubi/ubi_lite.c	the UBI on-flash format
 *
 * At attach: enable the clocks, save the boot loader's NAND configuration,
 * reset the block (that takes it out of BAM mode, where the CPU could not
 * write its registers), restore the configuration, read the chip ID, and
 * scan the UBI partition (from the device tree's fixed-partitions, label
 * "ubi").  The UBI volumes are character devices: minor number = volume
 * id, e.g. "mknod /dev/ubivol3 c <major> 3".  Only the volumes named
 * "config" (the saved settings) and "upgrade" (a new system being
 * installed, sysupgrade(8)) can be written, a LEB at a time,
 * read-modify-write, as UBI's atomic LEB change; the others hold boot
 * images and stay read-only.
 *
 * Minor 255 is the whole chip, read-only: every page with ECC applied, as
 * Linux's /dev/mtdN gives it, for backups.  Pages that fail ECC (a bad
 * block) are returned as read and reported on the console.
 *
 * sysctl hw.qcomqpic0:
 *	ubi_volumes		the volumes: "id name LEBs" per line
 *	ubi_remove=<name>	remove a volume, as Linux' ubirmvol does.  The
 *				boot loader (U-Boot 2012.07) cannot: this
 *				makes room on a box that still has the
 *				vendor's system in its UBI partition.
 *	ubi_create="<name> <LEBs>"	create a dynamic volume
 *	ubi_swap="<name> <name>"	swap two volumes' names, in one
 *				write of the volume table
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/conf.h>
#include <sys/device.h>
#include <sys/fcntl.h>
#include <sys/kmem.h>
#include <sys/mutex.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <sys/uio.h>

#include <dev/fdt/fdtvar.h>

#include <arm/qcom/qpic_nandvar.h>
#include <dev/ubi/ubi_lite.h>

#define NAND_VERSION		0xf08
#define NAND_CTRL		0xf00

struct qpic_softc {
	device_t		sc_dev;
	bus_space_tag_t		sc_bst;
	bus_space_handle_t	sc_bsh;
	kmutex_t		sc_lock;	/* one operation at a time */
	struct qpic_nand	sc_nand;
	struct ubi_lite		sc_ubi;
	bool			sc_ubi_ok;
	bool			sc_nand_ok;	/* the chip answered */
	uint8_t			*sc_leb;	/* one LEB, for read() */
	uint8_t			*sc_page;	/* one page, for the chip device */
	struct sysctllog	*sc_sysctllog;
};

static int	qpic_match(device_t, cfdata_t, void *);
static void	qpic_attach(device_t, device_t, void *);

static const struct device_compatible_entry compat_data[] = {
	{ .compat = "qcom,ipq4019-nand" },
	DEVICE_COMPAT_EOL
};

CFATTACH_DECL_NEW(qcom_qpic, sizeof(struct qpic_softc),
    qpic_match, qpic_attach, NULL, NULL);

extern struct cfdriver qcomqpic_cd;

/* the volumes that may be written: settings, and a system being installed */
static bool
ubivol_writable(const char *name)
{
	return strcmp(name, "config") == 0 || strcmp(name, "upgrade") == 0;
}
#define UBIVOL_CHIP	255		/* minor: the whole chip, read-only */
#define QPIC_CHIP_SIZE	(128U * 1024 * 1024)	/* 1 Gbit (id c2 f1) */
static dev_type_open(ubivol_open);
static dev_type_read(ubivol_read);
static dev_type_write(ubivol_write);

static const struct cdevsw ubivol_cdevsw = {
	.d_open = ubivol_open,
	.d_close = nullclose,
	.d_read = ubivol_read,
	.d_write = ubivol_write,
	.d_ioctl = noioctl,
	.d_stop = nostop,
	.d_tty = notty,
	.d_poll = nopoll,
	.d_mmap = nommap,
	.d_kqfilter = nokqfilter,
	.d_discard = nodiscard,
	.d_flag = D_OTHER | D_MPSAFE,
};

/* --- callbacks for the OS-independent code ------------------------ */

static uint32_t
qpic_rd(void *arg, uint32_t r)
{
	struct qpic_softc * const sc = arg;
	return bus_space_read_4(sc->sc_bst, sc->sc_bsh, r);
}

static void
qpic_wr(void *arg, uint32_t r, uint32_t v)
{
	struct qpic_softc * const sc = arg;
	bus_space_write_4(sc->sc_bst, sc->sc_bsh, r, v);
}

static void
qpic_delay(unsigned int us)
{
	delay(us);
}

static int
qpic_ubi_read(void *arg, uint32_t page, uint8_t *buf)
{
	struct qpic_softc * const sc = arg;
	const int error = qpic_nand_read_page(&sc->sc_nand, page, buf, NULL);

	return error == QPIC_ERASED ? UBI_READ_ERASED : error;
}

static int
qpic_ubi_program(void *arg, uint32_t page, const uint8_t *buf)
{
	struct qpic_softc * const sc = arg;
	const int error = qpic_nand_program_page(&sc->sc_nand, page, buf);

	if (error != 0)
		device_printf(sc->sc_dev, "program page %#x: error %d, status "
		    "%#x\n", page, error, sc->sc_nand.qn_status);
	return error;
}

static int
qpic_ubi_erase(void *arg, uint32_t block)
{
	struct qpic_softc * const sc = arg;
	const int error = qpic_nand_erase_block(&sc->sc_nand, block);

	if (error != 0)
		device_printf(sc->sc_dev, "erase block %u: error %d, status "
		    "%#x\n", block, error, sc->sc_nand.qn_status);
	return error;
}

static bool
qpic_ubi_bad(void *arg, uint32_t block)
{
	struct qpic_softc * const sc = arg;
	return qpic_nand_block_bad(&sc->sc_nand, block);
}

/* --- sysctl --------------------------------------------------------- */

/*
 * The writable string sysctls: copy in what was written, then run op on it
 * with the lock held.  Reading them gives "".
 */
static int
qpic_sysctl_op(SYSCTLFN_ARGS, int (*op)(struct qpic_softc *, char *))
{
	struct sysctlnode node = *rnode;
	struct qpic_softc * const sc = rnode->sysctl_data;
	char arg[160];
	int error;

	arg[0] = '\0';
	node.sysctl_data = arg;
	node.sysctl_size = sizeof(arg);
	error = sysctl_lookup(SYSCTLFN_CALL(&node));
	if (error || newp == NULL)
		return error;
	if (!sc->sc_ubi_ok)
		return ENXIO;
	mutex_enter(&sc->sc_lock);
	error = op(sc, arg);
	mutex_exit(&sc->sc_lock);
	return error;
}

/* split "a b" into two words */
static int
two_words(char *arg, char **a, char **b)
{
	char *sp = strchr(arg, ' ');

	if (sp == NULL)
		return EINVAL;
	*sp = '\0';
	*a = arg;
	*b = sp + 1;
	return **a == '\0' || **b == '\0' || strchr(*b, ' ') != NULL ?
	    EINVAL : 0;
}

static int
op_remove(struct qpic_softc *sc, char *name)
{
	const int vol = ubi_lite_lookup(&sc->sc_ubi, name);
	int error;

	if (vol < 0)
		return ENOENT;
	error = ubi_lite_remove(&sc->sc_ubi, vol, sc->sc_leb);
	device_printf(sc->sc_dev, "UBI volume %d \"%s\": %s (%d)\n", vol,
	    name, error == 0 ? "removed" : "remove failed", error);
	return error;
}

static int
op_create(struct qpic_softc *sc, char *arg)
{
	char *name, *num, *end;
	uint32_t id;
	int error;

	if ((error = two_words(arg, &name, &num)) != 0)
		return error;
	const unsigned long lebs = strtoul(num, &end, 10);
	if (*end != '\0' || lebs == 0 || lebs > sc->sc_ubi.ul_npebs)
		return EINVAL;
	error = ubi_lite_create(&sc->sc_ubi, name, lebs, &id, sc->sc_leb);
	if (error == 0)
		device_printf(sc->sc_dev, "UBI volume %u \"%s\": created, "
		    "%lu LEBs\n", id, name, lebs);
	else
		device_printf(sc->sc_dev, "UBI volume \"%s\": create failed "
		    "(%d)\n", name, error);
	return error;
}

static int
op_swap(struct qpic_softc *sc, char *arg)
{
	char *na, *nb;
	int a, b, error;

	if ((error = two_words(arg, &na, &nb)) != 0)
		return error;
	if ((a = ubi_lite_lookup(&sc->sc_ubi, na)) < 0 ||
	    (b = ubi_lite_lookup(&sc->sc_ubi, nb)) < 0)
		return ENOENT;
	error = ubi_lite_swap_names(&sc->sc_ubi, a, b, sc->sc_leb);
	device_printf(sc->sc_dev, "UBI volumes %d and %d: names \"%s\" and "
	    "\"%s\" %s (%d)\n", a, b, na, nb,
	    error == 0 ? "swapped" : "swap failed", error);
	return error;
}

static int
qpic_sysctl_remove(SYSCTLFN_ARGS)
{
	return qpic_sysctl_op(SYSCTLFN_CALL(rnode), op_remove);
}

static int
qpic_sysctl_create(SYSCTLFN_ARGS)
{
	return qpic_sysctl_op(SYSCTLFN_CALL(rnode), op_create);
}

static int
qpic_sysctl_swap(SYSCTLFN_ARGS)
{
	return qpic_sysctl_op(SYSCTLFN_CALL(rnode), op_swap);
}

/* hw.qcomqpic0.ubi_volumes: "id name LEBs" per line */
static int
qpic_sysctl_volumes(SYSCTLFN_ARGS)
{
	struct sysctlnode node = *rnode;
	struct qpic_softc * const sc = rnode->sysctl_data;
	struct ubi_lite * const u = &sc->sc_ubi;
	char *str;
	size_t o = 0;
	const size_t len = UBI_LITE_MAXVOL * 96;
	int error;

	str = kmem_zalloc(len, KM_SLEEP);
	mutex_enter(&sc->sc_lock);
	for (u_int v = 0; sc->sc_ubi_ok && v < UBI_LITE_MAXVOL; v++)
		if (u->ul_vol[v].v_reserved_pebs != 0)
			o += snprintf(str + o, len - o, "%u %s %u\n", v,
			    u->ul_vol[v].v_name, u->ul_vol[v].v_reserved_pebs);
	mutex_exit(&sc->sc_lock);
	node.sysctl_data = str;
	node.sysctl_size = len;
	error = sysctl_lookup(SYSCTLFN_CALL(&node));
	kmem_free(str, len);
	return error;
}

static void
qpic_sysctl_attach(struct qpic_softc *sc)
{
	struct sysctllog **log = &sc->sc_sysctllog;
	const struct sysctlnode *node;

	if (sysctl_createv(log, 0, NULL, &node, 0, CTLTYPE_NODE,
	    device_xname(sc->sc_dev), SYSCTL_DESCR("NAND controller"),
	    NULL, 0, NULL, 0, CTL_HW, CTL_CREATE, CTL_EOL) != 0)
		return;
	sysctl_createv(log, 0, &node, NULL, CTLFLAG_READONLY, CTLTYPE_STRING,
	    "ubi_volumes", SYSCTL_DESCR("UBI volumes: id, name, LEBs"),
	    qpic_sysctl_volumes, 0, (void *)sc, 0, CTL_CREATE, CTL_EOL);
	sysctl_createv(log, 0, &node, NULL, CTLFLAG_READWRITE, CTLTYPE_STRING,
	    "ubi_remove", SYSCTL_DESCR("write a volume's name to remove it"),
	    qpic_sysctl_remove, 0, (void *)sc, 160, CTL_CREATE, CTL_EOL);
	sysctl_createv(log, 0, &node, NULL, CTLFLAG_READWRITE, CTLTYPE_STRING,
	    "ubi_create", SYSCTL_DESCR("write \"name LEBs\" to create a "
	    "dynamic volume"),
	    qpic_sysctl_create, 0, (void *)sc, 160, CTL_CREATE, CTL_EOL);
	sysctl_createv(log, 0, &node, NULL, CTLFLAG_READWRITE, CTLTYPE_STRING,
	    "ubi_swap", SYSCTL_DESCR("write \"name name\" to swap two "
	    "volumes' names"),
	    qpic_sysctl_swap, 0, (void *)sc, 160, CTL_CREATE, CTL_EOL);
}

/* --- UBI volumes as character devices ------------------------------ */

static int
ubivol_open(dev_t dev, int flag, int mode, struct lwp *l)
{
	struct qpic_softc * const sc = device_lookup_private(&qcomqpic_cd, 0);
	const u_int vol = minor(dev);

	if (vol == UBIVOL_CHIP) {
		if (sc == NULL || !sc->sc_nand_ok)
			return ENXIO;
		return (flag & FWRITE) ? EROFS : 0;
	}
	if (sc == NULL || !sc->sc_ubi_ok)
		return ENXIO;
	if (vol >= UBI_LITE_MAXVOL || sc->sc_ubi.ul_vol[vol].v_reserved_pebs == 0)
		return ENXIO;
	if ((flag & FWRITE) && !ubivol_writable(sc->sc_ubi.ul_vol[vol].v_name))
		return EROFS;
	return 0;
}

/* minor UBIVOL_CHIP: the raw chip, a page at a time, ECC corrected */
static int
chip_read(struct qpic_softc *sc, struct uio *uio)
{
	int error = 0;

	while (uio->uio_resid > 0 && uio->uio_offset < QPIC_CHIP_SIZE) {
		const uint32_t page = uio->uio_offset / QPIC_PAGE_SIZE;
		const uint32_t off = uio->uio_offset % QPIC_PAGE_SIZE;
		const size_t n = MIN(uio->uio_resid, QPIC_PAGE_SIZE - off);

		mutex_enter(&sc->sc_lock);
		error = qpic_nand_read_page(&sc->sc_nand, page, sc->sc_page,
		    NULL);
		mutex_exit(&sc->sc_lock);
		if (error == ETIMEDOUT)
			return EIO;
		if (error > 0)		/* uncorrectable: keep what was read */
			device_printf(sc->sc_dev, "page %#x (offset %#x): "
			    "ECC error\n", page, page * QPIC_PAGE_SIZE);
		if ((error = uiomove(sc->sc_page + off, n, uio)) != 0)
			break;
	}
	return error;
}

static int
ubivol_read(dev_t dev, struct uio *uio, int flag)
{
	struct qpic_softc * const sc = device_lookup_private(&qcomqpic_cd, 0);
	struct ubi_lite * const u = &sc->sc_ubi;
	const u_int vol = minor(dev);

	if (vol == UBIVOL_CHIP)
		return chip_read(sc, uio);
	const uint64_t size = (uint64_t)u->ul_vol[vol].v_reserved_pebs *
	    u->ul_leb_size;
	int error = 0;

	while (uio->uio_resid > 0 && (uint64_t)uio->uio_offset < size) {
		const uint32_t lnum = uio->uio_offset / u->ul_leb_size;
		const uint32_t off = uio->uio_offset % u->ul_leb_size;
		const size_t n = MIN(uio->uio_resid, u->ul_leb_size - off);

		mutex_enter(&sc->sc_lock);
		error = ubi_lite_read_leb(u, vol, lnum, off, sc->sc_leb, n);
		mutex_exit(&sc->sc_lock);
		if (error != 0)
			break;
		if ((error = uiomove(sc->sc_leb, n, uio)) != 0)
			break;
	}
	return error;
}

static int
ubivol_write(dev_t dev, struct uio *uio, int flag)
{
	struct qpic_softc * const sc = device_lookup_private(&qcomqpic_cd, 0);
	struct ubi_lite * const u = &sc->sc_ubi;
	const u_int vol = minor(dev);
	if (vol == UBIVOL_CHIP)
		return EROFS;
	const uint64_t size = (uint64_t)u->ul_vol[vol].v_reserved_pebs *
	    u->ul_leb_size;
	int error = 0;

	if (vol >= UBI_LITE_MAXVOL || !ubivol_writable(u->ul_vol[vol].v_name))
		return EROFS;
	while (uio->uio_resid > 0) {
		if ((uint64_t)uio->uio_offset >= size)
			return ENOSPC;
		const uint32_t lnum = uio->uio_offset / u->ul_leb_size;
		const uint32_t off = uio->uio_offset % u->ul_leb_size;
		const size_t n = MIN(uio->uio_resid, u->ul_leb_size - off);
		uint32_t len;

		mutex_enter(&sc->sc_lock);
		/* the whole LEB: keep what is around the part written */
		error = ubi_lite_read_leb(u, vol, lnum, 0, sc->sc_leb,
		    u->ul_leb_size);
		if (error == 0)
			error = uiomove(sc->sc_leb + off, n, uio);
		if (error == 0) {
			/* program only up to the last byte that isn't 0xff */
			for (len = u->ul_leb_size; len > 0 &&
			    sc->sc_leb[len - 1] == 0xff; len--)
				continue;
			error = ubi_lite_write_leb(u, vol, lnum, sc->sc_leb,
			    len);
			if (error != 0)
				device_printf(sc->sc_dev, "LEB %u write: error "
				    "%d (PEB %u, offset %u: read %d, want %#x "
				    "got %#x)\n", lnum, error, u->ul_fail_peb,
				    u->ul_fail_off, u->ul_fail_error,
				    u->ul_fail_want, u->ul_fail_got);
		}
		mutex_exit(&sc->sc_lock);
		if (error != 0)
			break;
	}
	return error;
}

/* --- autoconf ------------------------------------------------------ */

/* the "ubi" partition of the device tree: offset and size in bytes */
static bool
qpic_ubi_partition(int phandle, bus_addr_t *off, bus_size_t *size)
{
	for (int chip = OF_child(phandle); chip; chip = OF_peer(chip)) {
		const int parts = of_find_firstchild_byname(chip, "partitions");
		if (parts <= 0)
			continue;
		for (int p = OF_child(parts); p; p = OF_peer(p)) {
			const char *label = fdtbus_get_string(p, "label");
			if (label != NULL && strcmp(label, "ubi") == 0 &&
			    fdtbus_get_reg(p, 0, off, size) == 0)
				return true;
		}
	}
	return false;
}

static int
qpic_match(device_t parent, cfdata_t cf, void *aux)
{
	struct fdt_attach_args * const faa = aux;

	return of_compatible_match(faa->faa_phandle, compat_data);
}

static void
qpic_attach(device_t parent, device_t self, void *aux)
{
	struct qpic_softc * const sc = device_private(self);
	struct fdt_attach_args * const faa = aux;
	const int phandle = faa->faa_phandle;
	struct qpic_nand * const qn = &sc->sc_nand;
	struct ubi_lite * const u = &sc->sc_ubi;
	struct fdtbus_reset *rst;
	struct clk *clk;
	bus_addr_t addr, uoff;
	bus_size_t size, usize;
	uint32_t id;
	int error;

	sc->sc_dev = self;
	sc->sc_bst = faa->faa_bst;
	if (fdtbus_get_reg(phandle, 0, &addr, &size) != 0 ||
	    bus_space_map(sc->sc_bst, addr, size, 0, &sc->sc_bsh) != 0) {
		aprint_error(": couldn't map registers\n");
		return;
	}
	for (u_int i = 0; (clk = fdtbus_clock_get_index(phandle, i)) != NULL;
	    i++)
		clk_enable(clk);
	if ((rst = fdtbus_reset_get(phandle, "qpic")) == NULL) {
		aprint_error(": no \"qpic\" reset\n");
		return;
	}
	mutex_init(&sc->sc_lock, MUTEX_DEFAULT, IPL_NONE);

	const uint32_t ver = qpic_rd(sc, NAND_VERSION);
	aprint_naive("\n");
	aprint_normal(": QPIC NAND controller v%u.%u\n", ver >> 28,
	    (ver >> 16) & 0xfff);

	/* out of BAM mode: save the boot loader's setup, reset, restore */
	qn->qn_arg = sc;
	qn->qn_read = qpic_rd;
	qn->qn_write = qpic_wr;
	qn->qn_delay = qpic_delay;
	const uint32_t ctrl = qpic_rd(sc, NAND_CTRL);
	qpic_nand_save(qn);
	fdtbus_reset_assert(rst);
	delay(100);
	fdtbus_reset_deassert(rst);
	delay(1000);
	qpic_nand_restore(qn);
	if (qpic_nand_read_id(qn, &id) != 0) {
		aprint_error_dev(self, "no answer to READ ID\n");
		return;
	}
	aprint_normal_dev(self, "NAND id %02x %02x %02x %02x (%s), boot "
	    "loader used %s mode\n", id & 0xff, (id >> 8) & 0xff,
	    (id >> 16) & 0xff, id >> 24,
	    (id & 0xff) == 0xc2 ? "Macronix" : "?",
	    (ctrl & 1) ? "BAM" : "register");
	sc->sc_page = kmem_alloc(QPIC_PAGE_SIZE, KM_SLEEP);
	sc->sc_nand_ok = true;

	if (!qpic_ubi_partition(phandle, &uoff, &usize)) {
		aprint_error_dev(self, "no \"ubi\" partition in the device "
		    "tree\n");
		goto devsw;
	}
	u->ul_arg = sc;
	u->ul_read_page = qpic_ubi_read;
	u->ul_program_page = qpic_ubi_program;
	u->ul_erase_block = qpic_ubi_erase;
	u->ul_block_bad = qpic_ubi_bad;
	u->ul_first_peb = uoff / QPIC_BLOCK_SIZE;
	u->ul_npebs = usize / QPIC_BLOCK_SIZE;
	u->ul_page_size = QPIC_PAGE_SIZE;
	u->ul_pages_per_peb = QPIC_PAGES_PER_BLOCK;
	u->ul_peb = kmem_zalloc(u->ul_npebs * sizeof(*u->ul_peb), KM_SLEEP);
	u->ul_buf = kmem_alloc(QPIC_PAGE_SIZE, KM_SLEEP);
	if ((error = ubi_lite_scan(u)) != 0) {
		aprint_error_dev(self, "UBI scan failed (%d)\n", error);
		goto devsw;
	}
	sc->sc_leb = kmem_alloc(u->ul_leb_size, KM_SLEEP);
	sc->sc_ubi_ok = true;
	aprint_normal_dev(self, "UBI: %u PEBs (%u used, %u free, %u bad, %u "
	    "corrupt), LEB %u bytes, max sqnum %llu\n", u->ul_npebs,
	    u->ul_nused, u->ul_nfree, u->ul_nbad, u->ul_ncorrupt,
	    u->ul_leb_size, (unsigned long long)u->ul_max_sqnum);
	for (u_int v = 0; v < UBI_LITE_MAXVOL; v++) {
		if (u->ul_vol[v].v_reserved_pebs == 0)
			continue;
		aprint_normal_dev(self, "UBI volume %u \"%s\": %u LEBs, %u "
		    "with data, %s\n", v, u->ul_vol[v].v_name,
		    u->ul_vol[v].v_reserved_pebs, ubi_lite_mapped(u, v),
		    u->ul_vol[v].v_type == 1 ? "dynamic" : "static");
	}

	qpic_sysctl_attach(sc);

devsw:;
	int bmajor = -1, cmajor = -1;
	if (devsw_attach("ubivol", NULL, &bmajor, &ubivol_cdevsw,
	    &cmajor) == 0)
		aprint_normal_dev(self, "volumes: character major %d, minor = "
		    "volume id, %d = the whole chip (read-only)\n", cmajor,
		    UBIVOL_CHIP);
}
