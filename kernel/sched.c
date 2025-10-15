#include "sched.h"
#include "terminal.h"

static process_t* current = NULL;
// Strategia semplice: scorri tabella processi e scegli il prossimo con stato NEW/READY.
extern void process_foreach(void (*cb)(process_t*, void*), void* user);

struct pick_ctx { process_t* after; int passed; process_t* cand; };
static void pick_scan_cb(process_t* p, void* user) {
    struct pick_ctx* c = (struct pick_ctx*)user;
    if (p == c->after) { c->passed = 1; return; }
    if (!c->passed && c->after!=NULL) return; // non ancora oltre 'after'
    if (p->state == PROC_NEW || p->state == PROC_READY) { if (!c->cand) c->cand = p; }
}
static process_t* pick_next(process_t* after) {
    struct pick_ctx c = { after, after==NULL?1:0, NULL };
    process_foreach(pick_scan_cb, &c);
    if (!c.cand) {
        // riprova da inizio
        struct pick_ctx c2 = { NULL, 1, NULL };
        process_foreach(pick_scan_cb, &c2);
        return c2.cand;
    }
    return c.cand;
}

void sched_init(void) {
    current = NULL;
}

process_t* sched_get_current(void) { return current; }

int sched_add_process(process_t* p) { (void)p; return 0; }

void sched_yield(void) {
    process_t* next = pick_next(current);
    if (next && next != current) {
        if (current && current->state == PROC_RUNNING) current->state = PROC_READY;
        next->state = PROC_RUNNING;
        current = next;
        // TODO: context switch reale (salvare regs, caricare CR3, ecc.)
    }
}

void sched_on_timer_tick(void) {
    if (current) current->cpu_ticks++;
    // Ogni N tick effettua yield (per ora ogni tick)
    sched_yield();
}
