/*
 * secos/udrv_net.h — the ring-3 NIC driver <-> kernel network proxy ABI
 * (Phase 5, M58; docs/PHASE5_PLAN.md D6, docs/devlog/M58-udrv-e1000e.md).
 *
 * The kernel stays the owner of the interface (ethN: IP config, sockets,
 * DHCP, CAP_NET). A ring-3 driver is an L2 device: it moves frames between
 * its NIC and a region the kernel shares with it.
 *
 * UDRV_OP_NET_ATTACH (SYS_UDRV, on a claim whose signed manifest has
 * UDRV_DRV_NET in udrv_flags, of a network-class function with DMA rights)
 * gives the claim:
 *   - the control region: struct udrv_net_ctrl (header, indices, four rings),
 *     zeroed kernel frames mapped RW+NX into the driver (never inherited by
 *     fork, munmap refused). The device cannot reach it;
 *   - UDRV_NET_NSLOTS packet slots of UDRV_NET_SLOT_SIZE bytes, mapped only
 *     into the device's IOMMU domain at slots_iova (slot i at
 *     slots_iova + i * UDRV_NET_SLOT_SIZE). The driver programs these IOVAs
 *     into its NIC descriptors; it never maps the slots itself.
 * The kernel reads the region through its own mapping, never through the
 * driver's. A second NET_ATTACH on the same claim is -EBUSY; the region goes
 * away with the claim (exit, kill, exec, RELEASE) and a new claim attaches
 * again to the same interface (its IP config and sockets survive).
 *
 * Slots: 0 .. UDRV_NET_TX_SLOTS-1 carry frames to send, the rest received
 * frames. At any time each slot is owned by the kernel or by the driver; the
 * kernel tracks this itself and never trusts the shared memory for it.
 *
 * TX (kernel -> NIC):
 *   kernel  copies a frame into a free TX slot, writes tx[tx_prod % TX_RING]
 *           = {slot, len}, then increments tx_prod, and sets UDRV_EV_TX_KICK
 *           on the claim fd (read() returns, poll() is POLLIN);
 *   driver  consumes tx[] from its own index up to tx_prod, points a NIC TX
 *           descriptor at the slot's IOVA, and when the NIC reports it done
 *           writes txc[txc_prod % TX_RING].slot = slot, then increments
 *           txc_prod. The slot is the kernel's again.
 * RX (NIC -> kernel):
 *   kernel  posts free RX slots: rxf[rxf_prod % RX_RING].slot, rxf_prod++;
 *   driver  consumes rxf[] from its own index, gives each slot to a NIC RX
 *           descriptor; for a received frame writes rx[rx_prod % RX_RING] =
 *           {slot, len}, increments rx_prod, and kicks the kernel: a write()
 *           of struct udrv_ack with UDRV_ACK_NET_KICK (the same write acks
 *           and unmasks vectors). The kernel also looks at the rings on its
 *           1 kHz timer tick, so a missed kick costs at most a millisecond.
 *   kernel  validates each entry, copies the frame out, and posts the slot
 *           back on rxf[].
 *   link    the driver writes 1 / 0 to `link` (carrier); `drv_flags` is
 *           the driver's own.
 * Every index is a free-running uint32_t; a ring position is index % ring.
 * Write an entry, then (store barrier) the index.
 *
 * What the kernel checks (the driver is untrusted; each shared field is read
 * once): an index may run ahead of the kernel's own copy by at most the ring
 * size; a slot must be in range and currently owned by the driver for that
 * direction; a received length must be within UDRV_NET_FRAME_MIN..MAX. A bad
 * entry is dropped and logged ("[UDRV] ethN bad ring entry: ..."); after
 * UDRV_NET_VIOL_KILL bad entries the driver is killed (SIGKILL) and init
 * restarts it.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef SECOS_UDRV_NET_H
#define SECOS_UDRV_NET_H
#include <stdint.h>

#define UDRV_OP_NET_ATTACH  11  /* struct udrv_net_attach (in mac, out region) */

/* Manifest v3 udrv_flags: the kernel proxies a driver may attach. */
#define UDRV_DRV_NET        0x1

/* udrv_ack.flags (write() of the claim fd). */
#define UDRV_ACK_NET_KICK   0x1 /* rx[] / txc[] / link changed: look now      */

#define UDRV_NET_MAGIC      0x3154454Eu   /* "NET1" */
#define UDRV_NET_VERSION    1u
#define UDRV_NET_SLOT_SIZE  2048u
#define UDRV_NET_TX_SLOTS   128u
#define UDRV_NET_RX_SLOTS   256u
#define UDRV_NET_TX_BASE    0u
#define UDRV_NET_RX_BASE    UDRV_NET_TX_SLOTS
#define UDRV_NET_NSLOTS     (UDRV_NET_TX_SLOTS + UDRV_NET_RX_SLOTS)
#define UDRV_NET_TX_RING    UDRV_NET_TX_SLOTS   /* tx[] and txc[] */
#define UDRV_NET_RX_RING    UDRV_NET_RX_SLOTS   /* rxf[] and rx[] */
#define UDRV_NET_FRAME_MIN  14u     /* an Ethernet header                     */
#define UDRV_NET_FRAME_MAX  1536u   /* the kernel's NET_FRAME_MAX             */
#define UDRV_NET_VIOL_KILL  8u      /* bad entries per claim before the kill  */

struct udrv_net_desc {
    uint16_t slot;
    uint16_t len;               /* tx[] and rx[]; 0 in txc[] / rxf[]          */
    uint32_t rsvd;
};

struct udrv_net_ctrl {
    /* constant, written by the kernel at NET_ATTACH */
    uint32_t magic, version, nslots, slot_size;
    uint32_t tx_base, tx_slots, rx_base, rx_slots;
    uint32_t tx_ring, rx_ring, ctrl_len, frame_max;
    uint8_t  pad0[16];
    /* kernel -> driver */
    volatile uint32_t tx_prod;  /* entries published in tx[]                  */
    volatile uint32_t rxf_prod; /* entries published in rxf[]                 */
    volatile uint32_t txc_done; /* txc[] entries the kernel consumed (info)   */
    volatile uint32_t rx_done;  /* rx[] entries the kernel consumed (info)    */
    uint8_t  pad1[48];
    /* driver -> kernel */
    volatile uint32_t txc_prod; /* entries published in txc[]                 */
    volatile uint32_t rx_prod;  /* entries published in rx[]                  */
    volatile uint32_t link;     /* 1 = carrier up                             */
    volatile uint32_t drv_flags;
    uint8_t  pad2[112];
    struct udrv_net_desc tx[UDRV_NET_TX_RING];
    struct udrv_net_desc txc[UDRV_NET_TX_RING];
    struct udrv_net_desc rxf[UDRV_NET_RX_RING];
    struct udrv_net_desc rx[UDRV_NET_RX_RING];
};
#define UDRV_NET_CTRL_PAGES 2u      /* sizeof(struct udrv_net_ctrl) = 6400    */

struct udrv_net_attach {
    uint8_t  mac[6];            /* in: the NIC's unicast address              */
    uint16_t flags;             /* in: 0                                      */
    uint32_t pad;
    uint64_t ctrl_uva;          /* out: struct udrv_net_ctrl                  */
    uint64_t ctrl_len;          /* out: bytes mapped                          */
    uint64_t slots_iova;        /* out: device address of slot 0              */
    char     ifname[16];        /* out: "eth0", ...                           */
};

#endif /* SECOS_UDRV_NET_H */
