/* SecOS BusyBox overlay: <termios.h> = SecOS libc termios (kernel struct
 * layout, Linux flag values and c_cc indices, NCCS=20, no c_line / speed
 * fields) + the remaining Linux names stty needs. Flags the kernel TTY does
 * not implement are stored in the struct and otherwise ignored.
 * SPDX-License-Identifier: MIT */
#ifndef _BB_TERMIOS_H
#define _BB_TERMIOS_H
#include <sys/types.h>
#include_next <termios.h>
/* no line-discipline field in the SecOS struct: alias the spare last c_cc slot */
#define c_line c_cc[NCCS - 1]
/* c_oflag delay values */
#define NL0    0000000
#define NL1    0000400
#define CR0    0000000
#define CR1    0001000
#define CR2    0002000
#define CR3    0003000
#define TAB1   0004000
#define TAB2   0010000
#define BS0    0000000
#define BS1    0020000
#define VT0    0000000
#define VT1    0040000
#define FF0    0000000
#define FF1    0100000
/* c_cflag: the rest of the Linux baud table */
#define EXTA    B19200
#define EXTB    B38400
#define CBAUDEX 0010000
#define B460800 0010004
#define B500000 0010005
#define B576000 0010006
#define B921600 0010007
#define B1000000 0010010
#define B1152000 0010011
#define B1500000 0010012
#define B2000000 0010013
#define B2500000 0010014
#define B3000000 0010015
#define B3500000 0010016
#define B4000000 0010017
#define CIBAUD  002003600000
#define CMSPAR  010000000000
#include <sys/ioctl.h>
#endif
