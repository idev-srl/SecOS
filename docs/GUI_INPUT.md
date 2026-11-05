# GUI kernel interfaces: framebuffer, input, console handoff, keymaps

This is the API contract for a SecOS compositor (and for the LVGL fbdev/evdev
drivers on top of it). Every interface follows the Linux ABI where that is
practical, so stock fbdev and evdev code compiles against `user/include/linux/*`
and runs unchanged. Milestone devlog: `docs/devlog/M40-gui-input.md`.

| Interface | Node / call | Linux equivalent | Capability |
|---|---|---|---|
| Framebuffer | `/dev/fb0` | fbdev | `CAP_DISPLAY` |
| Input devices | `/dev/input/event0..7` | evdev | `CAP_INPUT` |
| Console handoff | `ioctl(tty, KDSETMODE, KD_GRAPHICS / KD_TEXT)` | `KDSETMODE` | `CAP_DISPLAY` for `KD_GRAPHICS` |
| Keyboard layout | `ioctl(tty, KDSKBLAYOUT, "it")`, `loadkeys it` | none (Linux uses `KDSKBENT`) | none |
| 6-argument mmap | `secos_mmap(addr, len, prot, flags, fd, off)` | `mmap` | |

No new syscall numbers were needed: everything is an ioctl or a device node.
The `mmap` syscall (14) now also reads a 6th argument (the file offset, in
`r9` like Linux). Only device mappings use it.

## Capabilities

Reading input devices lets a process snoop every keystroke, and the
framebuffer shows (and can fake) everything on screen. So these two
capabilities are **explicit privileges**, like `CAP_NET`: a program only gets
them when its signed manifest declares them, even if it is not confined
(`CAP_ENFORCE`). Ambient programs such as bash and BusyBox do not have them.

| Bit | Manifest flag | Kernel `cap_t` | Grants |
|---|---|---|---|
| 13 (`0x2000`) | `MANIFEST_FLAG_CAP_INPUT` | `CAP_INPUT` | `open()` of `/dev/input/eventN` |
| 14 (`0x4000`) | `MANIFEST_FLAG_CAP_DISPLAY` | `CAP_DISPLAY` | `open()` of `/dev/fb0`; `KDSETMODE(KD_GRAPHICS)`; `KD_TEXT` while someone else owns graphics mode |

- The check happens at `open()` (`fs/devfs.c` `devfs_open_hook`) and returns
  `-EACCES`. `KDSETMODE` returns `-EPERM`. Every denial is recorded in the
  `[AUDIT]` ring (shell `audit`, debugcon).
- Kernel context (pid 0, the kernel shell) is always allowed.
- To sign a program with both capabilities, link it with `user/note_gui.S`
  (`flags = 0x6000`) instead of `user/note.S`. See the `fbdemo`/`guitest`
  rules in the Makefile.
- Opened fds are inherited across `fork`/`spawn`/`execve` like any fd (an open
  file description). A compositor that wants to stay the only input reader
  should also hold `EVIOCGRAB` (below) and not hand the fds to its clients.

## /dev/fb0 (fbdev)

The boot framebuffer (GRUB/Multiboot2 VBE or UEFI GOP): linear, 32 bpp,
XRGB8888 (`red.offset = 16`, `green.offset = 8`, `blue.offset = 0`, no alpha).
There is no mode setting. Header: `<linux/fb.h>`.

| Request | Behaviour |
|---|---|
| `FBIOGET_VSCREENINFO` (0x4600) | `struct fb_var_screeninfo` (160 bytes): `xres = xres_virtual`, `yres = yres_virtual`, `bits_per_pixel = 32`, the RGB bitfields, `height = width = -1` (unknown) |
| `FBIOGET_FSCREENINFO` (0x4602) | `struct fb_fix_screeninfo` (80 bytes): `id = "SecOS FB"`, `smem_len` = `line_length * yres` rounded up to a page, `line_length` = pitch, `type = FB_TYPE_PACKED_PIXELS`, `visual = FB_VISUAL_TRUECOLOR`. `smem_start = 0`: the physical address is not exposed. |
| `FBIOPUT_VSCREENINFO` (0x4601) | Accepts exactly the current mode and writes it back. Any other resolution, depth, virtual size or offset returns `-EINVAL`. |
| `FBIOPAN_DISPLAY` (0x4606) | Only offset 0,0 (`-EINVAL` otherwise) |
| `FBIOBLANK` (0x4611) | Accepted, no-op |
| anything else | `-ENOTTY` |

- `read()`/`write()`/`lseek()` copy at a byte offset into the visible
  framebuffer. A write past the end returns `-ENOSPC`.
- `mmap`: use `secos_mmap(NULL, smem_len, PROT_READ|PROT_WRITE, MAP_SHARED,
  fd, 0)` from `libsecos.h`. It returns `MAP_FAILED` and sets errno on error.
  - `MAP_SHARED` is required (`MAP_PRIVATE`/anonymous: `-EINVAL`).
  - `PROT_EXEC` is refused (`-EACCES`), and so is `mprotect(PROT_EXEC)` on the
    mapping later. `PROT_READ` alone gives a read-only view.
  - `off` must be page aligned and `[off, off + len)` must lie inside
    `smem_len`. Anything beyond it is refused with `-EINVAL`.
  - The pages are the framebuffer's physical pages: user, NX, Write-Combining
    (PAT slot 1), tagged `VMM_FLAG_DEVICE`. They are not PMM memory, so
    `munmap`, exit, `execve` and fork teardown just drop the PTEs. A `fork`
    child shares the mapping (no COW). `munmap` of part of it works and keeps
    the rest mapped.
  - The mapping counts against the signed `max_mem` like any other mapping.

The old 4/5-argument `mmap()`/`mmap_file()` in `libsecos.h` are unchanged.
The BusyBox compat `mmap()` turns `MAP_SHARED` into `MAP_PRIVATE`, so it
cannot map the framebuffer. Use `secos_mmap`.

## /dev/input/eventN (evdev)

One node per device, numbered in registration order:

| Typical node | Device | Reports |
|---|---|---|
| `event0` | `AT Translated Set 2 keyboard` (PS/2, bus `BUS_I8042`) | `EV_KEY` key codes 1..127, `EV_REP` |
| `event1` | `ImExPS/2 Generic Explorer Mouse` (PS/2 AUX, IRQ12) | `REL_X`, `REL_Y`, `REL_WHEEL`, `BTN_LEFT/RIGHT/MIDDLE` |
| `event2..` | USB HID (xHCI, `BUS_USB`): keyboard (boot protocol), mouse, tablet | keyboard as PS/2; mouse `REL_*`; tablet `ABS_X/ABS_Y` (0..32767 on QEMU `usb-tablet`) + buttons + wheel |

Don't hard-code the numbers. Probe with `EVIOCGBIT`/`EVIOCGNAME`, the way
libinput and LVGL do. Header: `<linux/input.h>`.

- `struct input_event` is the Linux x86-64 layout: `struct timeval` (two
  `long`s), `__u16 type`, `__u16 code`, `__s32 value`, 24 bytes in all.
  Timestamps are `CLOCK_REALTIME` by default, or `CLOCK_MONOTONIC` after
  `EVIOCSCLOCKID`.
- Events come in packets terminated by `EV_SYN/SYN_REPORT`.
- Sign conventions are Linux's: `REL_Y` grows downward, `REL_WHEEL > 0` means
  the wheel moved away from the user.
- Keys report value 1 on press, 0 on release and 2 on autorepeat. Autorepeat
  is a press of a key that is already down: PS/2 typematic does this, USB
  boot keyboards do not.
- Absolute axes report changes only.
- `read()` returns whole events only. A buffer smaller than 24 bytes gets
  `-EINVAL`. Without `O_NONBLOCK` the read blocks until an event arrives;
  with it, an empty queue returns `-EAGAIN`.
- `poll()`/`select()` report `POLLIN | POLLRDNORM` when events are queued.
- Each `open()` gets its own 256-event ring. If a reader falls behind and the
  ring fills up, the kernel drops the queued events and leaves one
  `EV_SYN/SYN_DROPPED`. The reader should discard until the next
  `SYN_REPORT` and resync state (for example with `EVIOCGKEY`), as on Linux.
- `mmap` of an event node returns `-ENODEV`.

Supported ioctls:

| Request | Result |
|---|---|
| `EVIOCGVERSION` | `0x010001` |
| `EVIOCGID` | `struct input_id` (bustype/vendor/product/version) |
| `EVIOCGNAME(len)`, `EVIOCGPHYS(len)`, `EVIOCGUNIQ(len)` | NUL-terminated string. Returns the bytes copied. |
| `EVIOCGPROP(len)` | `INPUT_PROP_POINTER` (mice) / `INPUT_PROP_DIRECT` (tablets) |
| `EVIOCGBIT(0, len)` | supported event types |
| `EVIOCGBIT(EV_KEY / EV_REL / EV_ABS, len)` | code bitmaps. Other types return zeros. |
| `EVIOCGKEY(len)` | keys and buttons currently down |
| `EVIOCGLED`, `EVIOCGSND`, `EVIOCGSW` | zeros |
| `EVIOCGABS(ABS_X / ABS_Y)` | `struct input_absinfo` (value, min, max). `-EINVAL` for an axis the device lacks. |
| `EVIOCGREP` / `EVIOCSREP` | 250 ms / 33 ms; a set is accepted and ignored |
| `EVIOCGRAB` (arg = int 1/0) | Exclusive delivery to this open. A second grab returns `-EBUSY`. A grabbed keyboard no longer feeds the console tty. The grab is released on close or exit. |
| `EVIOCSCLOCKID` | `CLOCK_REALTIME`, `CLOCK_MONOTONIC` or `CLOCK_BOOTTIME` |
| `FIONBIO` | toggles `O_NONBLOCK` |

USB HID devices have no interrupt path; the stack is polled. A blocked
`read()`/`poll()` on an event node re-polls the USB devices every ~2 ms. The
console tty polls them too, so a reader is not required. PS/2 keyboard and
mouse events come from IRQ1 and IRQ12. Both are routed through the IOAPIC in
APIC mode, or through the 8259 when booted with `-DSECOS_NO_APIC`.

The console keeps working while event nodes are open: every key goes both to
the evdev clients and through the keymap into the console tty, unless a
client holds `EVIOCGRAB` on that keyboard.

## Console handoff (KDSETMODE)

`<linux/kd.h>`. The ioctl goes to the console tty: fd 0/1/2 when they are
the console, or an open `/dev/console` or `/dev/tty`.

- `ioctl(tty, KDSETMODE, (void*)KD_GRAPHICS)` needs `CAP_DISPLAY` and makes
  the caller the graphics owner.
  - The framebuffer console stops drawing: no text, cursor blink or logo.
  - The vt keeps interpreting output into its cell grid, so nothing is lost.
- `ioctl(tty, KDSETMODE, (void*)KD_TEXT)` repaints the whole text console
  from the grid. While someone else owns graphics mode, only the owner or a
  `CAP_DISPLAY` program may do this; anyone else gets `-EPERM`.
- When the owner exits or is killed (fault, signal), the kernel switches back
  to text mode itself (`[VT] text console restored (graphics owner exited
  pid=N)` on debugcon). A crashed compositor never leaves a dead screen.
- `ioctl(tty, KDGETMODE, &int)` returns the current mode.
- A kernel panic always repaints in text mode: the panic path re-initialises
  the console, which clears graphics mode, after
  `fb_console_panic_detach_dbuf`.
- There is a single owner. A second `KD_GRAPHICS` from another program takes
  ownership over.

## Keyboard layouts

Both keyboard paths turn hardware codes (PS/2 scancode set 1, USB HID usages)
into Linux key codes, then a keymap (`drivers/keymap.c`) maps a key code plus
Shift/AltGr/Caps Lock to a Unicode code point.

- Layouts: `us` (the code default) and `it`.
- AltGr is the right Alt (`E0 38`, HID modifier bit 6). On layouts without
  AltGr (`us`) right Alt is Meta.
- Windows sends AltGr as LeftCtrl+RightAlt (WSLg, QEMU GTK). An AltGr symbol
  therefore never becomes a control character.
- `it` follows the standard Italian layout:

  | Key | Plain | Shift | AltGr | Shift+AltGr |
  |---|---|---|---|---|
  | 102nd key (left of Z) | `<` | `>` | | |
  | `7` | | `/` | | |
  | `.` `,` `-` | | `:` `;` `_` | | |
  | `è` | `è` | `é` | `[` | `{` |
  | `+` | `+` | `*` | `]` | `}` |
  | `ò` | `ò` | `ç` | `@` | |
  | `à` | `à` | `°` | `#` | |
  | `ù` | `ù` | `§` | | |
  | `'` | `'` | `?` | `` ` `` | |
  | `ì` | `ì` | `^` | `~` | |
  | `\` | `\` | `\|` | | |
  | `e`, `5` | | | `€` | |

- Characters above ASCII go to the tty as UTF-8. The framebuffer console has
  glyphs for à è é ì ò ù À È É Ì Ò Ù ç § £ €; ° uses the existing degree
  glyph. The VGA text fallback uses CP437 (capital accented letters fold to
  the base letter there).
- Select a layout:
  - at runtime: `loadkeys it` / `loadkeys us` / `loadkeys` (print), from
    bash (`/bin/loadkeys`) or the kernel shell;
  - programmatically: `ioctl(tty, KDSKBLAYOUT, "it")` and
    `ioctl(tty, KDGKBLAYOUT, char[16])`, on the console tty. An unknown name
    returns `-EINVAL`.
  - at boot: `make iso KEYMAP=it` (`-DSECOS_KEYMAP_DEFAULT="it"`). The boot
    log shows `[KBD] keymap it`.
- evdev clients get raw key codes and do their own layout handling. The
  kernel keymap only affects the console tty.
- USB boot keyboards have no host-side autorepeat yet.

## Demo and test

- `/bin/fbdemo` (`user/fbdemo.c`, signed `CAP_INPUT | CAP_DISPLAY`):
  - takes the screen, paints a pattern, moves an arrow cursor with any mouse
    or the tablet;
  - left button paints, right button clears, the wheel changes colour;
  - grabs the keyboards, quits on Esc or `q`, and gives the text console back.
- `/bin/guitest` (`user/guitest.c`) and the capability-less
  `/bin/guitest_nocap` are the selftest programs, driven by
  `tools/gui_drive.py`.
- Run it yourself:
  `qemu-system-x86_64 -cdrom myos.iso -m 512M -device qemu-xhci -device usb-tablet`,
  then `fbdemo` at the bash prompt. The tablet gives absolute pointing in a
  GTK window.
