/*
 * usb_msc.c — [M22] USB Mass Storage (Bulk-Only Transport + SCSI).
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * Binds a Bulk-Only / SCSI transparent Mass Storage interface: locates the bulk
 * IN/OUT endpoints, runs INQUIRY + TEST UNIT READY + READ CAPACITY(10) over the
 * CBW/CSW protocol, and exposes the LUN as block device "usb0" backed by polled
 * READ(10)/WRITE(10). FAT32/ext2/ext4 mount on top unchanged.
 */
#include "usb.h"
#include "block.h"
#include "debugcon.h"
#include <stddef.h>

#define CBW_SIG 0x43425355u   // 'USBC'
#define CSW_SIG 0x53425355u   // 'USBS'

static usb_device_t* g_dev;
static uint8_t g_ep_in, g_ep_out;
static uint32_t g_tag;

static uint8_t g_cbw[31] __attribute__((aligned(64)));
static uint8_t g_csw[13] __attribute__((aligned(64)));
static uint8_t g_data[32 * 1024] __attribute__((aligned(64)));
#define DATA_BYTES sizeof(g_data)

static block_dev_t g_blk;
static uint32_t g_sector_size;
static uint64_t g_sector_count;

static void put32le(uint8_t* p, uint32_t v) { p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24; }
static uint32_t get32be(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

// Recover a halted bulk endpoint (xHC reset + ring re-point) and clear the
// device's functional stall (CLEAR_FEATURE ENDPOINT_HALT). ep 0 = EP0 control.
static void clear_halt(uint8_t ep) {
    xhci_reset_endpoint(g_dev, ep);
    usb_control_out(g_dev, 0x02, 1, 0, ep);   // CLEAR_FEATURE(ENDPOINT_HALT)
}

// One Bulk-Only command: CBW (out), optional data phase, CSW (in).
// dir_in: 1 = data flows device->host. Returns 0 on a passing CSW, -1 otherwise.
// Per the BOT spec, a STALL on a bulk endpoint is recoverable: clear the halt and
// (for the status phase) retry reading the CSW once.
static int msc_cmd(int dir_in, const uint8_t* cdb, int cdblen, void* data, uint32_t dlen) {
    for (int i = 0; i < 31; i++) g_cbw[i] = 0;
    put32le(g_cbw, CBW_SIG);
    put32le(g_cbw + 4, ++g_tag);
    put32le(g_cbw + 8, dlen);
    g_cbw[12] = dir_in ? 0x80 : 0x00;   // bmCBWFlags
    g_cbw[13] = 0;                       // LUN 0
    g_cbw[14] = (uint8_t)cdblen;
    for (int i = 0; i < cdblen && i < 16; i++) g_cbw[15 + i] = cdb[i];

    if (xhci_transfer(g_dev, g_ep_out, g_cbw, 31) != 31) {
        if (xhci_last_cc() == XHCI_CC_STALL) clear_halt(g_ep_out);
        return -1;
    }
    if (dlen > 0) {
        uint8_t ep = dir_in ? g_ep_in : g_ep_out;
        int r = xhci_transfer(g_dev, ep, data, (int)dlen);
        if (r < 0) {
            // A stalled data phase is recoverable: clear it and still read the CSW.
            if (xhci_last_cc() == XHCI_CC_STALL) clear_halt(ep);
            else return -1;
        }
    }
    int got = xhci_transfer(g_dev, g_ep_in, g_csw, 13);
    if (got < 0) {
        if (xhci_last_cc() == XHCI_CC_STALL) {
            clear_halt(g_ep_in);
            got = xhci_transfer(g_dev, g_ep_in, g_csw, 13);   // retry CSW once
        }
        if (got < 0) return -1;
    }
    uint32_t sig = (uint32_t)g_csw[0] | ((uint32_t)g_csw[1] << 8) |
                   ((uint32_t)g_csw[2] << 16) | ((uint32_t)g_csw[3] << 24);
    if (sig != CSW_SIG) return -1;
    return (g_csw[12] == 0) ? 0 : -1;    // bCSWStatus
}

static int scsi_test_unit_ready(void) {
    uint8_t cdb[6] = {0};
    return msc_cmd(0, cdb, 6, NULL, 0);
}
static int scsi_request_sense(void) {
    uint8_t cdb[6] = {0x03, 0, 0, 0, 18, 0};
    return msc_cmd(1, cdb, 6, g_data, 18);
}
static int scsi_inquiry(void) {
    uint8_t cdb[6] = {0x12, 0, 0, 0, 36, 0};
    return msc_cmd(1, cdb, 6, g_data, 36);
}
static int scsi_read_capacity(void) {
    uint8_t cdb[10] = {0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    if (msc_cmd(1, cdb, 10, g_data, 8) != 0) return -1;
    uint32_t last_lba = get32be(g_data);
    uint32_t blksz = get32be(g_data + 4);
    g_sector_count = (uint64_t)last_lba + 1;
    g_sector_size = blksz ? blksz : 512;
    return 0;
}

static int msc_rw(uint64_t lba, uint8_t* buf, uint32_t count, int write) {
    uint32_t per = DATA_BYTES / g_sector_size;
    uint32_t done = 0;
    while (done < count) {
        uint32_t chunk = count - done; if (chunk > per) chunk = per;
        uint32_t nbytes = chunk * g_sector_size;
        if (write) for (uint32_t i = 0; i < nbytes; i++) g_data[i] = buf[done * g_sector_size + i];
        uint32_t slba = (uint32_t)(lba + done);
        uint8_t cdb[10] = { (uint8_t)(write ? 0x2A : 0x28), 0,
                            (uint8_t)(slba >> 24), (uint8_t)(slba >> 16),
                            (uint8_t)(slba >> 8), (uint8_t)slba, 0,
                            (uint8_t)(chunk >> 8), (uint8_t)chunk, 0 };
        if (msc_cmd(write ? 0 : 1, cdb, 10, g_data, nbytes) != 0) return -1;
        if (!write) for (uint32_t i = 0; i < nbytes; i++) buf[done * g_sector_size + i] = g_data[i];
        done += chunk;
    }
    return (int)count;
}

static int msc_read(block_dev_t* dev, uint64_t lba, void* buf, uint32_t count) {
    (void)dev; return msc_rw(lba, (uint8_t*)buf, count, 0);
}
static int msc_write(block_dev_t* dev, uint64_t lba, const void* buf, uint32_t count) {
    (void)dev; return msc_rw(lba, (uint8_t*)buf, count, 1);
}

void usb_msc_attach(usb_device_t* d, const usb_endpoint_t* eps, int n, uint8_t iface) {
    (void)iface;
    g_ep_in = g_ep_out = 0;
    for (int i = 0; i < n; i++) {
        if ((eps[i].attr & 0x3) != 2) continue;              // bulk only
        if (eps[i].addr & 0x80) g_ep_in = eps[i].addr; else g_ep_out = eps[i].addr;
    }
    if (!g_ep_in || !g_ep_out) { debugcon_writestring("[MSC] missing bulk endpoints\n"); return; }
    g_dev = d; g_tag = 0;

    // Get Max LUN (class request). Some devices stall it to mean "single LUN" —
    // that halts EP0, so clear it before continuing. We only use LUN 0 anyway.
    {
        uint8_t s[8], maxlun = 0;
        usb_setup(s, 0xA1, 0xFE, 0, iface, 1);
        if (xhci_control(d, s, &maxlun, 1) < 0 && xhci_last_cc() == XHCI_CC_STALL)
            clear_halt(0);
    }

    scsi_inquiry();
    // Clear the power-on Unit Attention: try TEST UNIT READY a few times.
    int ready = 0;
    for (int i = 0; i < 8 && !ready; i++) {
        if (scsi_test_unit_ready() == 0) ready = 1;
        else scsi_request_sense();
    }
    if (scsi_read_capacity() != 0) { debugcon_writestring("[MSC] read capacity failed\n"); return; }

    g_blk.name = "usb0";
    g_blk.sector_size = g_sector_size;
    g_blk.sector_count = g_sector_count;
    g_blk.read = msc_read;
    g_blk.write = msc_write;
    if (block_register(&g_blk) != 0) { debugcon_writestring("[MSC] block_register failed\n"); return; }
    debugcon_writestring("[MSC] usb0 ready sectsz=0x"); debugcon_print_hex(g_sector_size);
    debugcon_writestring(" sectors=0x"); debugcon_print_hex(g_sector_count); debugcon_writestring("\n");
}
