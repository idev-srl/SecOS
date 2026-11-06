/* SecOS <linux/ioctl.h>: the Linux _IOC request encoding (asm-generic layout).
 * SPDX-License-Identifier: MIT */
#ifndef _SECOS_LINUX_IOCTL_H
#define _SECOS_LINUX_IOCTL_H

#define _IOC_NRBITS    8
#define _IOC_TYPEBITS  8
#define _IOC_SIZEBITS  14
#define _IOC_DIRBITS   2
#define _IOC_NRSHIFT   0
#define _IOC_TYPESHIFT (_IOC_NRSHIFT + _IOC_NRBITS)
#define _IOC_SIZESHIFT (_IOC_TYPESHIFT + _IOC_TYPEBITS)
#define _IOC_DIRSHIFT  (_IOC_SIZESHIFT + _IOC_SIZEBITS)
#define _IOC_NONE      0U
#define _IOC_WRITE     1U
#define _IOC_READ      2U

#define _IOC(dir, type, nr, size) \
    (((dir) << _IOC_DIRSHIFT) | ((type) << _IOC_TYPESHIFT) | \
     ((nr) << _IOC_NRSHIFT) | ((size) << _IOC_SIZESHIFT))
#define _IO(type, nr)          _IOC(_IOC_NONE, (type), (nr), 0)
#define _IOR(type, nr, t)      _IOC(_IOC_READ, (type), (nr), (sizeof(t)))
#define _IOW(type, nr, t)      _IOC(_IOC_WRITE, (type), (nr), (sizeof(t)))
#define _IOWR(type, nr, t)     _IOC(_IOC_READ | _IOC_WRITE, (type), (nr), (sizeof(t)))
#define _IOC_DIR(nr)           (((nr) >> _IOC_DIRSHIFT) & ((1 << _IOC_DIRBITS) - 1))
#define _IOC_TYPE(nr)          (((nr) >> _IOC_TYPESHIFT) & ((1 << _IOC_TYPEBITS) - 1))
#define _IOC_NR(nr)            (((nr) >> _IOC_NRSHIFT) & ((1 << _IOC_NRBITS) - 1))
#define _IOC_SIZE(nr)          (((nr) >> _IOC_SIZESHIFT) & ((1 << _IOC_SIZEBITS) - 1))

#endif
