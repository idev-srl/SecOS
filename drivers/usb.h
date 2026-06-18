/*
 * usb.h — [M22] USB core (enumeration + class dispatch).
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#ifndef USB_H
#define USB_H
#include <stdint.h>
#include "xhci.h"

#define USB_CLASS_HID  0x03
#define USB_CLASS_MSC  0x08

// Descriptor types.
#define USB_DT_DEVICE     1
#define USB_DT_CONFIG     2
#define USB_DT_INTERFACE  4
#define USB_DT_ENDPOINT   5

// Build an 8-byte SETUP packet.
void usb_setup(uint8_t* out, uint8_t bmReqType, uint8_t bReq,
               uint16_t wValue, uint16_t wIndex, uint16_t wLength);

// Standard control helpers (return bytes transferred or -1).
int usb_get_descriptor(usb_device_t* d, uint8_t type, uint8_t index, void* buf, int len);
int usb_set_configuration(usb_device_t* d, uint8_t cfg);
int usb_control_out(usb_device_t* d, uint8_t bmReqType, uint8_t bReq,
                    uint16_t wValue, uint16_t wIndex);

// Called by xhci_enumerate() once a device is addressed: read descriptors,
// select a configuration and bind a class driver.
void usb_attach(usb_device_t* d);

// Bring up the whole USB subsystem (xHCI + enumeration). Returns 0 if a host
// controller was found.
int usb_init(void);

// Class driver entry points (implemented in usb_hid.c / usb_msc.c).
void usb_hid_attach(usb_device_t* d, const usb_endpoint_t* eps, int n, uint8_t iface);
void usb_msc_attach(usb_device_t* d, const usb_endpoint_t* eps, int n, uint8_t iface);

// Polled from the timer tick: drain pending HID keyboard reports.
void usb_hid_poll(void);

#endif // USB_H
