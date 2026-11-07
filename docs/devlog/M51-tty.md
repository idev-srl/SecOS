# M51 C5 — Console line discipline and pty locks (`feat/tty-locks`)

Phase C step 5 of the BKL removal (`docs/BKL_REMOVAL_PLAN.md` §4, C5, with
B4 folded in). The BKL stays on. With default settings the system behaves as
before, except that the TTY's two waits now drop the BKL (below). This step
adds the locking that makes the console line discipline, the pty layer, the
console graphics handoff and the kernel shell's directory listings correct
once the BKL is gone (phase D4).

## Lock design

| Lock | Level | Type | Covers |
|---|---|---|---|
| `tty_con_lock` (new, `kernel/tty.c`) | S10 | spinlock irqsave | the console's `struct tty` (`g_con`): termios, line buffer, read queue, VTIME state, `flush_req`/`intr_noproc` consumption; writes of the console foreground group |
| `g_pty_lock` (`kernel/pty.c`, unchanged) | S10 | spinlock irqsave | the pty table, every pty and its `struct tty` |
| `con_lock` (`drivers/vt.c`, unchanged, raw) | S50 | recursive, bounded | vt output; now also the KD mode and its owner (`drivers/fbdev.c`) and the repaint (`fb_console_set_graphics`) |

The two S10 locks are never nested. Both are near the top of the spinlock
order, so only klocks (the BKL) can be held when they are taken.

### Console (`kernel/tty.c`)

- **One lock context per ldisc operation.** The ldisc code is shared by the
  console and every pty slave. A pty is locked by its caller (`pty.c` holds
  `g_pty_lock` for the whole operation). The console takes `tty_con_lock` in
  its own entry points. The shared code gets a `tlk_t` (`{flags, held}`), so
  it can drop and retake the console lock around a wait. For a pty, `tlk_t`
  is empty and the lock calls do nothing. Every public ldisc function
  (`tty_ldisc_read`, `tty_ldisc_readable`, `tty_ldisc_ioctl{,_kbuf}`,
  `tty_input_byte`, `tty_input_room`) takes the console lock when it is given
  the console.
- **Never across a wait.** Two waits are left, and both now drop the lock.
  The first is the 10 ms ESC wait in `pull_input`, for a lone ESC in raw mode
  that comes from the serial line. The second is the `TTY_RD_SPIN` wait of a
  kernel reader in `wait_input`. Both use `tty_wait_irq()`: drop the console
  lock, `bkl_suspend()`, `cpu_wait_irq()`, `bkl_resume()`, retake the lock,
  then re-check (the loop looks at the queue again, `flush_req` is checked
  again). This is the TTY half of the M50 note that waits still ran with the
  BKL held. Other CPUs' syscalls now run during an ESC wait. They can touch
  the console, which is why the lock is needed even with `bkl=on`. A syscall
  in SecOS has no state that another task could change across the wait: a
  process has one thread, and fds are per process.
- **Never across a user copy.** `tty_ioctl` (console termios, winsize,
  FIONREAD, TIOCOUTQ) used to copy to and from user memory inside the ldisc.
  It now bounces the payload, as `pty_ioctl` has done since M50: it copies in
  before the lock and out after it (`con_ldisc_ioctl`).
- **Never across the USB HID poll.** `pull_input` used `keyboard_getc_raw()`,
  which polls the USB HID keyboard (xHCI transfers, `hid_lock` S12 and
  `xhci_lock` S45) on every byte. Under an irqsave lock that meant xHCI
  transfers with interrupts off. Now `pull_input` polls once with the lock
  dropped, then drains the keyboard ring under the lock with the new
  `keyboard_getc_raw_nopoll()`.
- **Bounded hold.** The drain drops and retakes the lock every 32 bytes. Echo
  runs inside the lock, so a 256-byte paste would otherwise keep interrupts
  off for the whole echo. Ordering is kept, because each byte is popped and
  processed under one hold.
- **What runs inside:**
  - `kbd_lock` (S50, the ring);
  - echo through `terminal_write_tty` (`con_lock`, serial);
  - signal posting for ISIG, SIGTTIN and SIGWINCH (`proc_lock` S30, then the
    scheduler wake).

  The console posts at once, as before: a kernel reader that spins in place
  must see its own SIGINT. That order (S10 → S30) is legal.
- **Interrupt and HID context never take it.** `keyboard_key_event` runs
  under `kbd_lock` (S50), from IRQ1 or from the HID poll under `hid_lock`
  (S12). It asks `tty_isig_for()` whether a byte is a signal character, which
  used to read the console termios. Taking S10 there would invert the order,
  and it would deadlock against a CPU echoing under `tty_con_lock`. So:
  - The console termios publishes a **lock-free ISIG snapshot**: one 32-bit
    word with ISIG, NOFLSH, VINTR, VQUIT and VSUSP. It is written under the
    lock by `TCSETS*`, `tty_reset` and init, and read by `tty_isig_for` and
    `tty_note_isig`.
  - `flush_req` stays a volatile flag set by the IRQ. It is consumed under
    the lock.
- **Emulator replies (DSR/DA).** The vt calls `tty_reply` with `con_lock`
  held. That can happen inside an echo on the same CPU, which already holds
  `tty_con_lock`: a terminal echoing `ESC [ 6 n` with ECHOCTL off makes the vt
  answer while the ldisc is mid-echo. Pushing into the read queue under the
  console lock would deadlock that CPU. Replies now go into a lock-free
  256-byte ring. Its producers are serialized by `con_lock`, and
  `pull_input` drains it under the console lock.
- `tty_write` (fd 1/2, `/dev/console`) only reads `onlcr` under the lock. The
  write itself runs unlocked: the vt serializes it, and it can be long.

### Foreground process group (`kernel/signal.c`)

`g_foreground_pgid` is written under `tty_con_lock`
(`signal_set_foreground_pgid` takes it through `tty_con_lock_irqsave()`), so
it never changes in the middle of a console ldisc operation. Once
`tcsetpgrp()` returns, no ISIG or SIGTTIN decision of an operation that
started before it can still target the old group. Reads are lock-free
(`__atomic_load_n`). Its readers are the keyboard IRQ and the HID poll
(S50/S12, inside S10), procfs `stat` (S15, inside S10) and the ldisc itself
under the lock, so readers *cannot* take the lock. The plan's "goes under the
lock" holds for writers only. Never call `signal_set_foreground_pgid` or
`tty_set_fg(console)` with the console lock held: it is not recursive. No
current caller does.

### pty (`kernel/pty.c`) — reviewed, unchanged except a test helper

- Every path into the ldisc (`pty_read` slave, `pty_write` master,
  `pty_poll`, `pty_ioctl` → `tty_ldisc_ioctl{,_kbuf}`) runs under
  `g_pty_lock`. It works on kernel buffers: `ksys_read`/`ksys_write` bounce
  through `uio`, and since M50 `pty_ioctl` copies payloads in before the lock
  and out after it. The M50 fix is complete. The only request without a
  payload that reaches `tty_ldisc_ioctl(t, cmd, arg)` with a user `arg` is
  TCFLSH/TCSBRK/TCXONC, and `arg` is a value there.
- The pty path never reaches a wait. `pull_input` returns at once for a pty,
  and `wait_input` arms `wait_pty` and returns -2.
- Signals raised under the pty lock are queued (`tty_emit_sig`) and posted
  after it (`finish`). `sched_wake_pty(p)` after a `kfree(p)` only compares
  the pointer.
- Left as is (benign, noted for D4): `pty_proc_ctty` reads `t->session`
  unlocked (the caller's own ctty, a 32-bit value), and `ksys_setsid` and
  `pty_console_{sctty,notty}` edit the caller's own `sid`/`pgid`/`ctty_*`
  without `proc_lock`. Other processes read those fields under `proc_lock`
  (`process_foreach`). Each read sees an aligned 32-bit value, old or new.

### KDSETMODE console handoff (`drivers/fbdev.c`, `drivers/fb_console.c`)

`g_kd_mode`/`g_kd_owner` were plain globals, changed by `vtcon_ioctl` and by
`vtcon_proc_exit` (the exit path) with no lock. `fb_console_redraw()` (leaving
graphics mode) also ran without `con_lock`, while another CPU could be
printing into the same shadow buffer and dirty rectangle. That was a
pre-existing SMP race (M40 + M48). Now:

- Mode changes happen under `con_lock`: GRAPHICS, TEXT, the owner's exit.
- `fb_console_set_graphics()` takes `con_lock` (recursive) around the
  repaint.
- `cap_check_explicit` (it audits) and the debugcon messages run outside it.
  For KD_TEXT the capability is looked up with the lock dropped only when the
  state needs it, and the state is then checked again.

The repaint now runs with interrupts off (`con_lock`). It is a one-off at a
mode switch.

## B4: kernel shell listings (`kernel/shell.c`)

`ls_cb` printed through `pager_print`, which shows `--More--` and waits for a
key with the BKL suspended. That wait happened *inside* `vfs_readdir`'s
callback, so inside the filesystem's operation. With phase C FS locks it
would wait with a K30 lock held. Even today, another task could change the
directory under the walk while the BKL was suspended. `ls_cb` and `vls_cb`
now only append lines to a growable buffer (`sh_lines_t`, capped at 1 MiB,
then `... (listing truncated)`). `sh_ls`/`sh_vls` page it after
`vfs_readdir` has returned. `vls` is paged now; it used to print unpaged.

Not changed: `rfls` and `rftree` page over an array of `ramfs_entry_t*` with
the BKL suspended. The entries live in a static table and are never freed,
so the worst case is a stale name, not a use-after-free. They should copy
their entries when ramfs gets its C2 lock.

## Tests

### In-kernel forced concurrency (`kernel/tty_test.c`, `-DSECOS_TTY_TEST`)

The syscall path still takes the BKL, so a userland stress cannot race.
Every CPU drives the console and ptys outside the BKL instead: the BSP from
`kernel_main`, each AP from its idle loop (`tty_test_ap`, hook in
`kernel/smp.c`). The phases:

- **A. Console canonical.** Two CPUs type 2 × 2500 lines atomically into the
  keyboard ring, with flow control (below). Two CPUs read with
  `tty_read_nonblock`. Every CPU also pokes the ldisc: FIONREAD,
  `tty_ldisc_readable`, TCGETS, TIOCGWINSZ, `tty_console_poll`, the
  foreground group. Each line must arrive exactly once and whole, and each
  reader must see each typist's lines in increasing order.
- **B. Echo on.** Echo runs under the lock and takes `con_lock` inside it,
  while another CPU writes to the console.
- **C. Console raw** (VMIN=1). Two typists, one reader, and one CPU pulling
  the ring into the queue through FIONREAD. Each byte encodes
  (typist, seq mod 64): every stream must be complete and in order. 0x1B is
  one of the bytes, so the ESC wait runs; the run counts the waits (~200).
- **D. Reply ring.** `ESC [ 6 n` written to the console must come back as a
  cursor report through the reply ring.
- **E. ptys, raw.** Two pairs, two writers per direction per pair on
  different CPUs, one reader per direction. A 13th role opens, writes,
  polls, reads and closes 300 ptys meanwhile. 8 × 30000 bytes, every stream
  complete and in order.
- **F. pty, canonical.** One writer, readers on all four CPUs and a CPU
  polling: 3000 lines exactly once, in order per reader.
- **N. Negative control.** Phase A with the console lock disabled
  (`g_tty_test_nolock`). The checks must see damage. It is informational in
  the kernel, but the gate requires it.

The line test needs flow control: the console ldisc drops input once its
4 KiB read queue is full, as n_tty does. Without flow control, typists faster
than the readers lose lines for reasons unrelated to locking. The first run
showed exactly that, 715 lines missing and 3 malformed by mid-line queue
overflow. Typists now type only while FIONREAD is below half the queue.

Result on a `-DSECOS_LOCKDEP` build at -smp 4 (TCG): **PASS** in ~3.4 s, every
stream exact. The negative control shows damage every time, for example
`bad=307 dup=21 order=12 missing=328`, or `bad=387 dup=20 order=19
missing=406`. Lockdep stayed silent.

### Userland and console stress (`tools/tty_stress_drive.py`, selftest "M51 TTY")

The driver boots the `-DSECOS_TTY_TEST -DSECOS_LOCKDEP` image at -smp 4 with
QMP. It checks the `[TTYTEST]` result, the negative control and that the ESC
wait ran. Then it runs these steps from bash:

- `/bin/smptest pty 16 64` (new): 16 processes, each with a pty pair. Each
  sends 64 KiB raw master→slave and back slave→master, in chunks of 1..509
  bytes, every byte compared. Then 200 canonical lines with echo: the slave
  reads each line, the master reads its echo with CR LF.
- `/bin/smptest pipe 3 3 512` (new): 3 pipelines of producer | 3 stages |
  consumer, 15 processes. Each moves 512 KiB with varied write and read
  sizes, and every byte and the count are checked.
- **Typing while a background job writes to the console**: a line typed on
  the PS/2 keyboard through QMP sendkey (IRQ1 → ring → ldisc), and one typed
  on the serial line, must reach `read -r` intact.
- **Ctrl-C** (QMP) ends a foreground `sleep 30` at once. **Ctrl-Z** stops a
  `sleep 40` ("Stopped"), and `kill -9 %1` cleans it up. Both go through the
  IRQ ISIG path and the lock-free snapshot.
- The debugcon log must be free of `[LOCKDEP]`, `[LOCK] BUG`, `[EXC]` and
  panics.

Result: **12/12**.

`tools/smp_stress_drive.py` gains the steps `smp-pty` (`smptest pty 16 32`,
also in `--quick`) and `smp-pipe`. `make lockdep-check` therefore exercises
them at -smp 4 and 8.

### Verification

- **Warnings:** a clean build with `-Wall -Wextra` gives **41** warnings in
  the normal, the `-DSECOS_LOCKDEP` and the `-DSECOS_LOCKDEP
  -DSECOS_TTY_TEST` builds, the same as the 42a04cb baseline (none new in a
  touched file). Every code commit of the branch builds in all three
  configurations, with 41 warnings each.
- **Selftest gates** (from `tools/selftest.sh`, private /tmp, normal
  build): **185 passed, 0 failed**.
  - M25: 5.
  - M30: 5.
  - Pseudo-terminals: `ptytest` + `script` relay + pty fuzz, 16.
  - SSH: Dropbear exec, 13.
  - PIPE: integrity, 25.
  - M40 GUI: fb0/evdev/KDSETMODE/keymaps, incl. UEFI, 58.
  - GUI compositor: secwm, terminal on a pty with job control, SWP
    isolation, KEYMAP=it run, 59.
  - Final no-fault checks: 4.
- **The new "M51 TTY" gate:** 13/13.
- **Lockdep GUI drivers:** on a `-DSECOS_LOCKDEP` image,
  `tools/gui_wm_drive.py` 57/57 and `tools/gui_drive.py` 17/17. The only
  `[EXC]` is the deliberate ring-3 crash of the gfx-crash case.
- **`make lockdep-check`: 18/18.** ssd4 and ssd8 include the new `smp-pty`
  and `smp-pipe` steps, and off4 includes `smp-pty`.
- **Kernel shell** (`-DSECOS_NO_INIT -DSECOS_NO_BASH -DSECOS_LOCKDEP`,
  -smp 4, serial): `ls /bin` and `vls /bin` page with `--More--`
  (SPACE/q), and `ls /etc` lists. Lockdep was silent.

## For phase D (D4: TTY/pty/ioctl/poll)

- After this step the console and pty paths hold no lock across a wait, a
  user copy, `sched_block_current` or the return to ring 3. The TTY's own
  waits suspend the BKL. Clearing the read/write/ioctl/poll bits needs
  nothing more from the TTY layer.
- **devfs must not hold its lock across the console.** `devfs_read` on
  `/dev/console` / `/dev/tty` calls `tty_read_try`, and it can reach the ESC
  wait. Once C2 wraps devfs ops in a K30 klock, that wait would run with
  devfs's lock held: `cpu_wait_irq` with a non-BKL klock is a lockdep report,
  and `bkl_suspend` under K30 releases a lower-level klock out of order. The
  devfs console/tty read and write, and the pty nodes, must be dispatched
  outside the devfs instance lock, or `tty_wait_irq` must be taught to
  refuse. The same holds for any future caller of the console ldisc that
  holds a klock other than the BKL.
- `tty_wait_irq()` assumes the BKL is the only klock the task holds.
- **A console reader arms `wait_tty` and is blocked by `syscall_trap.c`
  later.** If the tick's `tty_wake_cb` runs between the two, it sees the
  task not yet BLOCKED and skips it. The periodic wake (every 8 ticks)
  bounds the lost wakeup, as before. Without the BKL the window is the same
  size. A pending emulator reply does not trigger the fast wake either, only
  the periodic one.
- The kernel shell (`keyboard_getchar`) and a console reader both consume
  the keyboard ring and the serial line. With the BKL gone they can run at
  the same time on two CPUs, and a byte goes to whichever pulls first, as
  before.
- `signal_post_pgid` and the other posting paths run under `tty_con_lock`
  (S10 → S30 → scheduler locks). Keep the scheduler's wake path free of
  anything at S10 or below.

## Plan corrections

- `g_foreground_pgid` "goes under the lock" can only mean its writers. Its
  readers include IRQ context, the HID poll and procfs, which sit inside S10
  or cannot spin.
- The plan listed `con_lock` as "already safe". The vt output was, but the
  framebuffer repaint on leaving graphics mode and the KD mode state were not
  under it.
- The keyboard IRQ path could not stay "consistent with the lock" by taking
  it. It needed lock-free data for ISIG and for the emulator's replies,
  because `kbd_lock`, `hid_lock` and `con_lock` holders call into the TTY.
- Beyond `tty.c`, `signal.c`, `shell.c` and `keyboard.c`, the step touched
  `fbdev.c` and `fb_console.c` (handoff) and `pty.c` (a test-only helper).

## Shared files touched (small, additive)

- `kernel/smp.c`: AP idle hook under `#ifdef SECOS_TTY_TEST`.
- `kernel/kernel.c`: test call under `#ifdef SECOS_TTY_TEST`.
- `Makefile`: `kernel/tty_test.c`, which compiles to nothing without the
  define.
- `kernel/lock_order.h`: S10 comment.
- `tools/selftest.sh`: the "M51 TTY" gate, before the summary.
- `tools/smp_stress_drive.py`: two steps.
- `crypto/user_smptest_elf.h`: regenerated (`smptest pty|pipe`).
