# Driver Space & Mediated Interface (SYS_DRIVER)

This document describes the "Driver Space" subsystem introduced in SecOS to provide a safe, mediated channel between user processes and hardware devices (or their simulations) without directly exposing registers and sensitive memory.

## Motivation

Direct access to MMIO / I/O ports requires ring0 privileges and can compromise stability and security. The classic in-kernel driver model exposes high level APIs; here we add an intermediate layer: a user process requests granular operations on a registered device; the kernel validates capabilities, ranges and permissions before executing.

## Architecture

Main components:

- `device_desc_t` — Device descriptor (reg_base, reg_size, mem_base, mem_size, caps_mask, flags).
- Global device registry — Fixed array of slots populated at initialization (`driver_registry_init()`).
- Driver→device binding — Associated to a process (PCB). Only the bound process can perform calls.
- `driver_call_t` — Structure passed via syscall `SYS_DRIVER`:
  ```c
  typedef struct {
    uint32_t opcode;     // DRIVER_OP_*
    uint32_t device_id;  // registry index
    uint64_t reg_offset; // for READ/WRITE_REG
    uint64_t value;      // value to write / generic argument
    uint64_t mem_offset; // for MAP/UNMAP
    uint64_t mem_length; // region length
    uint32_t flags;      // DRIVER_FLAG_*
  } driver_call_t;
  ```
- Dispatcher `handle_driver_call()` — Switch on opcode, invokes helpers and returns result code.
- Permission engine `check_driver_permissions()` — Checks: device existence, current process binding, opcode allowed by capabilities, valid range.
- Audit log — Circular buffer of events (tick, pid, device, opcode, result). Logs errors or calls with flag `DRV_FLAG_REQUIRE_AUDIT`.

## Supported Opcodes

| Opcode | Description |
|--------|-------------|
| `DRIVER_OP_READ_REG`  | Read register (shadow buffer) at offset `reg_offset`. |
| `DRIVER_OP_WRITE_REG` | Write `value` to register at offset `reg_offset`. |
| `DRIVER_OP_MAP_MEM`   | (Stub) Request mapping of device memory region. Validates page and range. |
| `DRIVER_OP_UNMAP_MEM` | (Stub) Request unmapping of previous region. |
| `DRIVER_OP_GET_INFO`  | Returns basic metadata (caps, size, base) in temp struct or via register pattern. |

Future ops (not yet implemented): DMA_SETUP, IRQ_SUBSCRIBE, BULK_XFER.

## Capability Mask (`caps_mask`)

Bitmask of capabilities enabling individual operation classes:
- `DEV_CAP_READ_REG`
- `DEV_CAP_WRITE_REG`
- `DEV_CAP_MAP_MEM`
- `DEV_CAP_UNMAP_MEM`
- `DEV_CAP_GET_INFO`

Permission check ensures the bit for the opcode is set.

## Result Codes

| Code | Meaning |
|--------|-------------|
| `DRV_OK` | Operation completed successfully |
| `DRV_ERR_DEVICE` | Device missing or not registered |
| `DRV_ERR_BINDING` | Process not the registered driver for device |
| `DRV_ERR_OPCODE` | Opcode unsupported / missing capability |
| `DRV_ERR_RANGE` | Offset / length out of bounds |
| `DRV_ERR_PERM` | Insufficient permissions (flags or policy) |
| `DRV_ERR_ARGS` | Inconsistent arguments |
| `DRV_ERR_INTERNAL` | Unexpected internal error |

## Typical Flow

1. User process is loaded (ELF). After scheduling it appears as `sched_get_current()`.
2. Shell: `drvreg <device_id>` — registers current process as driver for device (if slot free).
3. Process performs syscall:
   ```c
   driver_call_t dc = {0};
   dc.opcode = DRIVER_OP_WRITE_REG;
   dc.device_id = 0;
   dc.reg_offset = 0x4;
   dc.value = 0xABCD1234;
   long res = driver_syscall(&dc); // wrapper user-space
  if (res != DRV_OK) { /* error handling */ }
   ```
4. Kernel executes `driver_syscall()` -> `handle_driver_call()` -> permission check -> operation -> audit log if needed.
5. Shell: `drvlog` — inspect audit (errors or calls with audit flag).

## Security

- No direct MMIO access: operate on register shadow buffer. Prevents real hardware side-effects initially.
- Exclusive binding: only one process can control a device (avoids races & confusion).
- Range validation: prevents overflow and out-of-area reads.
- Capability gating: minimal active surface per device.
- Immutable audit: errors traced for post-mortem analysis.

Future Hardening:
- Opcode rate limiting (flood debounce)
- Audit filtering for targeted queries (errors only, device, opcode)
- DMA sandbox: mapping only to user regions with page pinning
- IRQ subscription with whitelist events
- Isolated address space for driver (micro-kernel style)

## Sample Audit Entry

Structure (simplified):
```
[tick] pid=X dev=Y op=WRITE_REG res=0 (OK)
[tick] pid=X dev=Y op=MAP_MEM res=4 (DRV_ERR_OPCODE)
```

## Related Shell Commands

- `drvinfo` — Show devices and binding state.
- `drvreg <id>` — Register current process as driver (fallback to last created if none scheduled).
- `drvunreg <id>` — Remove binding.
- `drvlog` — Dump audit buffer.
- `drvtest` — Execute test sequence (`user/testdriver.c`).

## Best Practices for Driver Processes

- Always validate syscall result.
- Minimize number of calls: batch grouped operations.
- Do not assume offset zero is always configured: query with `GET_INFO` first.
- Use audit flag only for critical diagnostic operations (reduces noise).

## Planned Extensions

| Feature | Status | Notes |
|---------|-------|------|
| Rate limiting | Design | Counters per (pid, device, opcode) with time window |
| Audit filtering | Design | Shell params: `drvlog errors|dev=<n>|op=<code>` |
| DMA setup | Future | User buffer validation + IOMMU stub |
| IRQ subscribe | Future | List of registered user handlers |
| Bulk transfer | Future | Optimized copy / sg-list |

## FAQ

**Why a user-space driver?** To iterate quickly on prototypes without kernel crashes; gradual transition toward hybrid models.

**How is safe removal done?** `drvunreg` invalidates the binding; subsequent calls from the old process fail (permission check).

**What happens if the process dies?** Currently no auto-cleanup: planned periodic PCB scan to remove orphan bindings.

---
Initial version documentation. Update when new opcodes or capabilities are added.

## Implemented Rate Limiting (Base Version)

A circular table (`RL_CAPACITY=64`) tracks triples (pid, device_id, opcode). For each:
- Time window: 100 ticks
- Max calls: 50 per window
If the limit is exceeded the syscall returns `DRV_ERR_RATE` and is logged in audit.
Table full => no rate limiting (fail-open to avoid accidental boot blocking).

## Advanced Audit Filters

Shell command `drvlog` now supports:
- `errors` — show only events with result != DRV_OK
- `dev=<id>` — filter by device
- `op=<opcode>` — filter by opcode
- `limit=<N>` — max events (1..128, default 32)

Examples:
```
drvlog errors
drvlog dev=0 limit=10
drvlog errors dev=1 op=2
```
