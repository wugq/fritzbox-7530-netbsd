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
 * Minimal driver for the Qualcomm IPQ40xx ESS Ethernet DMA engine (EDMA,
 * "qcom,ipq4019-ess-edma"), written for the FRITZ!Box 7530 NetBSD
 * experiment.
 *
 * Register usage follows OpenWrt's ipqess driver
 * (target/linux/ipq40xx/files/drivers/net/ethernet/qualcomm/ipqess).
 *
 *  - the switch driver (qcom_ess) has reset the ESS (switch + EDMA) and
 *    set up the switch before this driver attaches; this one only drives
 *    the DMA engine behind the switch's CPU port;
 *  - four RX queues spread by RSS, one interrupt and one CPU each; RX is
 *    processed in per-CPU threads; four TX queues, one per CPU
 *    (if_transmit), so forwarding stays on one CPU without a shared lock;
 *  - a slow callout reclaims TX descriptors and acts as a backstop;
 *  - RX: zero copy, the hardware writes into mbuf clusters;
 *    TX: zero copy for frames over 256 bytes, smaller ones are copied
 *    into fixed DMA buffers;
 *  - IPv4 TCP/UDP checksum offload in both directions; TSO is off.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/callout.h>
#include <sys/device.h>
#include <sys/evcnt.h>
#include <sys/interrupt.h>
#include <sys/intr.h>
#include <sys/kcpuset.h>
#include <sys/kernel.h>
#include <sys/kmem.h>
#include <sys/mbuf.h>
#include <sys/mutex.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <sys/workqueue.h>

#include <machine/cpu_counter.h>

#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_ether.h>
#include <net/if_media.h>
#include <net/bpf.h>

#include <dev/fdt/fdtvar.h>

/* Registers (subset of ipqess.h) */
#define REG_RX_ISR		0x200
#define REG_TX_ISR		0x208
#define REG_MISC_ISR		0x210
#define REG_MISC_IMR		0x214
#define  MISC_ISR_RX_URG_Q(x)	__BIT(x)
#define REG_WOL_ISR		0x218	/* also WOL_IMR in ipqess.h */
#define REG_RX_SW_CONS_IDX_Q(x)	(0x220 + ((x) << 2))
#define REG_TX_SW_CONS_IDX_Q(x)	(0x240 + ((x) << 2))
#define REG_IRQ_MODRT_TIMER_INIT 0x280
#define REG_INTR_CTRL		0x284
#define REG_RX_INT_MASK_Q(x)	(0x300 + ((x) << 2))
#define REG_TX_INT_MASK_Q(x)	(0x340 + ((x) << 2))
#define REG_TX_SRAM_PART	0x400
#define  TX_SRAM_LOAD_PTR	__BIT(16)
#define REG_TXQ_CTRL		0x404
#define  TXQ_CTRL_TXQ_EN	0x20
#define  TXQ_CTRL_TPD_BURST_EN	0x100
#define REG_TXF_WATER_MARK	0x408
#define REG_TPD_RING_SIZE	0x41c
#define REG_TPD_BASE_ADDR_Q(x)	(0x420 + ((x) << 2))
#define REG_TPD_IDX_Q(x)	(0x460 + ((x) << 2))
#define  TPD_PROD_IDX		__BITS(15, 0)
#define  TPD_CONS_IDX		__BITS(31, 16)
#define REG_VQ_CTRL0		0x4a0
#define REG_VQ_CTRL1		0x4a4
#define  VQ_REG_VALUE		0x240240
#define REG_VLAN_CFG		0x4ac
#define REG_TX_STAT_PKT_Q(x)	(0x700 + ((x) << 3))
#define REG_LB_RING		0x800
#define REG_RSS_IDT(x)		(0x840 + ((x) << 2))
#define REG_RSS_TYPE		0x894
#define  RSS_TYPE_NONE		0x01
#define  RSS_TYPE_ALL		0x7e	/* IPv4/6, TCP/UDP over IPv4/6 */
#define  RSS_TYPE_TCPUDP	0x1e	/* 4-tuple only: flows spread */
#define  RSS_IDT_VALUE		0x64206420	/* buckets -> queues 0,2,4,6 */
#define LB_REG_VALUE		0x6040200
#define REG_RFD_BASE_ADDR_Q(x)	(0x950 + ((x) << 2))
#define REG_RFD_IDX_Q(x)	(0x9b0 + ((x) << 2))
#define  RFD_PROD_IDX		__BITS(11, 0)
#define  RFD_CONS_IDX		__BITS(27, 16)
#define REG_RX_DESC0		0xa10
#define REG_RX_DESC1		0xa14
#define REG_RXQ_CTRL		0xa18
#define  RXQ_CTRL_RMV_VLAN	__BIT(1)	/* strip 802.1Q tags into the RRD */
#define  RXQ_CTRL_EN_MASK	__BITS(15, 8)
#define  RXQ_CTRL_EN(q)		__BIT(8 + (q))
#define REG_AXIW_CTRL_MAXWRSIZE	0xa1c
#define REG_RX_STAT_PKT_Q(x)	(0xa50 + ((x) << 2))
#define REG_WOL_CTRL		0xc08

/* TX descriptor (TPD) */
struct edma_tpd {
	uint16_t	len;
	uint16_t	svlan_tag;
	uint32_t	word1;
	uint32_t	addr;
	uint32_t	word3;
} __packed;
#define TPD_EOP			__BIT(31)	/* word1 */
#define TPD_CSUM_START_SHIFT	0		/* word1: csum start / 2 */
#define TPD_CUSTOM_CSUM_EN	0x00000c00	/* word1 */
#define TPD_CSUM_POS_SHIFT	18		/* word1: csum field / 2 */
#define TPD_PORT_BITMAP_SHIFT	18
#define TPD_LSO_EN		0x00001000	/* word1: TCP segmentation */
#define TPD_FROM_CPU		__BIT(25)	/* word3: use the port bitmap */
#define TPD_INS_CVLAN		__BIT(17)	/* word3: insert the 802.1Q tag in bits 15-0 */
#define TPD_IPV4_EN		0x00010000	/* word1: IPv4 LSO */
#define TPD_MSS_SHIFT		18		/* word1: LSO MSS */
#define TPD_MSS_MASK		0x1fff
#define TPD_HDR_SHIFT		0		/* word1: LSO L4 header offset */

/* RX return descriptor (RRD), written by HW at the start of each buffer */
struct edma_rrd {
	uint16_t	rrd[8];
} __packed;
#define RRD_NUM_RFD_MASK	0x000f	/* rrd[1] */
#define RRD_PKT_SIZE_MASK	0x3fff	/* rrd[6] */
#define RRD_CSUM_FAIL_MASK	0xc000	/* rrd[6] */
#define RRD_DESC_VALID		0x8000	/* rrd[7] */
#define RRD_CVLAN		0x0001	/* rrd[7]: tag removed, it is in rrd[4] */
#define RRD_SIZE		16

/*
 * Rings: one TPD ring per TX queue, one RFD ring per RX queue, all in one
 * coherent (uncached) DMA area, each ring starting on its own page.  The
 * engine takes these addresses only after the ESS reset (done by the switch
 * driver, qcom_ess, before this one attaches);
 * before that it keeps U-Boot's rings, which is why the first driver
 * reused them.
 */
#define EDMA_NTX		128
/*
 * RX ring entries per queue.  128 overflowed under bursts (MISC_ISR
 * RX_URG).  512 works (its early failure, queues stopping after a few
 * dozen frames, was the work item queued twice, see edma_rx_work()), but
 * forwarded one flow about 10% slower than 256 in an A/B test (613 vs 683
 * Mbit/s), probably because 4 x 512 buffers of 2 KB miss the 256 KB L2
 * cache more often (not measured).
 */
#define EDMA_NRX		256
#define EDMA_RING_STRIDE	PAGE_SIZE	/* each ring on its own page */
#define EDMA_RING_SIZE		((EDMA_NTXQ + EDMA_NRXQ) * EDMA_RING_STRIDE)
#define EDMA_TPD_OFF(k)		((k) * EDMA_RING_STRIDE)
#define EDMA_RFD_OFF(k)		((EDMA_NTXQ + (k)) * EDMA_RING_STRIDE)
#define EDMA_TXBUFSZ		2048
#define EDMA_TX_COPYMAX		256	/* smaller frames are copied, not mapped */
#define EDMA_TX_MAXSEGS		8
#define EDMA_RX_HEAD_BUFF_SIZE	1540	/* RX_DESC0 buffer size (as ipqess) */
/* TX queues 0, 4, 8, 12 (as ipqess), one per CPU */
#ifndef EDMA_NTXQ
#define EDMA_NTXQ		4
#endif
#define EDMA_TXQ_HW(k)		((k) * 4)

/*
 * RX queues: RSS spreads flows over hardware queues 0, 2, 4, 6 (as ipqess),
 * each with its own interrupt (DT: 16 TX interrupts, then 8 RX), bound to
 * its own CPU.  EDMA_NRXQ=1 gives the single-queue driver.
 */
#ifndef EDMA_NRXQ
#define EDMA_NRXQ		4
#endif
#define EDMA_RXQ_HW(k)		((k) * 2)
#define EDMA_RXQ_INTR(k)	(16 + EDMA_RXQ_HW(k))

#define RX_ALIGN		2	/* RRD (16) + 2: IP header 4-byte aligned */

/* the DMA area holds the TX buffers */
#define OFF_TXBUF		0
#define DMA_SIZE		(EDMA_NTXQ * EDMA_NTX * EDMA_TXBUFSZ)

/* each ring must fit its page in the ring area */
CTASSERT(EDMA_NTX * 16 <= EDMA_RING_STRIDE);	/* sizeof(struct edma_tpd) */
CTASSERT(EDMA_NRX * 4 <= EDMA_RING_STRIDE);

struct edma_softc;

struct edma_rxq {
	struct edma_softc	*rq_sc;
	u_int			rq_k;		/* 0..EDMA_NRXQ-1 */
	u_int			rq_hw;		/* hardware queue */
	kmutex_t		rq_lock;
	volatile uint32_t	*rq_rfd;	/* RFD ring (in sc_ring) */
	bus_addr_t		rq_rfd_pa;
	u_int			rq_tail;	/* next RFD to look at */
	struct mbuf		*rq_m[EDMA_NRX];
	bus_dmamap_t		rq_map[EDMA_NRX];
	void			*rq_ih;
	char			rq_intrid[INTRIDBUF];
	struct work		rq_wk;
	bool			rq_queued;
	struct cpu_info		*rq_ci;		/* CPU of the last interrupt */
	u_int			rq_stall;	/* runs without progress */
	char			rq_evgroup[24];	/* "qcomedma0 rxq0" */
	struct evcnt		rq_ev_frames;	/* frames handed to the stack */
	struct evcnt		rq_ev_intr;	/* interrupts */
	struct evcnt		rq_ev_noisr;	/* ... with our RX_ISR bit clear */
	struct evcnt		rq_ev_runs;	/* RX thread runs */
	struct evcnt		rq_ev_idle;	/* ... that found nothing */
	struct evcnt		rq_ev_novalid;	/* RFDs without a valid RRD */
	struct evcnt		rq_ev_bad;	/* bad length / multi-RFD frames */
};

struct edma_txq {
	struct edma_softc	*tq_sc;
	u_int			tq_k;		/* 0..EDMA_NTXQ-1 */
	u_int			tq_hw;		/* hardware queue */
	kmutex_t		tq_lock;
	volatile uint32_t	*tq_tpd;	/* TPD ring (in sc_ring) */
	bus_addr_t		tq_tpd_pa;
	bus_size_t		tq_bufoff;	/* our TX buffers in sc_va */
	u_int			tq_head;	/* next TPD to fill */
	u_int			tq_tail;	/* oldest TPD not reclaimed */
	struct ifqueue		tq_ifq;		/* frames waiting for room */
	/*
	 * Zero-copy TX: a frame is mapped with the DMA map of its first
	 * TPD; the mbuf and that first index are kept at its last TPD.
	 */
	bus_dmamap_t		tq_map[EDMA_NTX];
	struct mbuf		*tq_m[EDMA_NTX];
	uint16_t		tq_first[EDMA_NTX];
	char			tq_evgroup[24];	/* "qcomedma0 txq0" */
	struct evcnt		tq_ev_tso;	/* TSO frames */
};

struct edma_softc {
	device_t		sc_dev;
	int			sc_phandle;
	bus_space_tag_t		sc_bst;
	bus_space_handle_t	sc_bsh;
	bus_dma_tag_t		sc_dmat;

	bus_dmamap_t		sc_map;		/* TX copy buffers (cached) */
	bus_dma_segment_t	sc_seg;
	uint8_t			*sc_va;
	bus_addr_t		sc_pa;

	bus_dmamap_t		sc_ringmap;	/* descriptor rings (coherent) */
	bus_dma_segment_t	sc_ringseg;
	uint8_t			*sc_ring;

	struct ethercom		sc_ec;
	struct ifmedia		sc_media;
	kmutex_t		sc_lock;	/* sc_running, init/stop */
	callout_t		sc_tick;
	struct workqueue	*sc_rxwq;	/* per CPU, see edma_intr */
	bool			sc_running;
	bool			sc_irqs_bound;

	u_int			sc_ntx, sc_nrx;

	/* tuning and debugging, under hw.qcomedmaN (edma_sysctl_attach) */
	struct sysctllog	*sc_sysctllog;
	int			sc_rss_type;	/* RSS_TYPE register */
	int			sc_tso_fromcpu;	/* TSO TPDs: set FROM_CPU */
	int			sc_watch;	/* print queue state every N s */
	u_int			sc_watch_ticks;

	struct edma_txq		sc_txq[EDMA_NTXQ];

	struct edma_rxq		sc_rxq[EDMA_NRXQ];
};

#define RD4(sc, r)	bus_space_read_4((sc)->sc_bst, (sc)->sc_bsh, (r))
#define WR4(sc, r, v)	bus_space_write_4((sc)->sc_bst, (sc)->sc_bsh, (r), (v))
#define M4(sc, r, clr, set) WR4((sc), (r), (RD4((sc), (r)) & ~(clr)) | (set))

/*
 * Descriptor writes go straight to the coherent ring memory.  Before the
 * engine is told about them (a producer index register write), call
 * edma_ring_sync(): on ARM a store to uncached normal memory and a later
 * store to a device register are not ordered by themselves.
 */
#define TPD_WR(sc, tq, i, w, v) \
	((tq)->tq_tpd[(i) * (sizeof(struct edma_tpd) / 4) + (w)] = htole32(v))
#define RFD_WR(sc, rq, i, v)	((rq)->rq_rfd[(i)] = htole32(v))

static const struct device_compatible_entry compat_data[] = {
	{ .compat = "qcom,ipq4019-ess-edma" },
	DEVICE_COMPAT_EOL
};

static int	edma_match(device_t, cfdata_t, void *);
static void	edma_attach(device_t, device_t, void *);

CFATTACH_DECL_NEW(ipq4019_edma, sizeof(struct edma_softc),
	edma_match, edma_attach, NULL, NULL);

/*
 * Cycle accounting per CPU (hw.qcomedma0.prof=1 prints and clears it):
 * RX descriptor work, TX work under sc_lock, and waiting for sc_lock.
 */
struct edma_prof {
	uint64_t	rx_cyc, rx_pkts, rx_calls;
	uint64_t	tx_cyc, tx_pkts;
	uint64_t	txlock_cyc, txlock_calls;
} __aligned(64);
static struct edma_prof edma_prof[MAXCPUS];
#define EDMA_PROF()	(&edma_prof[cpu_index(curcpu())])

/*
 * Make descriptor writes in [off, off + len) of the ring area visible to
 * the engine.  Sync only the ring just written: on this platform the sync
 * turned out to clean the data cache over the whole range given, and doing
 * all 32 KB for every transmitted frame cost ~4000 cycles a packet.
 */
static void
edma_ring_sync(struct edma_softc *sc, bus_size_t off, bus_size_t len)
{
	bus_dmamap_sync(sc->sc_dmat, sc->sc_ringmap, off, len,
	    BUS_DMASYNC_PREWRITE);
}

static void
edma_sync(struct edma_softc *sc, bus_size_t off, bus_size_t len, int ops)
{
	bus_dmamap_sync(sc->sc_dmat, sc->sc_map, off, len, ops);
}

static u_int
edma_rx_cons(struct edma_softc *sc, struct edma_rxq *rq)
{
	return __SHIFTOUT(RD4(sc, REG_RFD_IDX_Q(rq->rq_hw)), RFD_CONS_IDX) %
	    sc->sc_nrx;
}

static void edma_tx_purge(struct edma_softc *, struct edma_txq *);

static void
edma_hw_stop(struct edma_softc *sc)
{
	for (u_int i = 0; i < 8; i++)
		WR4(sc, REG_RX_INT_MASK_Q(i), 0);
	for (u_int i = 0; i < 16; i++)
		WR4(sc, REG_TX_INT_MASK_Q(i), 0);
	WR4(sc, REG_MISC_IMR, 0);
	WR4(sc, REG_RX_ISR, 0xff);
	WR4(sc, REG_TX_ISR, 0xffff);
	WR4(sc, REG_MISC_ISR, 0x1fff);
	WR4(sc, REG_WOL_ISR, 0x1);
	WR4(sc, REG_WOL_CTRL, 0);
	M4(sc, REG_RXQ_CTRL, RXQ_CTRL_EN_MASK, 0);
	M4(sc, REG_TXQ_CTRL, TXQ_CTRL_TXQ_EN, 0);
	delay(1000);
}

static int
edma_sysctl_prof(SYSCTLFN_ARGS)
{
	struct sysctlnode node = *rnode;
	int val = 0, error;

	node.sysctl_data = &val;
	error = sysctl_lookup(SYSCTLFN_CALL(&node));
	if (error || newp == NULL || val == 0)
		return error;
	for (u_int c = 0; c < ncpu; c++) {
		struct edma_prof * const pr = &edma_prof[c];
		printf("edma prof cpu%u: rx %llu pkts in %llu calls, %llu cyc/pkt;"
		    " tx %llu pkts, %llu cyc/pkt; txlock %llu calls, "
		    "%llu cyc/call\n", c,
		    (unsigned long long)pr->rx_pkts,
		    (unsigned long long)pr->rx_calls,
		    (unsigned long long)(pr->rx_pkts ?
			pr->rx_cyc / pr->rx_pkts : 0),
		    (unsigned long long)pr->tx_pkts,
		    (unsigned long long)(pr->tx_pkts ?
			pr->tx_cyc / pr->tx_pkts : 0),
		    (unsigned long long)pr->txlock_calls,
		    (unsigned long long)(pr->txlock_calls ?
			pr->txlock_cyc / pr->txlock_calls : 0));
		memset(pr, 0, sizeof(*pr));
	}
	return 0;
}

static int
edma_sysctl_rss_type(SYSCTLFN_ARGS)
{
	struct sysctlnode node = *rnode;
	struct edma_softc * const sc = rnode->sysctl_data;
	int val = sc->sc_rss_type, error;

	node.sysctl_data = &val;
	error = sysctl_lookup(SYSCTLFN_CALL(&node));
	if (error || newp == NULL)
		return error;
	sc->sc_rss_type = val & 0x7f;
	if (EDMA_NRXQ > 1)
		WR4(sc, REG_RSS_TYPE, sc->sc_rss_type);
	return 0;
}

static void
edma_hw_init(struct edma_softc *sc)
{
	uint32_t rxen = 0;
	u_int i, idx;

	edma_hw_stop(sc);
	delay(10000);	/* let in-flight DMA finish */

	/* SW index write type 0, as ipqess */
	M4(sc, REG_INTR_CTRL, __BIT(1), 0);
	WR4(sc, REG_IRQ_MODRT_TIMER_INIT, (0x50 << 16) | 0x20);
	WR4(sc, REG_VLAN_CFG, (ETHERTYPE_VLAN << 16) | ETHERTYPE_QINQ);
	WR4(sc, REG_TXQ_CTRL, 5 | (0x100 << 16) | TXQ_CTRL_TPD_BURST_EN);
	if (EDMA_NRXQ > 1) {
		/* hash buckets -> queues 0,2,4,6; load balance likewise */
		WR4(sc, REG_RSS_TYPE, sc->sc_rss_type);
		for (i = 0; i < 16; i++)
			WR4(sc, REG_RSS_IDT(i), RSS_IDT_VALUE);
		WR4(sc, REG_LB_RING, LB_REG_VALUE);
	} else {
		WR4(sc, REG_RSS_TYPE, RSS_TYPE_NONE);
		for (i = 0; i < 16; i++)
			WR4(sc, REG_RSS_IDT(i), 0);
		WR4(sc, REG_LB_RING, 0);
	}
	WR4(sc, REG_RX_DESC1, 8 | (16 << 8) | (0 << 16));
	/* 128 byte FIFO threshold; strip VLAN tags if vlanhwtag is on */
	WR4(sc, REG_RXQ_CTRL,
	    (sc->sc_ec.ec_capenable & ETHERCAP_VLAN_HWTAGGING) ?
	    RXQ_CTRL_RMV_VLAN : 0);

	/* RX rings: every RFD points at an mbuf cluster */
	for (u_int k = 0; k < EDMA_NRXQ; k++) {
		struct edma_rxq * const rq = &sc->sc_rxq[k];

		for (i = 0; i < sc->sc_nrx; i++)
			RFD_WR(sc, rq, i, rq->rq_map[i]->dm_segs[0].ds_addr);
		WR4(sc, REG_RFD_BASE_ADDR_Q(rq->rq_hw), rq->rq_rfd_pa);
		rxen |= RXQ_CTRL_EN(rq->rq_hw);
	}
	WR4(sc, REG_RX_DESC0, (EDMA_RX_HEAD_BUFF_SIZE << 16) | sc->sc_nrx);
	/* TX rings */
	for (u_int k = 0; k < EDMA_NTXQ; k++) {
		struct edma_txq * const tq = &sc->sc_txq[k];

		mutex_enter(&tq->tq_lock);
		edma_tx_purge(sc, tq);
		for (i = 0; i < sc->sc_ntx; i++)
			for (u_int w = 0; w < 4; w++)
				TPD_WR(sc, tq, i, w, 0);
		WR4(sc, REG_TPD_BASE_ADDR_Q(tq->tq_hw), tq->tq_tpd_pa);
		mutex_exit(&tq->tq_lock);
	}
	WR4(sc, REG_TPD_RING_SIZE, sc->sc_ntx);
	edma_ring_sync(sc, 0, EDMA_RING_SIZE);
	M4(sc, REG_TX_SRAM_PART, 0, TX_SRAM_LOAD_PTR);

	/* start from wherever the hardware consumer indexes are */
	for (u_int k = 0; k < EDMA_NTXQ; k++) {
		struct edma_txq * const tq = &sc->sc_txq[k];

		idx = __SHIFTOUT(RD4(sc, REG_TPD_IDX_Q(tq->tq_hw)),
		    TPD_CONS_IDX) % sc->sc_ntx;
		tq->tq_head = tq->tq_tail = idx;
		WR4(sc, REG_TPD_IDX_Q(tq->tq_hw), idx);
		WR4(sc, REG_TX_SW_CONS_IDX_Q(tq->tq_hw), idx);
	}

	for (u_int k = 0; k < EDMA_NRXQ; k++) {
		struct edma_rxq * const rq = &sc->sc_rxq[k];

		idx = edma_rx_cons(sc, rq);
		rq->rq_tail = idx;
		WR4(sc, REG_RX_SW_CONS_IDX_Q(rq->rq_hw), idx);
		/* all buffers are available: producer = one before consumer */
		M4(sc, REG_RFD_IDX_Q(rq->rq_hw), RFD_PROD_IDX,
		    (idx + sc->sc_nrx - 1) % sc->sc_nrx);
	}

	WR4(sc, REG_TXF_WATER_MARK, 0);
	WR4(sc, REG_VQ_CTRL0, VQ_REG_VALUE);
	WR4(sc, REG_VQ_CTRL1, VQ_REG_VALUE);
	WR4(sc, REG_AXIW_CTRL_MAXWRSIZE, 0);

	M4(sc, REG_TXQ_CTRL, 0, TXQ_CTRL_TXQ_EN);
	M4(sc, REG_RXQ_CTRL, RXQ_CTRL_EN_MASK, rxen);

	/* RX interrupts (cleared by writing 1 to the queue's RX_ISR bit) */
	WR4(sc, REG_RX_ISR, 0xff);
	for (u_int k = 0; k < EDMA_NRXQ; k++)
		if (sc->sc_rxq[k].rq_ih != NULL)
			WR4(sc, REG_RX_INT_MASK_Q(sc->sc_rxq[k].rq_hw), 1);

}

static void
edma_dump(struct edma_softc *sc)
{
	printf("edma: RX_ISR %#x MISC_ISR %#x RXQ_CTRL %#x RSS_TYPE %#x\n",
	    RD4(sc, REG_RX_ISR), RD4(sc, REG_MISC_ISR), RD4(sc, REG_RXQ_CTRL),
	    RD4(sc, REG_RSS_TYPE));
	for (u_int k = 0; k < EDMA_NTXQ; k++) {
		struct edma_txq * const tq = &sc->sc_txq[k];

		printf("edma: txq%u (hw %u): TPD_IDX %#x TX_SW_CONS %#x "
		    "head %u tail %u queued %d tso %u\n", k, tq->tq_hw,
		    RD4(sc, REG_TPD_IDX_Q(tq->tq_hw)),
		    RD4(sc, REG_TX_SW_CONS_IDX_Q(tq->tq_hw)),
		    tq->tq_head, tq->tq_tail, tq->tq_ifq.ifq_len,
		    (u_int)tq->tq_ev_tso.ev_count);
	}
	for (u_int k = 0; k < EDMA_NRXQ; k++) {
		struct edma_rxq * const rq = &sc->sc_rxq[k];

		printf("edma: rxq%u (hw %u, %s): RFD_IDX %#x tail %u, "
		    "queued %d, masked %d\n", k, rq->rq_hw,
		    rq->rq_intrid, RD4(sc, REG_RFD_IDX_Q(rq->rq_hw)),
		    rq->rq_tail, rq->rq_queued,
		    RD4(sc, REG_RX_INT_MASK_Q(rq->rq_hw)) == 0);
	}
}

static int
edma_sysctl_dump(SYSCTLFN_ARGS)
{
	struct sysctlnode node = *rnode;
	struct edma_softc * const sc = rnode->sysctl_data;
	int val = 0, error;

	node.sysctl_data = &val;
	error = sysctl_lookup(SYSCTLFN_CALL(&node));
	if (error || newp == NULL)
		return error;
	if (val != 0)
		edma_dump(sc);	/* racy reads, debug only */
	return 0;
}

/* --- TX ------------------------------------------------------------- */

static u_int
edma_tx_avail(struct edma_softc *sc, struct edma_txq *tq)
{
	return (tq->tq_tail + sc->sc_ntx - tq->tq_head - 1) % sc->sc_ntx;
}

static void
edma_tx_reclaim(struct edma_softc *sc, struct edma_txq *tq)
{
	u_int cons;

	KASSERT(mutex_owned(&tq->tq_lock));
	cons = __SHIFTOUT(RD4(sc, REG_TPD_IDX_Q(tq->tq_hw)), TPD_CONS_IDX) %
	    sc->sc_ntx;
	if (cons == tq->tq_tail)
		return;
	while (tq->tq_tail != cons) {
		const u_int i = tq->tq_tail;
		struct mbuf * const m = tq->tq_m[i];

		if (m != NULL) {
			bus_dmamap_t const map = tq->tq_map[tq->tq_first[i]];

			bus_dmamap_sync(sc->sc_dmat, map, 0, map->dm_mapsize,
			    BUS_DMASYNC_POSTWRITE);
			bus_dmamap_unload(sc->sc_dmat, map);
			m_freem(m);
			tq->tq_m[i] = NULL;
		}
		tq->tq_tail = (i + 1) % sc->sc_ntx;
	}
	WR4(sc, REG_TX_SW_CONS_IDX_Q(tq->tq_hw), tq->tq_tail);
}

/* free frames still on the ring or queued (the engine is stopped) */
static void
edma_tx_purge(struct edma_softc *sc, struct edma_txq *tq)
{
	for (u_int i = 0; i < sc->sc_ntx; i++) {
		struct mbuf * const m = tq->tq_m[i];

		if (m == NULL)
			continue;
		bus_dmamap_unload(sc->sc_dmat, tq->tq_map[tq->tq_first[i]]);
		m_freem(m);
		tq->tq_m[i] = NULL;
	}
	IF_PURGE(&tq->tq_ifq);
}

static void
edma_tpd(struct edma_softc *sc, struct edma_txq *tq, u_int i, u_int len,
    uint32_t word1, bus_addr_t pa, uint32_t word3)
{
	/* len | svlan_tag, word1, addr, word3 */
	TPD_WR(sc, tq, i, 0, len);
	TPD_WR(sc, tq, i, 1, word1);
	TPD_WR(sc, tq, i, 2, pa);
	/* no DSA tag: let the switch send it to ports 1-5 (as ipqess) */
	TPD_WR(sc, tq, i, 3, word3 | (0x3e << TPD_PORT_BITMAP_SHIFT) |
	    ((word1 & TPD_LSO_EN) && sc->sc_tso_fromcpu ? TPD_FROM_CPU : 0));
}

static bus_size_t
edma_tx_bufoff(struct edma_txq *tq, u_int i)
{
	return tq->tq_bufoff + i * EDMA_TXBUFSZ;
}

/*
 * TSO (as Linux ipqess): the stack left ip_sum 0 and the pseudo-header sum
 * without length in th_sum, which is what the hardware wants.  The headers
 * get a TPD of their own, then the payload goes out in EDMA_TXBUFSZ pieces,
 * one per TPD, each copied into that TPD's own buffer.  Off by default:
 * the engine consumes these TPDs but sends nothing (see edma_attach).
 */
/* vlan(4) with ETHERCAP_VLAN_HWTAGGING: the tag is in the mbuf header */
static inline uint32_t
edma_tx_vtag(struct mbuf *m)
{
	return vlan_has_tag(m) ? TPD_INS_CVLAN | vlan_get_tag(m) : 0;
}

static void
edma_tx_tso(struct edma_softc *sc, struct edma_txq *tq, struct mbuf *m)
{
	const u_int len = m->m_pkthdr.len;
	const u_int thoff = ETHER_HDR_LEN +
	    M_CSUM_DATA_IPv4_IPHL(m->m_pkthdr.csum_data);
	uint8_t off_x2;
	u_int i = tq->tq_head;

	m_copydata(m, thoff + 12, 1, &off_x2);
	const u_int hlen = thoff + ((off_x2 >> 4) << 2);
	const uint32_t word1 = TPD_LSO_EN | TPD_IPV4_EN |
	    ((m->m_pkthdr.segsz & TPD_MSS_MASK) << TPD_MSS_SHIFT) |
	    (thoff << TPD_HDR_SHIFT);

	for (u_int off = 0; off < len; ) {
		const u_int n = off == 0 ? hlen : MIN(EDMA_TXBUFSZ, len - off);
		const bus_size_t boff = edma_tx_bufoff(tq, i);

		m_copydata(m, off, n, sc->sc_va + boff);
		edma_sync(sc, boff, n, BUS_DMASYNC_PREWRITE);
		off += n;
		edma_tpd(sc, tq, i, n, word1 | (off == len ? TPD_EOP : 0),
		    sc->sc_pa + boff, edma_tx_vtag(m));
		i = (i + 1) % sc->sc_ntx;
	}
	tq->tq_head = i;
	tq->tq_ev_tso.ev_count++;
}

/* TPDs frame m may need */
static u_int
edma_tx_need(struct mbuf *m)
{
	if (m->m_pkthdr.csum_flags & M_CSUM_TSOv4)
		return 1 + howmany(m->m_pkthdr.len, EDMA_TXBUFSZ);
	return m->m_pkthdr.len > EDMA_TX_COPYMAX ? EDMA_TX_MAXSEGS : 1;
}

/* is there room for m?  Reading the consumer index is slow: only if short */
static bool
edma_tx_room(struct edma_softc *sc, struct edma_txq *tq, struct mbuf *m)
{
	const u_int need = edma_tx_need(m);

	if (edma_tx_avail(sc, tq) >= need)
		return true;
	edma_tx_reclaim(sc, tq);
	return edma_tx_avail(sc, tq) >= need;
}

/* put frame m on the ring (caller checked edma_tx_room); consumes m */
static void
edma_tx_put(struct edma_softc *sc, struct edma_txq *tq, struct mbuf *m)
{
	struct ifnet * const ifp = &sc->sc_ec.ec_if;
	const u_int i = tq->tq_head;
	u_int len = m->m_pkthdr.len;

	if (m->m_pkthdr.csum_flags & M_CSUM_TSOv4) {
		edma_tx_tso(sc, tq, m);
		bpf_mtap(ifp, m, BPF_D_OUT);
		m_freem(m);
		if_statinc(ifp, if_opackets);
		return;
	}

	uint32_t word1 = TPD_EOP;
	if (m->m_pkthdr.csum_flags & (M_CSUM_TCPv4 | M_CSUM_UDPv4)) {
		/* stack left the pseudo-header sum in the L4 field */
		const u_int start = ETHER_HDR_LEN +
		    M_CSUM_DATA_IPv4_IPHL(m->m_pkthdr.csum_data);
		const u_int pos = start +
		    M_CSUM_DATA_IPv4_OFFSET(m->m_pkthdr.csum_data);
		word1 |= TPD_CUSTOM_CSUM_EN |
		    ((start >> 1) << TPD_CSUM_START_SHIFT) |
		    ((pos >> 1) << TPD_CSUM_POS_SHIFT);
	}

	if (len > EDMA_TX_COPYMAX) {
		/* zero copy: the TPDs point into the mbuf chain */
		bus_dmamap_t const map = tq->tq_map[i];
		int error = bus_dmamap_load_mbuf(sc->sc_dmat, map, m,
		    BUS_DMA_WRITE | BUS_DMA_NOWAIT);
		if (error == EFBIG) {
			struct mbuf * const n = m_defrag(m, M_DONTWAIT);
			if (n != NULL) {
				m = n;
				error = bus_dmamap_load_mbuf(sc->sc_dmat, map,
				    m, BUS_DMA_WRITE | BUS_DMA_NOWAIT);
			}
		}
		if (error != 0) {
			if_statinc(ifp, if_oerrors);
			m_freem(m);
			return;
		}
		bus_dmamap_sync(sc->sc_dmat, map, 0, map->dm_mapsize,
		    BUS_DMASYNC_PREWRITE);
		bpf_mtap(ifp, m, BPF_D_OUT);
		u_int j = i;
		for (int k = 0; k < map->dm_nsegs; k++) {
			j = (i + k) % sc->sc_ntx;
			edma_tpd(sc, tq, j, map->dm_segs[k].ds_len,
			    (word1 & ~TPD_EOP) |
			    (k == map->dm_nsegs - 1 ? TPD_EOP : 0),
			    map->dm_segs[k].ds_addr, edma_tx_vtag(m));
		}
		tq->tq_m[j] = m;
		tq->tq_first[j] = i;
		tq->tq_head = (j + 1) % sc->sc_ntx;
		if_statinc(ifp, if_opackets);
		return;
	}

	/* small frame: copy into this TPD's buffer */
	const bus_size_t boff = edma_tx_bufoff(tq, i);
	uint8_t * const buf = sc->sc_va + boff;
	m_copydata(m, 0, len, buf);
	const uint32_t word3 = edma_tx_vtag(m);
	if (len < ETHER_MIN_LEN - ETHER_CRC_LEN) {
		memset(buf + len, 0, ETHER_MIN_LEN - ETHER_CRC_LEN - len);
		len = ETHER_MIN_LEN - ETHER_CRC_LEN;
	}
	bpf_mtap(ifp, m, BPF_D_OUT);
	m_freem(m);
	edma_sync(sc, boff, len, BUS_DMASYNC_PREWRITE);
	edma_tpd(sc, tq, i, len, word1, sc->sc_pa + boff, word3);
	tq->tq_head = (i + 1) % sc->sc_ntx;
	if_statinc(ifp, if_opackets);
}

/* move queued frames onto the ring while there is room; kick the engine */
static void
edma_tx_drain(struct edma_softc *sc, struct edma_txq *tq)
{
	struct mbuf *m;
	const u_int head0 = tq->tq_head;
	u_int npkts = 0;

	KASSERT(mutex_owned(&tq->tq_lock));
	if (!sc->sc_running)
		return;
	const uint32_t t0 = cpu_counter32();
	for (;;) {
		IF_POLL(&tq->tq_ifq, m);
		if (m == NULL || !edma_tx_room(sc, tq, m))
			break;
		IF_DEQUEUE(&tq->tq_ifq, m);
		edma_tx_put(sc, tq, m);
		npkts++;
	}
	/* the consumer half of the register is read-only: plain write (ipqess) */
	if (tq->tq_head != head0) {
		/* sync just the TPDs written: [head0, head), maybe wrapping */
		const bus_size_t base = EDMA_TPD_OFF(tq->tq_k);
		const bus_size_t tsz = sizeof(struct edma_tpd);
		if (tq->tq_head > head0)
			edma_ring_sync(sc, base + head0 * tsz,
			    (tq->tq_head - head0) * tsz);
		else {
			edma_ring_sync(sc, base + head0 * tsz,
			    (sc->sc_ntx - head0) * tsz);
			if (tq->tq_head > 0)
				edma_ring_sync(sc, base, tq->tq_head * tsz);
		}
		WR4(sc, REG_TPD_IDX_Q(tq->tq_hw), tq->tq_head);
	}
	if (npkts > 0) {
		struct edma_prof * const pr = EDMA_PROF();
		pr->tx_cyc += cpu_counter32() - t0;
		pr->tx_pkts += npkts;
	}
}

/*
 * if_transmit: each CPU sends on its own TX queue, so forwarding (done on
 * the CPU of the RX queue) rarely contends for a TX lock.
 */
static int
edma_transmit(struct ifnet *ifp, struct mbuf *m)
{
	struct edma_softc * const sc = ifp->if_softc;
	struct edma_txq * const tq =
	    &sc->sc_txq[cpu_index(curcpu()) % EDMA_NTXQ];
	const uint32_t t0 = cpu_counter32();

	mutex_enter(&tq->tq_lock);
	struct edma_prof * const pr = EDMA_PROF();
	pr->txlock_cyc += cpu_counter32() - t0;
	pr->txlock_calls++;
	if (IF_QFULL(&tq->tq_ifq)) {
		mutex_exit(&tq->tq_lock);
		if_statinc(ifp, if_oerrors);
		m_freem(m);
		return ENOBUFS;
	}
	IF_ENQUEUE(&tq->tq_ifq, m);
	edma_tx_drain(sc, tq);
	mutex_exit(&tq->tq_lock);
	return 0;
}

/* if_start: whatever still comes through if_snd goes out on queue 0 */
static void
edma_start(struct ifnet *ifp)
{
	struct edma_softc * const sc = ifp->if_softc;
	struct edma_txq * const tq = &sc->sc_txq[0];
	struct mbuf *m;

	mutex_enter(&tq->tq_lock);
	for (;;) {
		IFQ_DEQUEUE(&ifp->if_snd, m);
		if (m == NULL)
			break;
		if (IF_QFULL(&tq->tq_ifq)) {
			if_statinc(ifp, if_oerrors);
			m_freem(m);
			continue;
		}
		IF_ENQUEUE(&tq->tq_ifq, m);
	}
	edma_tx_drain(sc, tq);
	mutex_exit(&tq->tq_lock);
}

/* reclaim completed TPDs and send what waited for room */
static void
edma_tx_kick(struct edma_softc *sc, struct edma_txq *tq)
{
	KASSERT(mutex_owned(&tq->tq_lock));
	if (!sc->sc_running)
		return;
	edma_tx_reclaim(sc, tq);
	edma_tx_drain(sc, tq);
}

/* --- RX ------------------------------------------------------------- */

/* put a fresh mbuf cluster into RX slot i */
static int
edma_rx_newbuf(struct edma_softc *sc, struct edma_rxq *rq, u_int i)
{
	struct mbuf *m;
	int error;

	MGETHDR(m, M_DONTWAIT, MT_DATA);
	if (m == NULL)
		return ENOBUFS;
	MCLGET(m, M_DONTWAIT);
	if ((m->m_flags & M_EXT) == 0) {
		m_freem(m);
		return ENOBUFS;
	}
	m->m_len = m->m_pkthdr.len = MCLBYTES;
	m_adj(m, RX_ALIGN);
	/* recycled clusters may hold an old frame with a "valid" RRD */
	memset(mtod(m, void *), 0, RRD_SIZE);
	error = bus_dmamap_load_mbuf(sc->sc_dmat, rq->rq_map[i], m,
	    BUS_DMA_READ | BUS_DMA_NOWAIT);
	if (error != 0) {
		m_freem(m);
		return error;
	}
	bus_dmamap_sync(sc->sc_dmat, rq->rq_map[i], 0,
	    EDMA_RX_HEAD_BUFF_SIZE, BUS_DMASYNC_PREREAD | BUS_DMASYNC_PREWRITE);
	rq->rq_m[i] = m;
	return 0;
}

/*
 * Process RX queue rq up to the hardware consumer index.  Called with
 * rq_lock held; drops it to hand the frames to the stack.
 */
static void
edma_rx(struct edma_softc *sc, struct edma_rxq *rq)
{
	struct ifnet * const ifp = &sc->sc_ec.ec_if;
	struct mbuf *head = NULL, **tailp = &head;
	u_int cons, n;

	KASSERT(mutex_owned(&rq->rq_lock));
	const uint32_t t0 = cpu_counter32();
	cons = edma_rx_cons(sc, rq);

	for (n = 0; rq->rq_tail != cons && n < sc->sc_nrx; n++) {
		const u_int i = rq->rq_tail;
		bus_dmamap_t const map = rq->rq_map[i];
		struct mbuf * const m = rq->rq_m[i];
		const struct edma_rrd * const rrd = mtod(m, struct edma_rrd *);
		u_int nrfd = 1, len = 0;
		bool ok = false;

		bus_dmamap_sync(sc->sc_dmat, map, 0, EDMA_RX_HEAD_BUFF_SIZE,
		    BUS_DMASYNC_POSTREAD);

		if ((le16toh(rrd->rrd[7]) & RRD_DESC_VALID) == 0) {
			rq->rq_ev_novalid.ev_count++;
		} else {
			len = le16toh(rrd->rrd[6]) & RRD_PKT_SIZE_MASK;
			nrfd = le16toh(rrd->rrd[1]) & RRD_NUM_RFD_MASK;
			if (nrfd != 1 || len < ETHER_HDR_LEN ||
			    len > EDMA_RX_HEAD_BUFF_SIZE - RRD_SIZE) {
				/* multi-buffer (jumbo) frames: not supported */
				rq->rq_ev_bad.ev_count++;
				if (nrfd == 0)
					nrfd = 1;
			} else
				ok = true;
		}

		if (ok) {
			const uint16_t rrd6 = le16toh(rrd->rrd[6]);
			const uint16_t rrd7 = le16toh(rrd->rrd[7]);
			const uint16_t vtag = le16toh(rrd->rrd[4]);
			bus_dmamap_unload(sc->sc_dmat, map);
			if (edma_rx_newbuf(sc, rq, i) != 0) {
				/* no new buffer: drop, keep the old one */
				rq->rq_m[i] = m;
				(void)bus_dmamap_load_mbuf(sc->sc_dmat, map, m,
				    BUS_DMA_READ | BUS_DMA_NOWAIT);
				bus_dmamap_sync(sc->sc_dmat, map, 0,
				    EDMA_RX_HEAD_BUFF_SIZE, BUS_DMASYNC_PREREAD);
				if_statinc(ifp, if_iqdrops);
			} else {
				RFD_WR(sc, rq, i,
				    rq->rq_map[i]->dm_segs[0].ds_addr);
				m_adj(m, RRD_SIZE);
				m->m_len = m->m_pkthdr.len = len;
				m_set_rcvif(m, ifp);
				if ((rrd6 & RRD_CSUM_FAIL_MASK) == 0)
					m->m_pkthdr.csum_flags =
					    ifp->if_csum_flags_rx &
					    (M_CSUM_IPv4 | M_CSUM_TCPv4 |
					     M_CSUM_UDPv4);
				if (rrd7 & RRD_CVLAN)
					vlan_set_tag(m, vtag);
				rq->rq_ev_frames.ev_count++;
				/* hand over after the loop, without the lock */
				*tailp = m;
				tailp = &m->m_nextpkt;
			}
		} else {
			if_statinc(ifp, if_ierrors);
			/* reuse the buffer(s) */
			for (u_int k = 0; k < nrfd; k++) {
				const u_int j = (i + k) % sc->sc_nrx;
				bus_dmamap_sync(sc->sc_dmat, rq->rq_map[j],
				    0, EDMA_RX_HEAD_BUFF_SIZE,
				    BUS_DMASYNC_PREREAD);
			}
		}
		rq->rq_tail = (i + nrfd) % sc->sc_nrx;
	}

	if (n > 0) {
		edma_ring_sync(sc, EDMA_RFD_OFF(rq->rq_k), sc->sc_nrx * 4);
		WR4(sc, REG_RX_SW_CONS_IDX_Q(rq->rq_hw), rq->rq_tail);
		M4(sc, REG_RFD_IDX_Q(rq->rq_hw), RFD_PROD_IDX,
		    (rq->rq_tail + sc->sc_nrx - 1) % sc->sc_nrx);
		struct edma_prof * const pr = EDMA_PROF();
		pr->rx_cyc += cpu_counter32() - t0;
		pr->rx_pkts += n;
		pr->rx_calls++;
	}

	if (head != NULL) {
		mutex_exit(&rq->rq_lock);
		while (head != NULL) {
			struct mbuf * const m = head;
			head = m->m_nextpkt;
			m->m_nextpkt = NULL;
			if_percpuq_enqueue(ifp->if_percpuq, m);
		}
		mutex_enter(&rq->rq_lock);
	}
}

/*
 * The interrupt only masks itself and wakes the RX thread of its CPU
 * (NAPI style).  Processing RX in the hard interrupt livelocked the box
 * under load: ipintr() loops until ip_pktq is empty, the interrupt kept
 * refilling it, and the starved SOFTINT_CLOCK heartbeat panicked ("softints
 * stuck").  A thread runs below all softints, so the stack drains what we
 * hand it and the RX ring (hardware drops) absorbs any overload.
 */
static int
edma_intr(void *arg)
{
	struct edma_rxq * const rq = arg;
	struct edma_softc * const sc = rq->rq_sc;

	mutex_enter(&rq->rq_lock);
	rq->rq_ci = curcpu();
	rq->rq_ev_intr.ev_count++;
	if ((RD4(sc, REG_RX_ISR) & __BIT(rq->rq_hw)) == 0)
		rq->rq_ev_noisr.ev_count++;
	WR4(sc, REG_RX_INT_MASK_Q(rq->rq_hw), 0);
	WR4(sc, REG_RX_ISR, __BIT(rq->rq_hw));
	if (sc->sc_running && !rq->rq_queued) {
		rq->rq_queued = true;
		workqueue_enqueue(sc->sc_rxwq, &rq->rq_wk, NULL);
	}
	mutex_exit(&rq->rq_lock);
	return 1;
}

static void
edma_rx_work(struct work *wk, void *arg)
{
	struct edma_rxq * const rq = container_of(wk, struct edma_rxq, rq_wk);
	struct edma_softc * const sc = rq->rq_sc;

	mutex_enter(&rq->rq_lock);
	rq->rq_queued = false;
	if (!sc->sc_running) {
		mutex_exit(&rq->rq_lock);
		return;
	}
	const u_int tail0 = rq->rq_tail;
	edma_rx(sc, rq);
	rq->rq_ev_runs.ev_count++;
	/*
	 * Ack first: a frame arriving after the check re-raises it.  Also
	 * clear the queue's sticky "RFDs running out" bit in MISC_ISR: it
	 * gets set when the ring overflows under load, and while it stays
	 * set the queue's interrupt keeps firing with nothing to do (an
	 * interrupt storm of ~300k/s per queue that starved userland).
	 */
	WR4(sc, REG_RX_ISR, __BIT(rq->rq_hw));
	WR4(sc, REG_MISC_ISR, MISC_ISR_RX_URG_Q(rq->rq_hw));
	const u_int cons = edma_rx_cons(sc, rq);
	if (cons != rq->rq_tail && rq->rq_tail == tail0) {
		/* work pending but nothing processed: report a spin */
		if (++rq->rq_stall == 1000) {
			const struct edma_rrd * const rrd =
			    mtod(rq->rq_m[rq->rq_tail], struct edma_rrd *);
			printf("edma rxq%u: stuck on cpu%u: tail %u cons %u "
			    "RFD_IDX %#x SW_CONS %#x RX_ISR %#x MASK %#x "
			    "rrd1 %#x rrd6 %#x rrd7 %#x runs %u\n", rq->rq_k,
			    cpu_index(curcpu()), rq->rq_tail, cons,
			    RD4(sc, REG_RFD_IDX_Q(rq->rq_hw)),
			    RD4(sc, REG_RX_SW_CONS_IDX_Q(rq->rq_hw)),
			    RD4(sc, REG_RX_ISR),
			    RD4(sc, REG_RX_INT_MASK_Q(rq->rq_hw)),
			    le16toh(rrd->rrd[1]), le16toh(rrd->rrd[6]),
			    le16toh(rrd->rrd[7]), (u_int)rq->rq_ev_runs.ev_count);
		}
	} else
		rq->rq_stall = 0;
	if (rq->rq_tail == tail0 && cons == rq->rq_tail)
		rq->rq_ev_idle.ev_count++;
	/*
	 * rq_queued must be checked: edma_rx() drops rq_lock to hand frames
	 * to the stack, and the tick may have queued us meanwhile.  Queueing
	 * a work twice links it into the workqueue's list in a loop and the
	 * thread runs it forever (seen: ~300k runs/s per queue with nothing
	 * to do, after heavy load; userland starved, console deaf).
	 */
	if (rq->rq_queued) {
		/* already queued again: that run will unmask */
	} else if (cons != rq->rq_tail) {
		/* more work: requeue, so others get a turn in between */
		rq->rq_queued = true;
		workqueue_enqueue(sc->sc_rxwq, &rq->rq_wk, NULL);
	} else
		WR4(sc, REG_RX_INT_MASK_Q(rq->rq_hw), 1);
	mutex_exit(&rq->rq_lock);

	/* this CPU's TX completions are reclaimed in passing (no TX irq) */
	struct edma_txq * const tq =
	    &sc->sc_txq[cpu_index(curcpu()) % EDMA_NTXQ];
	if (mutex_tryenter(&tq->tq_lock)) {
		edma_tx_kick(sc, tq);
		mutex_exit(&tq->tq_lock);
	}
}

/* backstop for lost RX interrupts, and TX reclaim when idle */
static void
edma_tick(void *arg)
{
	struct edma_softc * const sc = arg;

	if (!sc->sc_running)
		return;
	/*
	 * Never process RX here: a flow's frames must all be forwarded by
	 * the same CPU, hence through the same TX queue, or they get
	 * reordered (the engine interleaves its TX queues).  Hand pending
	 * work to the queue's own CPU instead.
	 */
	for (u_int k = 0; k < EDMA_NRXQ; k++) {
		struct edma_rxq * const rq = &sc->sc_rxq[k];

		mutex_enter(&rq->rq_lock);
		if (!rq->rq_queued && edma_rx_cons(sc, rq) != rq->rq_tail) {
			rq->rq_queued = true;
			workqueue_enqueue(sc->sc_rxwq, &rq->rq_wk, rq->rq_ci);
		}
		mutex_exit(&rq->rq_lock);
	}
	for (u_int k = 0; k < EDMA_NTXQ; k++) {
		struct edma_txq * const tq = &sc->sc_txq[k];

		mutex_enter(&tq->tq_lock);
		edma_tx_kick(sc, tq);
		mutex_exit(&tq->tq_lock);
	}
	/* hw.qcomedma0.watch=N: one line of queue state every N seconds */
	if (sc->sc_watch > 0 &&
	    ++sc->sc_watch_ticks >= (u_int)sc->sc_watch * 10) {
		sc->sc_watch_ticks = 0;
		printf("edmaw cpu%u t=%lld rx", cpu_index(curcpu()),
		    (long long)time_uptime);
		for (u_int k = 0; k < EDMA_NRXQ; k++) {
			struct edma_rxq * const rq = &sc->sc_rxq[k];
			printf(" %u:%u/%u%s%s,ok%u,run%u,irq%u,noisr%u,idle%u",
			    k, rq->rq_tail,
			    edma_rx_cons(sc, rq), rq->rq_queued ? "Q" : "",
			    RD4(sc, REG_RX_INT_MASK_Q(rq->rq_hw)) ? "" : "M",
			    (u_int)rq->rq_ev_frames.ev_count,
			    (u_int)rq->rq_ev_runs.ev_count,
			    (u_int)rq->rq_ev_intr.ev_count,
			    (u_int)rq->rq_ev_noisr.ev_count,
			    (u_int)rq->rq_ev_idle.ev_count);
		}
		printf(" tx");
		for (u_int k = 0; k < EDMA_NTXQ; k++) {
			struct edma_txq * const tq = &sc->sc_txq[k];
			printf(" %u:%u-%u/%u,q%d", k, tq->tq_tail, tq->tq_head,
			    (u_int)__SHIFTOUT(RD4(sc, REG_TPD_IDX_Q(tq->tq_hw)),
			    TPD_CONS_IDX), tq->tq_ifq.ifq_len);
		}
		printf(" isr %#x misc %#x\n", RD4(sc, REG_RX_ISR),
		    RD4(sc, REG_MISC_ISR));
	}
	mutex_enter(&sc->sc_lock);
	if (sc->sc_running)
		callout_schedule(&sc->sc_tick, hz / 10);
	mutex_exit(&sc->sc_lock);
}

/* RX queue k's interrupt to CPU k (once all CPUs run) */
static void
edma_bind_irqs(struct edma_softc *sc)
{
	kcpuset_t *set;

	if (sc->sc_irqs_bound || ncpu < 2)
		return;
	sc->sc_irqs_bound = true;
	kcpuset_create(&set, true);
	for (u_int k = 0; k < EDMA_NRXQ; k++) {
		struct edma_rxq * const rq = &sc->sc_rxq[k];
		const cpuid_t cpu = k % ncpu;
		int error;

		if (rq->rq_ih == NULL)
			continue;
		kcpuset_zero(set);
		kcpuset_set(set, cpu);
		error = interrupt_distribute_handler(rq->rq_intrid, set, NULL);
		if (error != 0)
			device_printf(sc->sc_dev, "can't bind %s to cpu%lu: "
			    "%d\n", rq->rq_intrid, (u_long)cpu, error);
	}
	kcpuset_destroy(set);
}

/* --- ifnet ---------------------------------------------------------- */

static int
edma_init(struct ifnet *ifp)
{
	struct edma_softc * const sc = ifp->if_softc;

	edma_bind_irqs(sc);	/* takes cpu_lock: not under sc_lock */
	mutex_enter(&sc->sc_lock);
	if (!sc->sc_running) {
		edma_hw_init(sc);
		sc->sc_running = true;
		callout_schedule(&sc->sc_tick, 1);
	}
	ifp->if_flags |= IFF_RUNNING;
	mutex_exit(&sc->sc_lock);
	return 0;
}

static void
edma_stop(struct ifnet *ifp, int disable)
{
	struct edma_softc * const sc = ifp->if_softc;

	mutex_enter(&sc->sc_lock);
	sc->sc_running = false;
	edma_hw_stop(sc);
	ifp->if_flags &= ~IFF_RUNNING;
	mutex_exit(&sc->sc_lock);
	callout_halt(&sc->sc_tick, NULL);
	for (u_int k = 0; k < EDMA_NRXQ; k++)
		workqueue_wait(sc->sc_rxwq, &sc->sc_rxq[k].rq_wk);
}

static int
edma_ioctl(struct ifnet *ifp, u_long cmd, void *data)
{
	struct edma_softc * const sc = ifp->if_softc;
	int s, error;

	s = splnet();
	error = ether_ioctl(ifp, cmd, data);
	if (error == ENETRESET) {
		/* "ifconfig qcomedma0 [-]vlanhwtag"; no multicast filter */
		if (cmd == SIOCSETHERCAP)
			M4(sc, REG_RXQ_CTRL, RXQ_CTRL_RMV_VLAN,
			    (sc->sc_ec.ec_capenable & ETHERCAP_VLAN_HWTAGGING) ?
			    RXQ_CTRL_RMV_VLAN : 0);
		error = 0;
	}
	splx(s);
	return error;
}

static int
edma_mediachange(struct ifnet *ifp)
{
	return 0;
}

static void
edma_mediastatus(struct ifnet *ifp, struct ifmediareq *ifmr)
{
	/* internal 1G link to the switch (fixed-link) */
	ifmr->ifm_status = IFM_AVALID | IFM_ACTIVE;
	ifmr->ifm_active = IFM_ETHER | IFM_1000_T | IFM_FDX;
}

/*
 * Tuning and debugging knobs under hw.<xname> (e.g. hw.qcomedma0.watch):
 *   dump      write 1: print EDMA, queue and switch state to the console
 *   prof      write 1: print and clear per-CPU cycle accounting
 *   watch     N > 0: print one line of queue state every N seconds
 *   rss_type  RSS_TYPE register: which headers the RX hash covers
 *   tso_fromcpu  experiment: FROM_CPU bit in TSO TPDs (TSO is off)
 */
static void
edma_sysctl_attach(struct edma_softc *sc)
{
	struct sysctllog **log = &sc->sc_sysctllog;
	const struct sysctlnode *node;

	if (sysctl_createv(log, 0, NULL, &node, 0, CTLTYPE_NODE,
	    device_xname(sc->sc_dev), SYSCTL_DESCR("qcomedma tuning/debug"),
	    NULL, 0, NULL, 0, CTL_HW, CTL_CREATE, CTL_EOL) != 0) {
		aprint_error_dev(sc->sc_dev, "couldn't create sysctl node\n");
		return;
	}
	sysctl_createv(log, 0, &node, NULL, CTLFLAG_READWRITE, CTLTYPE_INT,
	    "dump", SYSCTL_DESCR("write 1 to print EDMA state"),
	    edma_sysctl_dump, 0, (void *)sc, 0, CTL_CREATE, CTL_EOL);
	sysctl_createv(log, 0, &node, NULL, CTLFLAG_READWRITE, CTLTYPE_INT,
	    "prof", SYSCTL_DESCR("write 1 to print and clear cycle accounting"),
	    edma_sysctl_prof, 0, NULL, 0, CTL_CREATE, CTL_EOL);
	sysctl_createv(log, 0, &node, NULL, CTLFLAG_READWRITE, CTLTYPE_INT,
	    "watch", SYSCTL_DESCR("print queue state every N seconds"),
	    NULL, 0, &sc->sc_watch, 0, CTL_CREATE, CTL_EOL);
	sysctl_createv(log, 0, &node, NULL, CTLFLAG_READWRITE, CTLTYPE_INT,
	    "rss_type", SYSCTL_DESCR("RSS_TYPE register: 0x02 v4 TCP, "
	    "0x04 v6 TCP, 0x08 v4 UDP, 0x10 v6 UDP, 0x20 v4, 0x40 v6"),
	    edma_sysctl_rss_type, 0, (void *)sc, 0, CTL_CREATE, CTL_EOL);
	sysctl_createv(log, 0, &node, NULL, CTLFLAG_READWRITE, CTLTYPE_INT,
	    "tso_fromcpu", SYSCTL_DESCR("TSO TPDs: FROM_CPU bit"),
	    NULL, 0, &sc->sc_tso_fromcpu, 0, CTL_CREATE, CTL_EOL);
}

/* event counters, shown by "vmstat -e" (group "qcomedma0 rxq0" etc.) */
static void
edma_evcnt_attach(struct edma_softc *sc)
{
	for (u_int k = 0; k < EDMA_NRXQ; k++) {
		struct edma_rxq * const rq = &sc->sc_rxq[k];
		const char *g = rq->rq_evgroup;

		snprintf(rq->rq_evgroup, sizeof(rq->rq_evgroup), "%s rxq%u",
		    device_xname(sc->sc_dev), k);
		evcnt_attach_dynamic(&rq->rq_ev_frames, EVCNT_TYPE_MISC, NULL,
		    g, "frames");
		evcnt_attach_dynamic(&rq->rq_ev_intr, EVCNT_TYPE_INTR, NULL,
		    g, "interrupts");
		evcnt_attach_dynamic(&rq->rq_ev_noisr, EVCNT_TYPE_MISC, NULL,
		    g, "interrupts without RX_ISR bit");
		evcnt_attach_dynamic(&rq->rq_ev_runs, EVCNT_TYPE_MISC, NULL,
		    g, "thread runs");
		evcnt_attach_dynamic(&rq->rq_ev_idle, EVCNT_TYPE_MISC, NULL,
		    g, "thread runs with nothing to do");
		evcnt_attach_dynamic(&rq->rq_ev_novalid, EVCNT_TYPE_MISC, NULL,
		    g, "RFDs without a valid RRD");
		evcnt_attach_dynamic(&rq->rq_ev_bad, EVCNT_TYPE_MISC, NULL,
		    g, "bad frames");
	}
	for (u_int k = 0; k < EDMA_NTXQ; k++) {
		struct edma_txq * const tq = &sc->sc_txq[k];

		snprintf(tq->tq_evgroup, sizeof(tq->tq_evgroup), "%s txq%u",
		    device_xname(sc->sc_dev), k);
		evcnt_attach_dynamic(&tq->tq_ev_tso, EVCNT_TYPE_MISC, NULL,
		    tq->tq_evgroup, "TSO frames");
	}
}

static int
edma_match(device_t parent, cfdata_t cf, void *aux)
{
	struct fdt_attach_args * const faa = aux;

	return of_compatible_match(faa->faa_phandle, compat_data);
}

static void
edma_attach(device_t parent, device_t self, void *aux)
{
	struct edma_softc * const sc = device_private(self);
	struct fdt_attach_args * const faa = aux;
	const int phandle = faa->faa_phandle;
	struct ifnet * const ifp = &sc->sc_ec.ec_if;
	uint8_t enaddr[ETHER_ADDR_LEN];
	bus_addr_t addr;
	bus_size_t size;
	int nsegs, error;

	sc->sc_dev = self;
	sc->sc_phandle = phandle;
	sc->sc_rss_type = RSS_TYPE_TCPUDP;
	sc->sc_tso_fromcpu = 1;
	sc->sc_bst = faa->faa_bst;
	sc->sc_dmat = faa->faa_dmat;

	if (fdtbus_get_reg(phandle, 0, &addr, &size) != 0 ||
	    bus_space_map(sc->sc_bst, addr, size, 0, &sc->sc_bsh) != 0) {
		aprint_error(": couldn't map registers\n");
		return;
	}

	error = bus_dmamem_alloc(sc->sc_dmat, DMA_SIZE, PAGE_SIZE, 0,
	    &sc->sc_seg, 1, &nsegs, BUS_DMA_WAITOK);
	if (error == 0)
		error = bus_dmamem_map(sc->sc_dmat, &sc->sc_seg, nsegs,
		    DMA_SIZE, (void **)&sc->sc_va,
		    BUS_DMA_WAITOK);	/* cached: buffers are synced explicitly */
	if (error == 0)
		error = bus_dmamap_create(sc->sc_dmat, DMA_SIZE, 1, DMA_SIZE,
		    0, BUS_DMA_WAITOK, &sc->sc_map);
	if (error == 0)
		error = bus_dmamap_load(sc->sc_dmat, sc->sc_map, sc->sc_va,
		    DMA_SIZE, NULL, BUS_DMA_WAITOK);
	if (error != 0) {
		aprint_error(": couldn't set up DMA memory (%d)\n", error);
		return;
	}
	sc->sc_pa = sc->sc_map->dm_segs[0].ds_addr;

	int maclen;
	const void *mac = fdtbus_get_prop(phandle, "local-mac-address", &maclen);
	if (mac != NULL && maclen == ETHER_ADDR_LEN)
		memcpy(enaddr, mac, ETHER_ADDR_LEN);
	else {
		static const uint8_t def[ETHER_ADDR_LEN] =
		    { 0x02, 0x00, 0x00, 0x00, 0x75, 0x30 };
		memcpy(enaddr, def, ETHER_ADDR_LEN);
	}

	mutex_init(&sc->sc_lock, MUTEX_DEFAULT, IPL_NET);
	callout_init(&sc->sc_tick, CALLOUT_MPSAFE);
	callout_setfunc(&sc->sc_tick, edma_tick, sc);

	aprint_naive("\n");
	aprint_normal(": IPQ40xx ESS EDMA, %u RX queue%s, %u TX queue%s, "
	    "DMA at %#lx\n", EDMA_NRXQ, EDMA_NRXQ > 1 ? "s (RSS)" : "",
	    EDMA_NTXQ, EDMA_NTXQ > 1 ? "s" : "", (u_long)sc->sc_pa);

	sc->sc_nrx = EDMA_NRX;
	sc->sc_ntx = EDMA_NTX;
	/* descriptor rings: one coherent area, uncached, one page per ring */
	error = bus_dmamem_alloc(sc->sc_dmat, EDMA_RING_SIZE, PAGE_SIZE, 0,
	    &sc->sc_ringseg, 1, &nsegs, BUS_DMA_WAITOK);
	if (error == 0)
		error = bus_dmamem_map(sc->sc_dmat, &sc->sc_ringseg, nsegs,
		    EDMA_RING_SIZE, (void **)&sc->sc_ring,
		    BUS_DMA_WAITOK | BUS_DMA_COHERENT);
	if (error == 0)
		error = bus_dmamap_create(sc->sc_dmat, EDMA_RING_SIZE, 1,
		    EDMA_RING_SIZE, 0, BUS_DMA_WAITOK, &sc->sc_ringmap);
	if (error == 0)
		error = bus_dmamap_load(sc->sc_dmat, sc->sc_ringmap,
		    sc->sc_ring, EDMA_RING_SIZE, NULL, BUS_DMA_WAITOK);
	if (error != 0) {
		aprint_error_dev(self, "couldn't set up rings (%d)\n", error);
		return;
	}
	memset(sc->sc_ring, 0, EDMA_RING_SIZE);
	const bus_addr_t ringpa = sc->sc_ringmap->dm_segs[0].ds_addr;
	for (u_int k = 0; k < EDMA_NTXQ; k++) {
		struct edma_txq * const tq = &sc->sc_txq[k];

		tq->tq_sc = sc;
		tq->tq_k = k;
		tq->tq_hw = EDMA_TXQ_HW(k);
		tq->tq_tpd = (volatile uint32_t *)(sc->sc_ring + EDMA_TPD_OFF(k));
		tq->tq_tpd_pa = ringpa + EDMA_TPD_OFF(k);
		tq->tq_bufoff = (bus_size_t)k * EDMA_NTX * EDMA_TXBUFSZ;
		mutex_init(&tq->tq_lock, MUTEX_DEFAULT, IPL_NET);
		IFQ_SET_MAXLEN(&tq->tq_ifq, IFQ_MAXLEN);
		for (u_int i = 0; i < sc->sc_ntx; i++) {
			if (bus_dmamap_create(sc->sc_dmat, EDMA_TXBUFSZ,
			    EDMA_TX_MAXSEGS, EDMA_TXBUFSZ, 0, BUS_DMA_WAITOK,
			    &tq->tq_map[i]) != 0) {
				aprint_error_dev(self,
				    "couldn't create TX DMA maps\n");
				return;
			}
		}
	}
	for (u_int k = 0; k < EDMA_NRXQ; k++) {
		struct edma_rxq * const rq = &sc->sc_rxq[k];

		rq->rq_sc = sc;
		rq->rq_k = k;
		rq->rq_hw = EDMA_RXQ_HW(k);
		rq->rq_rfd = (volatile uint32_t *)(sc->sc_ring + EDMA_RFD_OFF(k));
		rq->rq_rfd_pa = ringpa + EDMA_RFD_OFF(k);
		mutex_init(&rq->rq_lock, MUTEX_DEFAULT, IPL_NET);
		for (u_int i = 0; i < sc->sc_nrx; i++) {
			if (bus_dmamap_create(sc->sc_dmat, MCLBYTES, 1,
			    MCLBYTES, 0, BUS_DMA_WAITOK, &rq->rq_map[i]) != 0 ||
			    edma_rx_newbuf(sc, rq, i) != 0) {
				aprint_error_dev(self,
				    "couldn't set up RX buffers\n");
				return;
			}
		}
	}
	aprint_normal_dev(self, "rings: %u x TPD x%u at %#lx, "
	    "%u x RFD x%u at %#lx\n", EDMA_NTXQ, sc->sc_ntx,
	    (u_long)(ringpa + EDMA_TPD_OFF(0)), EDMA_NRXQ, sc->sc_nrx,
	    (u_long)(ringpa + EDMA_RFD_OFF(0)));

	if (workqueue_create(&sc->sc_rxwq, device_xname(self), edma_rx_work,
	    NULL, PRI_SOFTNET, IPL_NET, WQ_MPSAFE | WQ_PERCPU) != 0) {
		aprint_error_dev(self, "can't create RX workqueue, polling\n");
	} else for (u_int k = 0; k < EDMA_NRXQ; k++) {
		struct edma_rxq * const rq = &sc->sc_rxq[k];
		char intrstr[128], xname[16];
		const char *p;

		if (!fdtbus_intr_str(phandle, EDMA_RXQ_INTR(k), intrstr,
		    sizeof(intrstr)))
			continue;
		snprintf(xname, sizeof(xname), "%s rx%u", device_xname(self),
		    rq->rq_hw);
		rq->rq_ih = fdtbus_intr_establish_xname(phandle,
		    EDMA_RXQ_INTR(k), IPL_NET, FDT_INTR_MPSAFE, edma_intr, rq,
		    xname);
		if (rq->rq_ih == NULL) {
			aprint_error_dev(self, "couldn't establish %s\n",
			    intrstr);
			continue;
		}
		/* intrctl(8) name, for interrupt_distribute_handler() */
		p = strstr(intrstr, "irq ");
		snprintf(rq->rq_intrid, sizeof(rq->rq_intrid), "armgic %s",
		    p != NULL ? p : intrstr);
		aprint_normal_dev(self, "RX queue %u interrupting on %s\n",
		    rq->rq_hw, intrstr);
	}

	edma_sysctl_attach(sc);
	edma_evcnt_attach(sc);

	strlcpy(ifp->if_xname, device_xname(self), IFNAMSIZ);
	ifp->if_softc = sc;
	ifp->if_flags = IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST;
	ifp->if_extflags = IFEF_MPSAFE;
	ifp->if_capabilities = IFCAP_CSUM_IPv4_Rx |
	    IFCAP_CSUM_TCPv4_Rx | IFCAP_CSUM_TCPv4_Tx |
	    IFCAP_CSUM_UDPv4_Rx | IFCAP_CSUM_UDPv4_Tx | IFCAP_TSOv4;
	ifp->if_start = edma_start;
	ifp->if_transmit = edma_transmit;
	ifp->if_ioctl = edma_ioctl;
	ifp->if_init = edma_init;
	ifp->if_stop = edma_stop;
	IFQ_SET_MAXLEN(&ifp->if_snd, IFQ_MAXLEN);
	IFQ_SET_READY(&ifp->if_snd);

	/*
	 * vlan(4), for the WAN: the switch allows 1522 byte frames, and the
	 * engine inserts and strips tags (so vlan(4) gets the checksum
	 * offloads too)
	 */
	sc->sc_ec.ec_capabilities = ETHERCAP_VLAN_MTU | ETHERCAP_VLAN_HWTAGGING;
	sc->sc_ec.ec_capenable = sc->sc_ec.ec_capabilities;
	sc->sc_ec.ec_ifmedia = &sc->sc_media;
	ifmedia_init(&sc->sc_media, 0, edma_mediachange, edma_mediastatus);
	ifmedia_add(&sc->sc_media, IFM_ETHER | IFM_1000_T | IFM_FDX, 0, NULL);
	ifmedia_set(&sc->sc_media, IFM_ETHER | IFM_1000_T | IFM_FDX);

	if_attach(ifp);
	if_deferred_start_init(ifp, NULL);
	ether_ifattach(ifp, enaddr);
	/*
	 * if_initialize() cleared these: enable the checksum offloads.  TSO
	 * stays off: the engine consumes LSO TPDs (ipqess' layout, with or
	 * without FROM_CPU) but sends nothing, although Linux' TSO works on
	 * this hardware.  "ifconfig qcomedma0 tso4" turns it on to experiment.
	 */
	ifp->if_capenable = ifp->if_capabilities & ~IFCAP_TSOv4;
	ifp->if_csum_flags_tx = M_CSUM_TCPv4 | M_CSUM_UDPv4;
	ifp->if_csum_flags_rx = M_CSUM_IPv4 | M_CSUM_TCPv4 | M_CSUM_UDPv4;
}
