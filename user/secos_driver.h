/* SecOS user-space Driver Space ABI. SPDX-License-Identifier: MIT
 *
 * Mirrors the kernel driver_call_t layout (kernel/driver_if.h) exactly so the
 * SYS_DRIVER copy_from_user/copy_to_user round-trips correctly. A program is
 * only granted driver privilege if its signed .note.secos manifest declares
 * PROC_TYPE_DRIVER (see user/note_driver.S). */
#ifndef SECOS_DRIVER_H
#define SECOS_DRIVER_H
#include <stdint.h>

/* Opcodes (must match kernel/driver_if.h) */
#define DRIVER_OP_READ_REG   0x01
#define DRIVER_OP_WRITE_REG  0x02
#define DRIVER_OP_MAP_MEM    0x03
#define DRIVER_OP_UNMAP_MEM  0x04
#define DRIVER_OP_GET_INFO   0x05

/* Capability bits (DEV_CAP_*) */
#define DEV_CAP_READ_REG   (1u << 0)
#define DEV_CAP_WRITE_REG  (1u << 1)
#define DEV_CAP_MAP_MEM    (1u << 2)
#define DEV_CAP_UNMAP_MEM  (1u << 3)
#define DEV_CAP_GET_INFO   (1u << 4)

/* Flags */
#define DRV_FLAG_REQUIRE_AUDIT  0x01

/* Error codes (kernel returns these via rax) */
#define DRV_OK             0
#define DRV_ERR_PERM      -1
#define DRV_ERR_RANGE     -2
#define DRV_ERR_OPCODE    -3
#define DRV_ERR_DEVICE    -4
#define DRV_ERR_ARGS      -5
#define DRV_ERR_RATE      -6
#define DRV_ERR_NOTDRV    -7   /* caller is not a driver process */

/* Must be byte-identical to the kernel struct (offsetof(value)==16). */
typedef struct driver_call {
    int opcode;
    uint64_t target;
    uint64_t value;
    int flags;
    int device_id;
} driver_call_t;

/* SYS_DRIVER wrapper: returns DRV_OK or a negative DRV_ERR_*. */
long secos_driver(driver_call_t* call);

#endif /* SECOS_DRIVER_H */
