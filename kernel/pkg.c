/* pkg.c — SecOS signed package install (.spkg). Phase K / M32.
 *
 * A package is sealed with the same Ed25519 root of trust as executables:
 * install = verify the signature, then unpack into the VFS. A forged or
 * tampered package installs nothing. See tools/secos-pkg and docs/SIGNING.md.
 *
 * SPDX-License-Identifier: MIT
 */
#include "pkg.h"
#include "sha256.h"
#include "ed25519.h"
#include "secos_pubkey.h"
#include "debugcon.h"

extern int vfs_create(const char*, const void*, size_t);
extern int vfs_mkdir(const char*);

/* On-wire layout (little-endian, matches tools/secos-pkg). */
struct spkg_hdr {                 /* 64 bytes */
    char     magic[8];           /* "SECOSPKG" */
    uint32_t version;
    uint32_t file_count;
    uint64_t sig_offset;         /* offset of the 64-byte Ed25519 signature */
    uint8_t  reserved[40];
} __attribute__((packed));

struct spkg_ent {                /* 152 bytes */
    char     path[128];
    uint32_t mode;
    uint32_t type;               /* 0 = file, 1 = directory */
    uint64_t data_offset;
    uint64_t data_size;
} __attribute__((packed));

static int magic_ok(const char* m) {
    static const char want[8] = {'S','E','C','O','S','P','K','G'};
    for (int i = 0; i < 8; i++) if (m[i] != want[i]) return 0;
    return 1;
}

int pkg_install(const void* buf, size_t len) {
    const uint8_t* b = (const uint8_t*)buf;
    if (len < sizeof(struct spkg_hdr)) return -1;
    const struct spkg_hdr* h = (const struct spkg_hdr*)b;
    if (!magic_ok(h->magic) || h->version != 1) { debugcon_writestring("[PKG] bad header\n"); return -1; }

    uint64_t sig_off = h->sig_offset;
    uint32_t n = h->file_count;
    if (sig_off + 64 > len) { debugcon_writestring("[PKG] truncated\n"); return -1; }
    if ((uint64_t)sizeof(struct spkg_hdr) + (uint64_t)n * sizeof(struct spkg_ent) > sig_off) {
        debugcon_writestring("[PKG] bad table\n"); return -1;
    }

    /* Verify: digest = SHA-256(file with the 64 sig bytes zeroed), Ed25519. */
    static const uint8_t zeros[64] = {0};
    uint8_t digest[32];
    sha256_ctx c; sha256_init(&c);
    sha256_update(&c, b, sig_off);
    sha256_update(&c, zeros, 64);
    sha256_update(&c, b + sig_off + 64, len - sig_off - 64);
    sha256_final(&c, digest);
    if (ed25519_verify(digest, sizeof(digest), b + sig_off, secos_trusted_pubkey) != 1) {
        debugcon_writestring("[PKG] SIGNATURE INVALID — refusing install\n");
        return -1;
    }

    /* Unpack. Directories first naturally if the packager ordered them so. */
    const struct spkg_ent* ents = (const struct spkg_ent*)(b + sizeof(struct spkg_hdr));
    int done = 0;
    for (uint32_t i = 0; i < n; i++) {
        const struct spkg_ent* e = &ents[i];
        char path[128]; for (int k = 0; k < 127; k++) path[k] = e->path[k]; path[127] = 0;
        if (e->type == 1) {
            vfs_mkdir(path); done++;        /* mkdir is idempotent enough here */
            continue;
        }
        if (e->data_offset + e->data_size > sig_off) { debugcon_writestring("[PKG] bad entry bounds\n"); return -1; }
        if (vfs_create(path, b + e->data_offset, (size_t)e->data_size) == 0) done++;
    }
    debugcon_writestring("[PKG] installed entries=0x"); debugcon_print_hex((uint64_t)done);
    debugcon_writestring("\n");
    return done;
}
