/*
 * SecOS Kernel - Driver Interface Subsystem
 * Developed by iDev srl - Luigi De Astis <l.deastis@idev-srl.com>
 * Open License: Educational / permissive use.
 */
#include "driver_if.h"
#include "terminal.h"
#include "process.h"
#include "debugcon.h"
#include "heap.h" // kmalloc/kfree

static device_desc_t g_devices[MAX_DEVICES];
static int g_device_count = 0;
static driver_binding_t g_bindings[MAX_DRIVER_BINDINGS];

int driver_registry_init(void){
    g_device_count = 0;
    for(int i=0;i<MAX_DEVICES;i++){ g_devices[i].device_id=-1; }
    for(int i=0;i<MAX_DRIVER_BINDINGS;i++){ g_bindings[i].proc=NULL; g_bindings[i].device_id=-1; g_bindings[i].caps=0; }
    // Example seed devices: 0 (timer), 1 (framebuffer).
    // [M11] Device 0 also advertises MAP_MEM support so the granted-capability
    // boundary is meaningful: a driver granted only READ/WRITE/GET_INFO is
    // refused MAP_MEM by its *manifest grant* even though the device supports it.
    g_devices[0].device_id=0; g_devices[0].reg_base=0xF0000000ULL; g_devices[0].reg_size=0x1000; g_devices[0].mem_base=0xF1000000ULL; g_devices[0].mem_size=0x10000; g_devices[0].caps_mask=DEV_CAP_READ_REG|DEV_CAP_WRITE_REG|DEV_CAP_GET_INFO|DEV_CAP_MAP_MEM|DEV_CAP_UNMAP_MEM; g_device_count++;
    g_devices[1].device_id=1; g_devices[1].reg_base=0xE0000000ULL; g_devices[1].reg_size=0x2000; g_devices[1].mem_base=0xE1000000ULL; g_devices[1].mem_size=0x20000; g_devices[1].caps_mask=DEV_CAP_READ_REG|DEV_CAP_GET_INFO; g_device_count++;
    terminal_writestring("[DRV] registry initialized\n");
    return 0;
}

const device_desc_t* driver_get_device(int device_id){
    for(int i=0;i<g_device_count;i++){ if(g_devices[i].device_id==device_id) return &g_devices[i]; }
    return NULL;
}

int driver_register_binding_caps(process_t* p, int device_id, uint32_t caps){
    if(!p) return DRV_ERR_ARGS;
    const device_desc_t* dev = driver_get_device(device_id);
    if(!dev) return DRV_ERR_DEVICE;
    // Never grant more than the device actually supports.
    caps &= dev->caps_mask;
    // Already bound: refresh granted caps (idempotent).
    for(int i=0;i<MAX_DRIVER_BINDINGS;i++){ if(g_bindings[i].proc==p && g_bindings[i].device_id==device_id){ g_bindings[i].caps=caps; return DRV_OK; } }
    // Find free slot
    for(int i=0;i<MAX_DRIVER_BINDINGS;i++){ if(!g_bindings[i].proc){ g_bindings[i].proc=p; g_bindings[i].device_id=device_id; g_bindings[i].caps=caps; return DRV_OK; } }
    return DRV_ERR_PERM; // no free slot
}

// Manual/dev path (shell drvreg): grant the device's full capability set.
int driver_register_binding(process_t* p, int device_id){
    const device_desc_t* dev = driver_get_device(device_id);
    if(!dev) return p ? DRV_ERR_DEVICE : DRV_ERR_ARGS;
    return driver_register_binding_caps(p, device_id, dev->caps_mask);
}

int driver_remove_binding(process_t* p, int device_id){
    if(!p) return DRV_ERR_ARGS;
    for(int i=0;i<MAX_DRIVER_BINDINGS;i++){ if(g_bindings[i].proc==p && g_bindings[i].device_id==device_id){ g_bindings[i].proc=NULL; g_bindings[i].device_id=-1; g_bindings[i].caps=0; return DRV_OK; } }
    return DRV_ERR_DEVICE; // binding not found
}

void driver_remove_all_bindings(process_t* p){
    if(!p) return;
    for(int i=0;i<MAX_DRIVER_BINDINGS;i++){ if(g_bindings[i].proc==p){ g_bindings[i].proc=NULL; g_bindings[i].device_id=-1; g_bindings[i].caps=0; } }
}

int driver_is_bound(process_t* p, int device_id){
    if(!p) return 0;
    for(int i=0;i<MAX_DRIVER_BINDINGS;i++){ if(g_bindings[i].proc==p && g_bindings[i].device_id==device_id) return 1; }
    return 0;
}

uint32_t driver_binding_caps(process_t* p, int device_id){
    if(!p) return 0;
    for(int i=0;i<MAX_DRIVER_BINDINGS;i++){ if(g_bindings[i].proc==p && g_bindings[i].device_id==device_id) return g_bindings[i].caps; }
    return 0;
}

// Traduzione opcode -> capability bit
static uint32_t opcode_cap_bit(int opcode){
    switch(opcode){
        case DRIVER_OP_READ_REG:  return DEV_CAP_READ_REG;
        case DRIVER_OP_WRITE_REG: return DEV_CAP_WRITE_REG;
        case DRIVER_OP_MAP_MEM:   return DEV_CAP_MAP_MEM;
        case DRIVER_OP_UNMAP_MEM: return DEV_CAP_UNMAP_MEM;
        case DRIVER_OP_GET_INFO:  return DEV_CAP_GET_INFO;
        default: return 0;
    }
}

int check_driver_permissions(process_t* p, const driver_call_t* req){
    if(!p||!req) return DRV_ERR_ARGS;
    // [M11] The driver privilege is the security boundary: only a process that
    // the signed manifest marked PROC_TYPE_DRIVER may use SYS_DRIVER at all.
    // A plain user process is refused before any device/binding check.
    if(p->proc_type != PROC_TYPE_DRIVER) return DRV_ERR_NOTDRV;
    const device_desc_t* dev = driver_get_device(req->device_id);
    if(!dev) return DRV_ERR_DEVICE;
    if(!driver_is_bound(p, req->device_id)) return DRV_ERR_PERM;
    uint32_t need = opcode_cap_bit(req->opcode);
    if(need==0) return DRV_ERR_OPCODE;
    // Enforce against the *granted* capability subset (trust-rooted in the
    // manifest), not just what the device supports. A driver granted only
    // READ/WRITE/GET_INFO is refused MAP_MEM even on a device that supports it.
    if(!(driver_binding_caps(p, req->device_id) & need)) return DRV_ERR_PERM;
    if(!(dev->caps_mask & need)) return DRV_ERR_PERM;
    // Range checks
    if(req->opcode==DRIVER_OP_READ_REG || req->opcode==DRIVER_OP_WRITE_REG){
        uint64_t off = req->target;
        if(off < dev->reg_base || off >= dev->reg_base + dev->reg_size) return DRV_ERR_RANGE;
    } else if(req->opcode==DRIVER_OP_MAP_MEM || req->opcode==DRIVER_OP_UNMAP_MEM){
        uint64_t addr = req->target;
        if(addr < dev->mem_base || addr >= dev->mem_base + dev->mem_size) return DRV_ERR_RANGE;
        if((addr & 0xFFFULL)!=0) return DRV_ERR_ARGS; // require page alignment
    }
    // Future flags: DRV_FLAG_DIRECT_ACCESS might require separate capability.
    return DRV_OK;
}

// ================== Rate Limiting ==================
// Semplice finestra scorrevole per (pid, device_id, opcode)
typedef struct rl_entry { int pid; int device_id; int opcode; uint64_t window_start_tick; int count; } rl_entry_t;
#define RL_CAPACITY 64
static rl_entry_t rl_table[RL_CAPACITY];
static int rl_inited=0;
static void rl_init(void){ if(rl_inited) return; for(int i=0;i<RL_CAPACITY;i++){ rl_table[i].pid=-1; rl_table[i].device_id=-1; rl_table[i].opcode=0; rl_table[i].window_start_tick=0; rl_table[i].count=0; } rl_inited=1; }
// Parametri finestra: 100 ticks, max 50 chiamate per tripletta
#define RL_WINDOW_TICKS 100ULL
#define RL_MAX_CALLS    50
extern uint64_t timer_get_ticks(void);
int driver_rate_check(process_t* p, const driver_call_t* req){ rl_init(); if(!p||!req) return DRV_ERR_ARGS; uint64_t now = timer_get_ticks();
    // Cerca entry
    rl_entry_t* slot=NULL; for(int i=0;i<RL_CAPACITY;i++){ if(rl_table[i].pid==p->pid && rl_table[i].device_id==req->device_id && rl_table[i].opcode==req->opcode){ slot=&rl_table[i]; break; } }
    if(!slot){ // trova slot libero
        for(int i=0;i<RL_CAPACITY;i++){ if(rl_table[i].pid==-1){ slot=&rl_table[i]; slot->pid=p->pid; slot->device_id=req->device_id; slot->opcode=req->opcode; slot->window_start_tick=now; slot->count=0; break; } }
        if(!slot) return DRV_OK; // tabella piena => nessun rate limit
    }
    // Reset finestra se scaduta
    if(now - slot->window_start_tick > RL_WINDOW_TICKS){ slot->window_start_tick = now; slot->count=0; }
    if(slot->count >= RL_MAX_CALLS) return DRV_ERR_RATE;
    slot->count++;
    return DRV_OK;
}

// Syscall dispatcher implementation
int handle_driver_call(process_t* cur, driver_call_t* req){
    if(!req) return DRV_ERR_ARGS;
    int perm = check_driver_permissions(cur, req);
    if(perm!=DRV_OK){ driver_audit_log(req, perm, cur); return perm; }
    int rl = driver_rate_check(cur, req); if(rl!=DRV_OK){ driver_audit_log(req, rl, cur); return rl; }
    int result;
    switch(req->opcode){
        case DRIVER_OP_READ_REG: {
            uint64_t val=0; result=read_hardware_register(req->device_id, req->target, &val); if(result==DRV_OK){ req->value=val; } break; }
        case DRIVER_OP_WRITE_REG: {
            result=write_hardware_register(req->device_id, req->target, req->value); break; }
        case DRIVER_OP_MAP_MEM: {
            result=map_device_memory(cur, req->device_id, req->target, 0x1000, req->flags); break; }
        case DRIVER_OP_UNMAP_MEM: {
            result=unmap_device_memory(cur, req->device_id, req->target, 0x1000); break; }
        case DRIVER_OP_GET_INFO: {
            const device_desc_t* d = driver_get_device(req->device_id); if(!d) result=DRV_ERR_DEVICE; else { req->value = d->caps_mask; result=DRV_OK; } break; }
        default: result=DRV_ERR_OPCODE; break;
    }
    if((req->flags & DRV_FLAG_REQUIRE_AUDIT) || result!=DRV_OK){ driver_audit_log(req, result, cur); }
    return result;
}

int driver_syscall(driver_call_t* req){
    extern process_t* sched_get_current(void); process_t* cur = sched_get_current();
    return handle_driver_call(cur, req);
}

// Shadow register simulation: simple per-device buffer
typedef struct shadow_regs { int device_id; uint64_t base; uint64_t size; uint8_t* buf; } shadow_regs_t;
static shadow_regs_t g_shadow[ MAX_DEVICES ];
static int shadow_inited=0;
static void shadow_init(void){ if(shadow_inited) return; for(int i=0;i<MAX_DEVICES;i++){ g_shadow[i].device_id=-1; g_shadow[i].buf=NULL; } shadow_inited=1; }
static shadow_regs_t* shadow_get(int device_id){ shadow_init(); const device_desc_t* d=driver_get_device(device_id); if(!d) return NULL; for(int i=0;i<MAX_DEVICES;i++){ if(g_shadow[i].device_id==device_id) return &g_shadow[i]; } for(int i=0;i<MAX_DEVICES;i++){ if(g_shadow[i].device_id==-1){ g_shadow[i].device_id=device_id; g_shadow[i].base=d->reg_base; g_shadow[i].size=d->reg_size; g_shadow[i].buf=(uint8_t*)kmalloc(d->reg_size); if(g_shadow[i].buf){ for(uint64_t k=0;k<d->reg_size;k++) g_shadow[i].buf[k]=0; } return &g_shadow[i]; } } return NULL; }

int read_hardware_register(int device_id, uint64_t addr, uint64_t* out_value){ if(!out_value) return DRV_ERR_ARGS; const device_desc_t* d=driver_get_device(device_id); if(!d) return DRV_ERR_DEVICE; if(addr<d->reg_base||addr>=d->reg_base+d->reg_size) return DRV_ERR_RANGE; shadow_regs_t* sh=shadow_get(device_id); if(!sh||!sh->buf) return DRV_ERR_PERM; uint64_t off = addr - d->reg_base; // emulate 8-byte read
 if(off+8 > d->reg_size) return DRV_ERR_RANGE; uint64_t val=0; for(int i=0;i<8;i++){ val |= ((uint64_t)sh->buf[off+i]) << (i*8); } *out_value=val; return DRV_OK; }

int write_hardware_register(int device_id, uint64_t addr, uint64_t value){ const device_desc_t* d=driver_get_device(device_id); if(!d) return DRV_ERR_DEVICE; if(addr<d->reg_base||addr>=d->reg_base+d->reg_size) return DRV_ERR_RANGE; shadow_regs_t* sh=shadow_get(device_id); if(!sh||!sh->buf) return DRV_ERR_PERM; uint64_t off=addr - d->reg_base; if(off+8 > d->reg_size) return DRV_ERR_RANGE; for(int i=0;i<8;i++){ sh->buf[off+i] = (value >> (i*8)) & 0xFF; } return DRV_OK; }

// Memory mapping (stub: validation only, no real mapping)
int map_device_memory(process_t* p, int device_id, uint64_t phys_addr, size_t length, int flags){ (void)flags; if(!p) return DRV_ERR_ARGS; const device_desc_t* d=driver_get_device(device_id); if(!d) return DRV_ERR_DEVICE; if(length==0) length=0x1000; if((phys_addr & 0xFFF)!=0) return DRV_ERR_ARGS; if(phys_addr<d->mem_base || (phys_addr+length)>d->mem_base+d->mem_size) return DRV_ERR_RANGE; // TODO: use vmm to map into driver space
 return DRV_OK; }
int unmap_device_memory(process_t* p, int device_id, uint64_t phys_addr, size_t length){ if(!p) return DRV_ERR_ARGS; const device_desc_t* d=driver_get_device(device_id); if(!d) return DRV_ERR_DEVICE; if(length==0) length=0x1000; if((phys_addr & 0xFFF)!=0) return DRV_ERR_ARGS; if(phys_addr<d->mem_base || (phys_addr+length)>d->mem_base+d->mem_size) return DRV_ERR_RANGE; // TODO: vmm unmap
 return DRV_OK; }

// Audit log implementation
static driver_audit_entry_t audit_buf[DRIVER_AUDIT_CAPACITY];
static int audit_index=0; static int audit_full=0;
extern uint64_t timer_get_ticks(void); // ipotetica funzione globale
void driver_audit_log(const driver_call_t* req, int result, process_t* p){ if(!req) return; driver_audit_entry_t e; e.tick=timer_get_ticks(); e.pid=p? (int)p->pid: -1; e.device_id=req->device_id; e.opcode=req->opcode; e.target=req->target; e.value=req->value; e.flags=req->flags; e.result=result; audit_buf[audit_index]=e; audit_index=(audit_index+1)%DRIVER_AUDIT_CAPACITY; if(audit_index==0) audit_full=1;
    // [M11] Mirror every audited event to debugcon so the capability boundary is
    // verifiable non-interactively (denials are always audited; successes only
    // with DRV_FLAG_REQUIRE_AUDIT — see handle_driver_call).
    debugcon_writestring("[DRV-AUDIT] pid="); debugcon_print_hex((uint64_t)(uint32_t)e.pid);
    debugcon_writestring(" dev="); debugcon_print_hex((uint64_t)(uint32_t)e.device_id);
    debugcon_writestring(" op="); debugcon_print_hex((uint64_t)(uint32_t)e.opcode);
    debugcon_writestring(" res="); debugcon_print_hex((uint64_t)(uint32_t)e.result);
    debugcon_writestring("\n");
}
int driver_audit_dump(driver_audit_entry_t* out_buf, int max){ if(!out_buf||max<=0) return 0; int count = audit_full? DRIVER_AUDIT_CAPACITY : audit_index; if(max>count) max=count; // copia partendo dall'entry più recente
 int written=0; for(int i=0;i<max;i++){ int idx = (audit_index - 1 - i); if(idx<0) idx += DRIVER_AUDIT_CAPACITY; out_buf[written++] = audit_buf[idx]; } return written; }
