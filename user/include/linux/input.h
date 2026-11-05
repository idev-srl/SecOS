/* SecOS <linux/input.h>: the Linux evdev ABI served by /dev/input/eventN.
 * Layouts are Linux x86-64's (struct input_event = 24 bytes), so stock evdev
 * readers (LVGL's evdev driver, libevdev-style probing) work unchanged.
 * Opening an event node requires CAP_INPUT in the program's signed manifest
 * (see docs/GUI_INPUT.md).
 * SPDX-License-Identifier: MIT */
#ifndef _SECOS_LINUX_INPUT_H
#define _SECOS_LINUX_INPUT_H
#include <stdint.h>
#include <linux/ioctl.h>
#include <linux/input-event-codes.h>

struct input_event {
    struct { long tv_sec; long tv_usec; } time;   /* layout of struct timeval */
#define input_event_sec  time.tv_sec
#define input_event_usec time.tv_usec
    uint16_t type;
    uint16_t code;
    int32_t  value;
};

#define EV_VERSION 0x010001

struct input_id {
    uint16_t bustype;
    uint16_t vendor;
    uint16_t product;
    uint16_t version;
};

struct input_absinfo {
    int32_t value;
    int32_t minimum;
    int32_t maximum;
    int32_t fuzz;
    int32_t flat;
    int32_t resolution;
};

#define BUS_USB    0x03
#define BUS_I8042  0x11

#define EVIOCGVERSION      _IOR('E', 0x01, int)
#define EVIOCGID           _IOR('E', 0x02, struct input_id)
#define EVIOCGREP          _IOR('E', 0x03, unsigned int[2])
#define EVIOCSREP          _IOW('E', 0x03, unsigned int[2])
#define EVIOCGNAME(len)    _IOC(_IOC_READ, 'E', 0x06, len)
#define EVIOCGPHYS(len)    _IOC(_IOC_READ, 'E', 0x07, len)
#define EVIOCGUNIQ(len)    _IOC(_IOC_READ, 'E', 0x08, len)
#define EVIOCGPROP(len)    _IOC(_IOC_READ, 'E', 0x09, len)
#define EVIOCGKEY(len)     _IOC(_IOC_READ, 'E', 0x18, len)
#define EVIOCGLED(len)     _IOC(_IOC_READ, 'E', 0x19, len)
#define EVIOCGSND(len)     _IOC(_IOC_READ, 'E', 0x1a, len)
#define EVIOCGSW(len)      _IOC(_IOC_READ, 'E', 0x1b, len)
#define EVIOCGBIT(ev, len) _IOC(_IOC_READ, 'E', 0x20 + (ev), len)
#define EVIOCGABS(abs)     _IOR('E', 0x40 + (abs), struct input_absinfo)
#define EVIOCSABS(abs)     _IOW('E', 0xc0 + (abs), struct input_absinfo)
#define EVIOCGRAB          _IOW('E', 0x90, int)
#define EVIOCSCLOCKID      _IOW('E', 0xa0, int)

#endif
