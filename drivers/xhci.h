/*
 * xhci.h — [M22] xHCI (USB 3) host controller driver.
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * Minimal polled xHCI: bring up the controller, reset+enable each connected
 * root port, enable a device slot, address the device, and run control / bulk /
 * interrupt transfers through per-endpoint TRB rings with polled event-ring
 * completion. The USB core (usb.c) drives enumeration on top of this; the HID
 * and Mass-Storage class drivers use the transfer primitives.
 */
#ifndef XHCI_H
#define XHCI_H
#include <stdint.h>

#define XHCI_RING_SIZE   64
#define XHCI_EPR_PER_DEV 4

typedef struct { volatile uint32_t d[4]; } xhci_trb_t;

typedef struct {
    xhci_trb_t* trb;     // ring storage (virtual)
    uint64_t    phys;    // ring base physical
    uint32_t    enq;     // enqueue index
    uint32_t    cycle;   // producer cycle state
    uint8_t     dci;     // device context index this ring serves (0 = unused)
} xhci_ring_t;

typedef struct usb_device {
    int      slot_id;        // xHC slot (0 = unused)
    int      port;           // 1-based root hub port
    int      speed;          // PORTSC speed field
    int      devidx;         // index into the controller's device table
    uint8_t  config_val;     // bConfigurationValue chosen
    uint16_t vendor, product;
    uint8_t  dev_class, dev_subclass, dev_proto;
    uint8_t* input_ctx;      // input context block
    uint8_t* output_ctx;     // device (output) context block
    xhci_ring_t ep0;         // control endpoint ring (DCI 1)
    xhci_ring_t epr[XHCI_EPR_PER_DEV];
    int      n_epr;
    void*    class_priv;      // bound class driver state
    int      class_id;        // USB_CLASS_* the core bound, or 0
} usb_device_t;

// A parsed endpoint from a configuration descriptor.
typedef struct {
    uint8_t  addr;       // bEndpointAddress (bit7 = IN)
    uint8_t  attr;       // bmAttributes (bits 1:0 = transfer type)
    uint16_t max_packet; // wMaxPacketSize
    uint8_t  interval;   // bInterval
} usb_endpoint_t;

// Probe + initialize the host controller. Returns 0 if an xHCI controller is
// present and came up, -1 otherwise.
int xhci_init(void);

// Enumerate connected root ports: reset, enable slot, address device, and hand
// each addressed device to the USB core. Called after xhci_init().
void xhci_enumerate(void);

// Control transfer on EP0. setup = 8-byte SETUP packet; data may be NULL.
// Returns bytes transferred (>=0) or -1.
int xhci_control(usb_device_t* d, const void* setup, void* data, int len);

// Install the given endpoints on the device (one Configure Endpoint command),
// creating a transfer ring per endpoint. Returns 0 on success.
int xhci_configure_endpoints(usb_device_t* d, const usb_endpoint_t* eps, int n);

// Normal transfer on a previously-configured bulk/interrupt endpoint, addressed
// by bEndpointAddress. Returns bytes transferred (>=0) or -1.
int xhci_transfer(usb_device_t* d, uint8_t ep_addr, void* data, int len);

// Non-blocking variant for interrupt endpoints: enqueue one transfer
// (xhci_submit) and later check for its completion (xhci_poll_transfer, which
// returns 1 and sets *bytes when the event is ready — -1 in *bytes on error —
// else 0). Used to poll the HID keyboard without spinning in the wait loop.
int xhci_submit(usb_device_t* d, uint8_t ep_addr, void* data, int len);
int xhci_poll_transfer(usb_device_t* d, uint8_t ep_addr, int len, int* bytes);

// Update EP0 max packet size (Evaluate Context) after reading the descriptor.
int xhci_set_ep0_mps(usb_device_t* d, uint16_t mps);

// Completion code of the most recent transfer (for error recovery decisions).
#define XHCI_CC_STALL 6
int xhci_last_cc(void);

// Recover a halted endpoint after a STALL: Reset Endpoint + reset its transfer
// ring + Set TR Dequeue Pointer. ep_addr 0 means the control endpoint (EP0).
// Returns 0 on success. The caller still issues CLEAR_FEATURE(ENDPOINT_HALT) to
// the device for a functional stall.
int xhci_reset_endpoint(usb_device_t* d, uint8_t ep_addr);

#endif // XHCI_H
