/* pkg.h — SecOS signed package install (.spkg). Phase K / M32.
 * SPDX-License-Identifier: MIT */
#ifndef PKG_H
#define PKG_H
#include <stddef.h>
#include <stdint.h>

/* Verify a .spkg's Ed25519 signature against the trusted key and, if valid,
 * unpack its files/dirs into the VFS. Returns the number of entries installed,
 * or <0 on a bad/forged/short package (nothing is installed in that case). */
int pkg_install(const void* buf, size_t len);

#endif
