# U5 — Task Manager (`feat/taskmgr`)

**Goal** (`docs/USERLAND_PLAN.md` §3 U5, design §2 D11): a graphical task
manager in the style of the Windows one, `/bin/taskmgr`, an LVGL client of
the secwm compositor, plus the kernel counters it needs (`/proc/net/dev`).

## What was built

### Kernel: per-NIC counters and `/proc/net/dev`

- `net/net.h`: `net_dev_t` gains `net_dev_stats_t stats` with the Linux
  names (`rx/tx_bytes`, `_packets`, `_errors`, `_dropped`), plus two inline
  helpers for the drivers, `net_dev_rx_error()` and `net_dev_rx_dropped()`.
- Who writes what (M47 locking): `net_rx()` counts a delivered frame (or a
  dropped one, bad length) and runs only from `dev->poll()`, that is under the
  NIC's `rx_lock`; `net_dev_xmit()` counts packets/bytes/errors under
  `tx_lock`. So each counter has exactly one writer at a time and needs no
  lock of its own. Readers (procfs) take no lock: an aligned 64-bit load is
  atomic on x86-64; two counters read a moment apart may disagree by one
  frame, as on Linux. No new lock, nothing for `kernel/lock_order.h`.
- Drivers (e1000, e1000e, vmxnet3, igc): a descriptor they cannot deliver
  (not end-of-packet, i.e. the frame spans buffers, or a bad length) counts as
  `rx_errors`. Before, those were dropped silently.
- `fs/procfs.c`: `/proc/net/` (a directory) with `dev` in the Linux layout:
  the two header lines, then `%6s:` + 8 receive and 8 transmit columns.
  `fifo/frame/compressed/multicast/colls/carrier` are 0 (not tracked). There
  is no `lo` line: SecOS has no loopback device.

### Kernel: CPU time — already there

Checked, nothing to add:

- `/proc/stat`: `cpu ` total + one `cpuN` line per online CPU with
  `user nice system idle iowait irq softirq steal guest guest_nice` in USER_HZ
  (from `kiface_account_tick`, which runs on every CPU's tick).
- `/proc/<pid>/stat`: `utime`/`stime` (fields 14/15), `starttime` (22),
  `rss` (24, pages); `/proc/<pid>/status` `Threads: 1` (stays 1 until U9).
- The owner of `/proc/<pid>` is the process's effective uid (M43), so one
  `stat()` gives the user column.

### `/bin/taskmgr` (`user/lvgl/taskmgr.c`)

Built by `make port-lvgl` next to sysmon (same LVGL objects, same SWP glue)
and signed with `user/note_taskmgr.S`: `CAP_ENFORCE | CAP_FS_READ |
CAP_SIGNAL | CAP_TIME` (0xC60).

- **Processes**: Name, PID, User, Status, CPU, Memory, Command line. CPU % is
  the share of the whole machine (Windows' convention): Δ(utime+stime) /
  (elapsed × CPUs). The header shows total CPU and memory % above their
  columns. Sorting by any column (first click on CPU/Memory sorts descending,
  as on Windows), load shading of CPU and memory cells, hover, selection kept
  by PID across refreshes, drag-to-scroll and a draggable scroll bar (SWP has
  no wheel event), keyboard navigation and type-ahead.
- **End task**: `kill(pid, 0)` first. EPERM (another user's process,
  `cred_may_signal`) → "Access denied: "sleep" (PID 9) belongs to root. You
  are signed in as tmu and can end only your own processes." and nothing is
  sent (user decision Q9: no elevation). Otherwise a confirmation (with an
  extra warning for init, the compositor and the task manager itself) →
  SIGTERM; if the process is still alive (and not a zombie) 3 s later, a
  "Not responding" dialog offers "End now" (SIGKILL) or "Wait" (ask again in
  3 s). Before each signal the start time is re-read, so a recycled PID is
  never signalled.
- **Performance**: sidebar (CPU, Memory, one "Ethernet" per NIC, each with a
  live mini graph and a one-line summary); the right pane shows one graph per
  online CPU in a grid chosen for the largest cells, or memory, or a NIC's
  receive/send graph with an auto scale (1-2-5 steps in bits/s); a row of
  stats below (utilization, speed, processes, up time, logical processors /
  in use, available, total, kernel heap / rates, totals, packets, errors).
- **Users** (D11): processes, CPU and memory per user.
- Status bar: processes, CPU, memory, up time.

**Responsiveness.** The process list and the graphs are LVGL objects that draw
themselves in `LV_EVENT_DRAW_MAIN` (rects, lines, triangles, labels). After a
refresh the list compares a hash of every visible row with what it drew and
invalidates only the rows that changed (the header separately); the
performance tab invalidates only the visible graphs. Each flushed area goes out
as its own PUT (sysmon unions them into one bounding box), so an idle refresh
sends a few rows instead of the 880×600 window through the 4 KiB pipe. The
loop waits for SWP events until the next LVGL timer or the next refresh, and a
button event reads the input device at once, so a quick click is never lost.

Graph areas: the area under the line is an opaque, pre-mixed colour made of
one rectangle under the lower end of each segment plus one triangle for the
slope. The first version used two triangles per segment and the shared
anti-aliased diagonal left visible seams.

### secwm

- Panel button **Task Manager** (only when `/bin/taskmgr` exists) and
  **Ctrl+Shift+Esc** (never forwarded to a window). One instance: a second
  request raises and focuses the open window.
- secwm passes `--max WxH`, the largest content size the screen takes
  (`CREATE` larger than that is a protocol violation, and the client cannot
  know the screen size otherwise), and `--trace` when it traces itself. The
  window is 880×600, smaller on small screens.
- The desktop hint line mentions Ctrl+Shift+Esc; `SW-PANEL TaskManager …` in
  the trace.

## Gate TASKMGR

`tools/gui_wm_drive.py --taskmgr [--smp N]`, selftest "TASKMGR" (34 checks,
`-smp $SELFTEST_SMP`, override `TASKMGR_SMP`). The driver:

1. checks the `/proc/net/dev` format over serial (eth0 with non-zero rx/tx
   after the DHCP lease);
2. creates `tmu` (groups video, input), writes `/etc/gui-autostart`
   `user=tmu`, starts `sleep 2000` as root, then `startgui --trace --auto`
   (secwm switches to tmu, the console with it);
3. in the GUI terminal: `sleep 1000 &` (uid 1000);
4. opens the task manager from the panel; checks the window, user tmu,
   `cpus=` and `TASKMGR-CPUGRAPHS` equal to `-smp`, `init` and `bash` in the
   list (with root/tmu owners); focuses the terminal and checks that
   Ctrl+Shift+Esc focuses the same window without a second client;
5. screenshot with > 40 colours; sorts by PID with a header click;
6. clicks the sleep's row (from `TASKMGR-ROWS` geometry), End task, the
   dialog's "End process" (from `TASKMGR-OBJ dlg-ok`): `TASKMGR-TERM rc=0`,
   `TASKMGR-GONE`, gone from the next `TASKMGR-LIST`;
7. selects root's sleep, End task: `TASKMGR-REFUSED … owner=root errno=1`,
   no CONFIRM/TERM for it; then Home + Delete on init: refused too;
8. Performance tab (blue graph pixels, `TASKMGR-NIC eth0`), Users tab,
   Memory and Ethernet panes (screenshots);
9. Ctrl+Alt+W closes it (`SW-CLIENT-EXIT`), Ctrl+Shift+Esc opens a new one,
   Ctrl+Alt+Backspace leaves the GUI;
10. as root on serial: the user's sleep is gone, root's sleep is alive;
    no `[AUDIT] … DENY` for either task-manager pid; no panic.

## Tests run

- TASKMGR gate: `-smp 2` and `-smp 4`: 34/34.
- GUI compositor gate (`tools/gui_wm_drive.py`, `-smp 2`): 57/57 driver
  checks, exactly the 5 hostile clients disconnected.
- M40 GUI gate (`tools/gui_drive.py --scenario main`, `-smp 1` default):
  17/17 driver checks, `G-END … fails=0` for fb, nocap, input, keys, gfx.
- `tools/smoke.sh --mb2` and `--uefi`: PASS.
- `-Wall -Wextra` warnings: 35 before and after, normal and `-DSECOS_LOCKDEP`
  builds (no new warning); taskmgr itself builds clean with `-Wall -Wextra`.

## Deviations from the plan

- No **Threads** column (D11 lists it): it would always show 1 until U9;
  add it with U9.
- `sysmon.elf` is not reproducible across toolchains (the rebuilt header
  differed), so `crypto/user_sysmon_elf.h` was left as it was; only
  `crypto/user_taskmgr_elf.h` (new) and `crypto/user_secwm_elf.h` are
  regenerated.

## Notes for later steps

- `kernel/process.c` `MAX_PROCESSES` is 32 (idle tasks included). The task
  manager lists up to 64; the system limit is the kernel's.
- CPU time is sampled per tick, so per-process CPU % has a resolution of
  1 / (USER_HZ × CPUs) per second (1 % on one CPU, 0.5 % on two).
- `tx_dropped` is never incremented (no TX queue); `rx_dropped` only for
  frames `net_rx` refuses.
- U7 rebuilds taskmgr on musl; the code uses only `open/read/opendir/stat/
  kill/getuid/getppid/clock_gettime` and the SWP pipes.
