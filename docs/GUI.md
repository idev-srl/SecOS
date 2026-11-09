# SecOS graphical UI: the secwm compositor and terminal

`secwm` is the SecOS window system: a compositor, a window manager, a
graphical terminal and a small client protocol, all in one signed user-space
program. It builds on the kernel interfaces in `docs/GUI_INPUT.md`:
`/dev/fb0`, evdev, `KDSETMODE` and the keymaps.

## Starting and leaving it

From the bash prompt on the console:

```
startgui                      # same binary as /bin/secwm
```

`startgui` opens one terminal. The other options are:

| Option | Effect |
|---|---|
| `--no-term` | start without a terminal |
| `--run "PROGRAM [ARGS]"` | start an SWP client (repeatable, up to 8) |
| `--trace` | print `SW-*` state markers and every terminal output line to stdout (console + debugcon). Used by the selftest. |
| `--auto` | exit 0 at once unless GUI-at-boot is on (`/etc/gui-autostart`, else the build default; the init hook, below) |

There are three ways back to the text console:

- **Ctrl+Alt+Backspace** (left Alt, see the AltGr note below);
- the **× Exit** button at the right end of the panel;
- the compositor dying for any reason. When the owner of graphics mode exits
  or is killed, the kernel switches the console back to text mode itself
  (`[VT] text console restored (graphics owner exited pid=N)`). The selftest
  kills secwm with SIGSEGV to check this.

On exit secwm closes every pty master, which hangs up the shells in its
terminals (SIGHUP), kills its clients, releases the input grabs and restores
`KD_TEXT`.

In QEMU, use `make run-gui`. It builds the ISO and opens a GTK window with a
PS/2 keyboard and a USB tablet (`-device qemu-xhci -device usb-tablet`), so the
pointer follows the host cursor without a mouse grab. The debugcon log goes to
`secos-gui.log`. The window also has an e1000 NIC with host port
`GUI_SSH_PORT` (default 2222) forwarded to the guest's sshd, so with a baked
key you can log in over SSH while the GUI runs.

Quick start:

    make run-gui SSH_AUTHORIZED_KEYS=~/.ssh/id_ed25519.pub GUI_AUTOSTART=1
    # in SecOS, once:  loadkeys it      (Italian keyboard; saved, see below)
    ssh -p 2222 root@localhost

`make run-gui` boots from the persistent system disk `secos-root.img`
(created on the first run, never overwritten by a build; details in
[docs/devlog/M41-persistent-root.md](devlog/M41-persistent-root.md)). What you
change in SecOS stays: files in `/root` and `/home`, `/etc` edits, the SSH host
key, and the settings below. `make run-gui ROOT=ram` boots without the disk
(RAMFS root, nothing persists); `make reset-root` deletes the disk and starts
over.

The keyboard layout: `loadkeys it` switches it and saves `KEYMAP=it` in
`/etc/vconsole.conf`, which the kernel applies at every boot; `loadkeys -n it`
switches for this boot only. `make run-gui KEYMAP=it` sets the build default,
used only while `/etc/vconsole.conf` does not exist.

### Autostart (opt-in)

The default `/etc/inittab` always has this entry:

    gui:once:/bin/startgui --auto

`startgui --auto` starts the GUI only when GUI-at-boot is on, and otherwise
exits 0 at once:

- `/etc/gui-autostart` holds the setting: `echo 1 > /etc/gui-autostart` turns
  it on, `echo 0 > /etc/gui-autostart` turns it off (also `yes`/`no`,
  `on`/`off`; an empty file means on). On the persistent root the setting
  survives reboots and kernel upgrades.
- Without the file, the build default decides: `make iso GUI_AUTOSTART=1`
  (same as `CFLAGS_EXTRA=-DSECOS_GUI_AUTOSTART`) turns it on. The kernel
  passes it as `secos.gui_autostart=1` on `/proc/cmdline`; the default build
  has `secos.gui_autostart=0`.
- A persistent root created before this entry existed (its `/etc/inittab` is
  kept as it is): add the line above to `/etc/inittab` yourself.

init (PID 1) starts it once, after the `sysinit` entries (so after `ifup`),
next to the console bash; `once` means it is never restarted. Leaving the GUI
(Ctrl+Alt+Backspace, Exit, or a crash) returns the screen to the console bash
that init keeps running underneath: while the GUI owns the screen, console
output is recorded but not drawn, and the keyboard goes only to the GUI
(EVIOCGRAB). The serial console still reaches that bash. The GUI is not the
boot default.

## Using it

- **Panel** (top): the SecOS shield, the launchers **▶ Terminal**,
  **○ Clock** and **■ System**, one task button per window (click to raise
  it), the UTC clock, and **× Exit**. **System** opens `/bin/sysmon`, the
  LVGL system monitor (below). If it is missing, the plain `/bin/sysinfo`
  opens instead. **Task Manager** opens `/bin/taskmgr` (below), also with
  Ctrl+Shift+Esc; there is one instance, a second request raises it.
- **Windows**: click anywhere in a window to focus and raise it. Drag the
  title bar to move it, and click the red × to close it. Terminals also have a
  resize grip in the bottom-right corner; the new size reaches the shell as
  TIOCSWINSZ, which sends SIGWINCH.
- The **title bar badge** is written by the compositor and never by a client.
  It shows `pts/N` for a terminal, `secwm` for built-in windows, and
  `<program> pid <N>` in amber for a client. A client chooses only the text
  after the badge, so it cannot pass itself off as a terminal.
- **Keyboard shortcuts** use Ctrl + left Alt and are never passed to windows:

  | Keys | Action |
  |---|---|
  | Ctrl+Alt+T | new terminal |
  | Ctrl+Alt+N | raise and focus the next window |
  | Ctrl+Alt+W | close the focused window |
  | Ctrl+Alt+C | clock |
  | Ctrl+Alt+S | System panel |
  | Ctrl+Alt+Backspace | leave the GUI |
  | Ctrl+Shift+Esc | Task Manager |

  Windows (WSLg, QEMU GTK) sends AltGr as LeftCtrl+RightAlt. Because the
  shortcuts use the left Alt, AltGr combinations never trigger them.
- **Terminal scrollback**: Shift+PgUp / Shift+PgDn or the mouse wheel. 400
  lines are kept; typing returns to the live screen.

## Architecture

### Process model

```
bash (console) ── startgui = secwm   [CAP_DISPLAY|CAP_INPUT, holds fb0 + evdev, EVIOCGRAB]
                    ├── bash -i        session leader, ctty = pts/0 (terminal window 1)
                    ├── bash -i        session leader, ctty = pts/1 (terminal window 2)
                    └── sysmon         own session, confined manifest, fds 0-4 only (LVGL)
```

- **secwm** is the only process holding `/dev/fb0` and the input devices. It
  opens every keyboard and pointer (probed with `EVIOCGBIT`) and takes
  `EVIOCGRAB`, so the console tty gets no keys while the GUI runs. Every fd it
  opens is `O_CLOEXEC`.
- **Terminals are internal widgets.** For each one, secwm opens a pty pair and
  forks. The child runs `setsid()`, `TIOCSCTTY`, puts the slave on fds 0/1/2,
  closes every other fd, resets signal dispositions and the mask, and execs
  `/bin/bash -i` with `TERM=xterm`. bash is therefore a session leader with
  the pty as its controlling terminal, and job control works: Ctrl+C, Ctrl+Z,
  `fg`, `bg`. The VT emulator (`user/secwm/term.c`) runs inside secwm.
- **Clients** are separate signed programs that secwm starts itself, from the
  launcher or with `--run`. secwm creates two pipes, forks, and in the child
  calls `setsid()`, points stdin at `/dev/null`, puts the pipes on fds 3 and
  4, closes everything else and execs the program. Clients cannot be started
  any other way, so there is no socket to connect to and no rendezvous name
  to guess.

**Why pipes and not shared memory?** SecOS has no shared anonymous memory
between processes (`MAP_SHARED` works only for `/dev/fb0`). A shared-memory
object would need a new kernel primitive (syscalls 90–94 are reserved for it),
with refcounted teardown and a new way for one process to reach another's
pages. Copying pixels through a pipe needs no kernel change. It also keeps
isolation by construction: a client's pixels land only in a buffer secwm
owns, and nothing maps another process's memory. The cost is a copy per
update. That is fine for panels and dialogs (`sysinfo` redraws 420×360 once
a second) but not for video. If a client ever needs more bandwidth, a
`memfd`-style object passed at spawn time is the upgrade path; the protocol
would gain one request naming the object instead of streaming bytes.

### Rendering

- `back` is a shadow copy of the screen (normal RAM). `desk` holds the
  wallpaper, drawn once. Each window has its own buffer with the decorations
  and the content.
- Every change adds a **damage rectangle**. Rectangles that overlap or nearly
  touch are merged; past 32 they collapse into one. Once per loop iteration,
  each damaged rectangle is composed into `back` (wallpaper, then the windows
  bottom to top with a soft shadow, then the panel) and copied to the
  framebuffer. The cursor is drawn on the framebuffer copy only, so moving
  the mouse repaints just its two 12×19 rectangles. The framebuffer is
  write-combining memory and is never read back.
- Terminals render incrementally. term.c remembers what every cell looked
  like when it was last drawn. A scroll moves pixel rows. Only cells that
  differ are redrawn, and the damage is that region, not the window.
- The loop `poll()`s the evdev fds, pty masters and client request pipes. Its
  timeout is the next second boundary, for the panel clock and the client
  ticks. With nothing happening, secwm redraws only the clock area once a
  second.

### Keyboard layouts

secwm compiles the kernel's own `drivers/keymap.c` (the US and Italian tables)
into user space. The tables are shared, not copied. secwm reads the console's
active layout with `ioctl(/dev/console, KDGKBLAYOUT)` at start and again every
second, so `loadkeys it` works from any terminal, even while the GUI runs. The
choice is saved in `/etc/vconsole.conf` and restored at boot.

Key codes come from evdev and go through `keymap_lookup()` with the
Shift/AltGr/Caps state. Special keys use xterm sequences that match the
built-in termcap (`user/termcap.c`): arrows (with DECCKM), Home/End, PgUp/PgDn,
Insert/Delete, F1–F12, Shift+Tab, and the keypad. Ctrl+letter gives a control
code, and left Alt is Meta (an ESC prefix). The Italian layout covers `<>` on
the 102nd key, AltGr `@ # [ ] { } ~ €`, and the accented letters, which reach
bash as UTF-8.

### Terminal emulation

term.c implements enough of VT100/xterm for bash/readline, BusyBox `vi`,
`less`, `top` and lua:

- cursor movement and addressing;
- insert and delete of characters and lines;
- erase in line and display;
- the scroll region (DECSTBM) and origin mode;
- autowrap with a pending wrap;
- insert mode;
- tab stops;
- the alternate screen (`?47/?1047/?1049`) and saved cursor;
- DECCKM and the keypad modes;
- bracketed paste (the mode is tracked);
- SGR attributes: bold, dim, underline, reverse, hidden, strike;
- colours: 16, 256 and 24-bit;
- DEC line drawing;
- OSC 0/2 window titles;
- DSR and DA replies.

The text is drawn with an 8×16 bitmap font converted from **Terminus Font**
(SIL Open Font License 1.1, `third_party/terminus-font/`): 389 glyphs,
regular and bold, covering ASCII, Latin-1, punctuation, arrows, box drawing,
blocks and some shapes.

## SWP: the client protocol

Wire format, constants and structures: `user/secwm/swp.h`. Client helper
library: `user/secwm/swpc.{c,h}`.

A client has two pipes: **fd 3** carries events from the compositor and
**fd 4** carries requests to it. Each message is an 8-byte header
`{u32 type, u32 len}` followed by `len` bytes of payload, little endian. One
connection owns at most one window.

| Request | Payload | Rules |
|---|---|---|
| `HELLO` (1) | `{magic "SWP1", version 1}` | must be first |
| `CREATE` (2) | `{w, h, title[48]}` | once; 32×16 ≤ size ≤ 1024×720 and smaller than the screen |
| `PUT` (3) | `{x, y, w, h}` + `w*h*4` bytes XRGB | after CREATE; the rectangle lies inside the window; `len` must equal `16 + w*h*4` exactly |
| `FILL` (4) | `{x, y, w, h, color}` | after CREATE; inside the window |
| `TITLE` (5) | `title[48]` | after CREATE |

| Event | Payload | When |
|---|---|---|
| `CONFIGURE` (101) | `{w, h}` | the window exists |
| `KEY` (102) | `{code, value, mods, cp}` | only while the window has focus |
| `POINTER` (103) | `{x, y, buttons}` | pointer moves inside the content of the focused window (or while a button press that started there is held) |
| `BUTTON` (104) | `{x, y, button, value}` | press/release inside the content |
| `FOCUS` (105) | `{in}` | focus gained/lost |
| `CLOSE` (106) | none | the user closed the window; exit, or be killed after 2 s |
| `FRAME` (107) | `{ms}` | 1 Hz tick |

Coordinates in events are relative to the window content, clamped to it.

```c
swpc_t c;
swpc_open(&c, 320, 200, "Hello");               /* HELLO + CREATE, waits for CONFIGURE */
gfx_fill(&c.surf, 0, 0, c.w, c.h, 0x203040);    /* draw locally */
swpc_commit(&c, 0, 0, c.w, c.h);                /* PUT */
for (;;) { struct swpc_event e; if (swpc_wait(&c, &e, 1000) < 0 || e.type == SWP_EV_CLOSE) break; }
```

## LVGL clients

LVGL 9.2.2 (MIT) is built from source by `make port-lvgl` (`tools/port-lvgl.sh`).
The script fetches the pinned release and checks its SHA-256. It then compiles
all of `src/` against the SecOS libc with `user/lvgl/lv_conf.h`:

- LVGL's own allocator, string and sprintf functions, and no OS layer;
- software rendering at 32 bpp;
- no file system, image decoder or external library;
- Montserrat 12/14/20/28 and the dark default theme.

The result is linked into `/bin/sysmon` (`user/lvgl/sysmon.c`) and signed
with the confined manifest.

The SWP glue fits in a few lines:

- `lv_display_create(w, h)` with `LV_COLOR_FORMAT_XRGB8888` and
  `LV_DISPLAY_RENDER_MODE_DIRECT`, using the client's local window buffer
  (`swpc_t.px`) as the draw buffer;
- a flush callback that unions the flushed areas and sends them with one
  `swpc_commit()` (PUT) when `lv_display_flush_is_last()`;
- an LVGL pointer input device fed by SWP `POINTER` / `BUTTON` events;
- `lv_tick_set_cb()` on `CLOCK_MONOTONIC`, and a loop that waits for SWP
  events at most until the next LVGL timer.

`sysmon` has three tabs:

- **Overview**: uptime, load, a memory gauge and a 60 s memory chart;
- **Processes**: a table of PID, state, RSS and command from `/proc/<pid>`;
- **About**.

It refreshes on the compositor's 1 Hz `FRAME` event. Another LVGL program
takes the same glue (copy `sysmon.c`'s `main`) and a line in
`tools/port-lvgl.sh`.

### Task Manager (`/bin/taskmgr`)

`user/lvgl/taskmgr.c`, built by the same `make port-lvgl`, modelled on the
Windows Task Manager:

- **Processes**: name, PID, user, status, CPU % (share of the whole machine,
  from the `/proc/<pid>/stat` utime+stime deltas), memory (RSS) and command
  line. Click a column header to sort, again to reverse. CPU and memory cells
  are shaded by load. Keys: arrows, PgUp/PgDn, Home/End, typing a name
  selects the first process that starts with it, Delete = End task.
- **End task** asks for confirmation, sends SIGTERM, and when the process is
  still there 3 s later offers SIGKILL ("End now" / "Wait"). The start time is
  checked before each signal, so a reused pid is never hit. A process of
  another user is refused before any dialog: `kill(pid, 0)` asks the kernel
  (`cred_may_signal`), and EPERM shows "Access denied". There is no "end as
  administrator": that would need an elevation prompt in the GUI.
- **Performance**: a sidebar with CPU, memory and one entry per NIC, each with
  a live mini graph. CPU shows one graph per online CPU (`/proc/stat`),
  utilization, speed, processes, up time; memory shows in use / available /
  total / kernel heap (`/proc/meminfo`); a NIC shows receive and send rates
  and totals (`/proc/net/dev`).
- **Users**: processes, CPU and memory grouped by user.

It refreshes once a second. The list and the graphs are drawn by the program
itself inside LVGL objects and invalidated only where a row or a graph
changed; each flushed area is its own PUT, so an idle refresh sends a few
rows, not the window. secwm starts it with `--max WxH` (the largest window
the screen takes; a larger CREATE would be a protocol violation) and, when
tracing, `--trace` (TASKMGR-* markers for the selftest).

It is signed with `user/note_taskmgr.S`: the confined manifest plus
`CAP_SIGNAL` (`CAP_ENFORCE | CAP_FS_READ | CAP_SIGNAL | CAP_TIME`). The
capability lets it call `kill()`; which processes it may signal is still the
kernel's credential rule: its own user's, or every process when the desktop
runs as root.

## Security model and threat model

**Assets.** What the user types, especially into a shell; what each window
shows; the integrity of what is on screen (no spoofing); and the availability
of the console, so the user can always get back to text mode.

**Trusted.** The kernel, and secwm (signed with `CAP_DISPLAY | CAP_INPUT`:
whoever holds those sees every keystroke and every pixel, as an X server or
Wayland compositor does).

**Untrusted.**
- Every SWP client, even a signed one: signing proves where the code came
  from, not that it behaves.
- Every byte a client sends.
- Programs running in the terminals, as far as other windows are concerned.

### Guarantees and how they are enforced

1. **A client sees only its own input.**
   - Key events go only to the focused window; pointer events only while the
     pointer is inside its content (or during a press that started there).
   - Compositor shortcuts never reach any window.
   - The physical devices are `EVIOCGRAB`'d by secwm. A client cannot open
     them anyway: no `CAP_INPUT`, so `open()` returns `EACCES`.
   - Tested: keys typed into a terminal never show up as `SWT-KEY` in the
     probe client; keys typed into the probe never show up in the terminal.
2. **A client sees only its own pixels.**
   - No request returns pixels or names another window. An unknown request
     type is a protocol violation.
   - A client cannot open `/dev/fb0` (no `CAP_DISPLAY`, `EACCES`) or switch
     the console mode (`KDSETMODE` returns `EPERM`).
   - There is no shared memory to look into.
   - secwm's fds are all close-on-exec and also closed explicitly in the
     child, so a client holds exactly fds 0–4. The probe checks this.
3. **A client cannot write outside its window or inject input elsewhere.**
   - Pixels go only into the client's own window buffer.
   - Before any copy, every rectangle is checked against that window's size
     with overflow-safe arithmetic (`w > cw - x`, never `x + w > cw`). PUT's
     length must match its rectangle exactly, and the pixel data then streams
     straight into rows already proven to be in bounds.
   - There is no event-sending request. A client has no pipe to another
     client or to a terminal.
   - **pty slaves are locked**: secwm sets `TIOCSPTLCK` again on each master
     once bash's slave is open, so `open("/dev/pts/N")` fails with `EIO`.
     Without this, any process could open a terminal's slave by name and read
     its keystrokes. bash and its children are unaffected: they use their
     inherited fds, and `/dev/tty` resolves to the controlling terminal
     without the lock check.
4. **A client cannot take the compositor down.**
   - Every message is parsed as untrusted:
     - a header is checked as soon as its 8 bytes are in;
     - the type must be known, small requests are capped at 256 bytes and PUT
       at one full window;
     - the order is enforced (HELLO, then CREATE, then the rest);
     - titles are sanitised to printable ASCII.
   - Any violation logs `SW-VIOLATION client N pid=P: <reason>`, then
     disconnects and SIGKILLs the client. A client that has not created a
     window within 5 s is dropped the same way.
   - secwm reads at most 256 KB per client per loop iteration, so one client
     cannot starve the rest.
   - Event writes are non-blocking and fit in one atomic pipe write: a client
     that stops reading loses events, and secwm never blocks on it.
   - secwm ignores SIGPIPE.
5. **Clients run with least privilege.**
   - `sysmon`, `sysinfo` and `swptest` are signed with the confined manifest
     (`user/note_confined.S`: `CAP_ENFORCE | CAP_FS_READ | CAP_TIME`). They
     can read files and draw. They cannot write files, fork or exec, signal
     any process (secwm included), use the network, or create IPC. Every
     denial goes to the `[AUDIT]` ring.
   - `taskmgr` has the same manifest plus `CAP_SIGNAL`, so "End task" can
     signal the user's own processes (secwm included, after a warning); the
     kernel refuses other users' processes with EPERM.
   - Each client runs in its own session, so terminal job-control signals
     never reach it.
6. **No spoofing of trusted UI.**
   - Decorations, the panel and the cursor are drawn only by secwm.
   - The title badge (`pts/N` / `secwm` / amber `program pid N`) comes from
     what secwm itself started, not from anything the client says.
7. **The console always comes back.**
   - Ctrl+Alt+Backspace and the Exit button both work.
   - On a crash or kill, the kernel restores text mode when the graphics owner
     exits, and the input grabs are released when their fds close.

### Out of scope / known gaps

- **secwm itself is fully trusted.** A bug in it exposes everything it sees.
  It is about 1,400 lines of C, and every client-facing path is in
  `client_read`, `client_request` and `put_*`.
- **Programs in the terminals are ambient processes.** bash and what it runs
  have the normal ambient privileges of a signed program on SecOS. A program
  started from a GUI terminal can signal other processes, secwm included. That
  is a DoS, and the console comes back. Its own `/dev/fb0` and evdev opens are
  refused, because it lacks the explicit `CAP_DISPLAY` / `CAP_INPUT` bits.
- **The console tty is shared.** Clients and terminal programs may write to
  `/dev/console`. While the GUI runs, that output only reaches the hidden text
  grid (and debugcon).
- **No clipboard and no drag and drop between windows.** These are deliberately
  absent for now. A future clipboard should be compositor-mediated and only
  move data on an explicit user action.
- **Single-user system.** There are no per-user ACLs on `/dev/pts`; the slave
  lock above is what protects terminals.

## Tests

`tools/gui_wm_drive.py` is the gate driver; `tools/selftest.sh` runs it as the
"GUI compositor gate" (log `/tmp/secos_selftest_wm.log`). It boots the ISO
with a PS/2 keyboard and a usb-tablet, starts the GUI over serial and drives
it through QMP:

- **Session 1**
  - The desktop, panel and client pixels are checked in a screendump, which
    must not look like the text console.
  - The client's sandbox probes run: fds, fb0, input, KDSETMODE, ptys,
    kill/fork/write.
  - A click focuses the terminal, and `echo GUI-OK-$((6*7))` typed with
    sendkey must come back as `GUI-OK-42`.
  - Job control: Ctrl+C gives `$?` = 130, Ctrl+Z gives `Stopped`, then `fg`
    and Ctrl+C.
  - `stty size` must print `24 80`.
  - Focus isolation in both directions.
  - The launcher button and Ctrl+Alt+T both open terminals.
  - The System launcher starts the LVGL `sysmon`. It must draw a real UI,
    switch tabs on a click and close with Ctrl+Alt+W.
  - `loadkeys it`, then the AltGr and 102nd-key symbols `@#<>[]{}` must
    arrive.
  - A window is dragged by its title bar.
  - Ctrl+Alt+Backspace must bring back the text console, and bash must still
    answer.
- **Session 2**: five hostile clients (`swptest oob|len|read|early|big`) must
  each be disconnected with the right reason. A good client next to them must
  keep its pixels, and no out-of-bounds magenta pixel may appear anywhere.
- **Session 3**: `kill -SEGV` of secwm must bring back the text console, and
  bash must still answer.

`tools/gui_wm_drive.py --taskmgr [--smp N]` is the **TASKMGR** gate (selftest
"TASKMGR", 34 checks): the desktop runs as an unprivileged user; the task
manager opens from the panel and with Ctrl+Shift+Esc, lists `init` and `bash`,
has one CPU graph per CPU, ends a `sleep` started in a terminal (SIGTERM after
the confirmation), refuses root's `sleep` and `init` (both survive), and the
`[AUDIT]` log has no denial for it. It also checks the `/proc/net/dev` format.

`tools/gui_wm_drive.py --demo --log L --out O --shots DIR` takes screenshots of
`top`, `vi`, `less`, the clock and the LVGL System monitor, for looking at
the result.
