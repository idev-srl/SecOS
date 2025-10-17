/*
 * SecOS Kernel - Driver Interface Definitions
 * Developed by iDev srl - Luigi De Astis <l.deastis@idev-srl.com>
 * Open License: Educational / permissive use.
 */
#pragma once
#include <stdint.h>
#include "process.h" // for process_t

// Opcodes (initial set)
#define DRIVER_OP_READ_REG   0x01
#define DRIVER_OP_WRITE_REG  0x02
#define DRIVER_OP_MAP_MEM    0x03
#define DRIVER_OP_UNMAP_MEM  0x04
#define DRIVER_OP_GET_INFO   0x05

// Flags
#define DRV_FLAG_REQUIRE_AUDIT  0x01
#define DRV_FLAG_DIRECT_ACCESS  0x02  // tentativo di accesso diretto (può essere filtrato)

// Error codes (negative values)
#define DRV_OK             0
#define DRV_ERR_PERM      -1
#define DRV_ERR_RANGE     -2
#define DRV_ERR_OPCODE    -3
#define DRV_ERR_DEVICE    -4
#define DRV_ERR_ARGS      -5
#define DRV_ERR_RATE      -6  // superato limite chiamate nella finestra

typedef struct driver_call {
    int opcode;         // requested operation
    uint64_t target;    // address/offset (register or memory)
    uint64_t value;     // write value / out param for reads
    int flags;          // control/audit flags
    int device_id;      // target device index
} driver_call_t;

// Syscall entrypoint (invoked via SYS_DRIVER)
int driver_syscall(driver_call_t* req);
int handle_driver_call(process_t* p, driver_call_t* req);

// Capability bitmask for each device (bit per supported opcode)
#define DEV_CAP_READ_REG   (1u << 0)
#define DEV_CAP_WRITE_REG  (1u << 1)
#define DEV_CAP_MAP_MEM    (1u << 2)
#define DEV_CAP_UNMAP_MEM  (1u << 3)
#define DEV_CAP_GET_INFO   (1u << 4)

typedef struct device_desc {
    int device_id;            // identifier
    uint64_t reg_base;        // register space base
    uint64_t reg_size;        // register space size
    uint64_t mem_base;        // memory window base (DMA / MMIO)
    uint64_t mem_size;        // memory window size
    uint32_t caps_mask;       // capabilities (DEV_CAP_*)
} device_desc_t;

#define MAX_DEVICES 16
#define MAX_DRIVER_BINDINGS 32

typedef struct driver_binding {
    process_t* proc;    // owning driver process
    int device_id;      // bound device
} driver_binding_t;

// Registry functions
int driver_registry_init(void);
const device_desc_t* driver_get_device(int device_id);
int driver_register_binding(process_t* p, int device_id);
int driver_remove_binding(process_t* p, int device_id);
int driver_is_bound(process_t* p, int device_id);

// Policy check: ritorna DRV_OK o codice errore
int check_driver_permissions(process_t* p, const driver_call_t* req);
// Rate limiting (returns DRV_OK or DRV_ERR_RATE)
int driver_rate_check(process_t* p, const driver_call_t* req);

// Abstract register access
int read_hardware_register(int device_id, uint64_t addr, uint64_t* out_value);
int write_hardware_register(int device_id, uint64_t addr, uint64_t value);

// Device memory mapping into driver space (stub)
int map_device_memory(process_t* p, int device_id, uint64_t phys_addr, size_t length, int flags);
int unmap_device_memory(process_t* p, int device_id, uint64_t phys_addr, size_t length);

// Audit log
#define DRIVER_AUDIT_CAPACITY 128
typedef struct driver_audit_entry {
    uint64_t tick;      // timestamp (es. global ticks)
    int pid;            // processo chiamante
    int device_id;
    int opcode;
    uint64_t target;
    uint64_t value;
    int flags;
    int result;         // DRV_OK o errore
} driver_audit_entry_t;

void driver_audit_log(const driver_call_t* req, int result, process_t* p);
int  driver_audit_dump(driver_audit_entry_t* out_buf, int max); // copy last N entries (newest first)

// Shell helper (implemented in shell.c) for driver commands
void driver_shell_print_audit(int max);
