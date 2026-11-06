# M40 — GUI kernel interfaces: mouse, evdev, fbdev, console handoff, keymaps

Branch `feat/gui-input` (from `integ/phase2`). This is the kernel half of the
graphical UI. The compositor and the LVGL terminal build on these interfaces
later. The API contract for that work is `docs/GUI_INPUT.md`.

## What landed

### Input core + evdev (`drivers/input.{c,h}`)

- Devices register an `input_dev_t` and become `/dev/input/eventN`.
- Events use Linux `struct input_event` (24 bytes) with `EV_SYN`, `EV_KEY`,
  `EV_REL` and `EV_ABS`. A press of a key that is already down is reported as
  value 2 (autorepeat); absolute axes report changes only, as in Linux.
- Every `open()` gets a client from a static pool. The client's embedded
  inode is substituted into the fd, the same trick `kernel/pty.c` uses.
- Each client has its own 256-event ring. On overflow the queue is dropped
  and one `SYN_DROPPED` is left, as in Linux.
- `read()` returns whole events, blocks or returns `-EAGAIN` with
  `O_NONBLOCK`. `poll()` works.
- ioctls:
  - `EVIOCGVERSION`, `EVIOCGID`, `EVIOCGNAME`, `EVIOCGPHYS`, `EVIOCGUNIQ`,
    `EVIOCGPROP`;
  - `EVIOCGBIT` for all event types, `EVIOCGKEY`, `EVIOCGABS`;
  - `EVIOCGREP`/`EVIOCSREP`, `EVIOCGRAB`, `EVIOCSCLOCKID`, `FIONBIO`.
- A blocked reader re-polls every ~2 ms. USB HID is polled and has no
  interrupt, so the `SYS_READ` "would block" path sleeps on `sleep_until`, and
  poll uses a 2 ms re-check when an event fd is in the set.
- Glue:
  - `ksys_open_mode` calls `devfs_open_hook`;
  - `ksys_read` and `ksys_ioctl` route evdev fds first;
  - `ki_fd_revents` handles poll;
  - fd ref/unref happens at dup, fork, cloexec and exit.

### Capabilities

- Two new explicit manifest bits: `CAP_INPUT` (bit 13) and `CAP_DISPLAY`
  (bit 14). Like `CAP_NET`, a program needs them declared in its signed
  manifest even when it is not confined.
- `cap_check_explicit()` (`kernel/cap.c`) does the check and audits denials.
- `user/note_gui.S` is the manifest with both bits (`0x6000`).
- The checks are at `open()` of `/dev/fb0` and the event nodes (`-EACCES`),
  and at `KDSETMODE(KD_GRAPHICS)` (`-EPERM`). `KD_TEXT` while another program
  owns graphics mode also needs `CAP_DISPLAY`, so an unprivileged console user
  cannot scribble over a compositor.

### PS/2 mouse (`drivers/ps2mouse.c`)

- Init: i8042 AUX enable, defaults, then the IntelliMouse knock (rates
  200/100/80, ID 3 = wheel). It runs polled with IRQs off.
- IRQ12 goes to vector 0x2C (`isr_mouse` in `arch/x86/idt_asm.asm`):
  - APIC mode: routed through the IOAPIC by `irq_enable_aux()`, with ISA
    overrides honoured, exactly like the keyboard's IRQ1;
  - PIC mode: unmasked on the slave 8259 plus the cascade line.
  - `irq_eoi_aux()` sends the matching EOI (LAPIC, or slave + master 8259).
- Events use Linux sign conventions: `REL_Y` grows downward, `REL_WHEEL` is
  `-dz`.
- The keyboard IRQ now checks the i8042 status AUX bit. A mouse byte that
  surfaces there goes to the mouse driver instead of being decoded as a
  scancode.

### USB HID mouse / tablet (`drivers/usb_hid.c`, `drivers/xhci.c`, `drivers/usb.c`)

- `usb_hid.c` was rewritten for up to 4 HID interfaces. Protocol 1 is a boot
  keyboard; any other HID interface is a pointer.
- For pointers, the HID report descriptor is fetched and parsed by a small
  parser:
  - it reads usage pages, usage, usage min/max, logical min/max, report
    size/count/ID and Input items;
  - it extracts buttons, X/Y (relative or absolute), Wheel and AC Pan;
  - each report is decoded field by field.
- QEMU `usb-tablet` therefore comes out as `ABS_X`/`ABS_Y` 0..32767 with
  `INPUT_PROP_DIRECT`.
- Device names come from the iProduct string descriptor ("QEMU USB Tablet").
- `usb.c` records the HID descriptor's report length, the interface
  sub/protocol and iProduct.
- The model is still polled, from the input wait paths, never from an ISR.
- **xHCI event routing fix.** With several interrupt endpoints armed at once,
  the single event ring delivers completions in any order:
  - `xhci_poll_transfer` used to drop events that were not its own, which
    would wedge the other HID device forever;
  - `evt_wait` matched any transfer event, so an MSC/control wait could
    consume a HID completion as its own (a latent bug since M22).
  - Completions for endpoints with an outstanding `xhci_submit` are now parked
    in a 16-entry stash keyed by (slot, DCI), and blocking waits match their
    own (slot, DCI).

### Keymaps (`drivers/keymap.{c,h}`, `drivers/keyboard.c`)

- Both keyboard paths now produce Linux key codes:
  - PS/2: set-1 scancodes plus an E0 table;
  - USB: HID usages via Linux's `hid_keyboard[]` table, with modifier
    changes and releases diffed from the boot report.
- A shared `keyboard_key_event()` applies modifiers, the active keymap, Caps
  Lock, termios ISIG and the byte ring. Before this, the USB path had no ISIG
  handling and its own Caps Lock state.
- Layouts: `us` and `it`, with AltGr = right Alt, the 102nd key (`<` `>`),
  `@ # [ ] { } ~` and `` ` `` on AltGr, and `à è é ì ò ù ç ° § £ €`.
- Windows' LeftCtrl+RightAlt AltGr (QEMU GTK under WSLg) produces the AltGr
  symbol, not a control code.
- Code points above ASCII go to the tty as UTF-8.
- The fb console font gained glyphs for à è é ì ò ù À È É Ì Ò Ù ç § £ €.
  They used to fold to ASCII. The VGA text fallback maps them to CP437.
- Selection:
  - `ioctl(tty, KDSKBLAYOUT/KDGKBLAYOUT)` (0x4BF0/0x4BF1, SecOS-specific);
  - `/bin/loadkeys` (`user/loadkeys.c`) and the kernel-shell `loadkeys`;
  - at build time: `make iso KEYMAP=it`. A stamp file rebuilds `keymap.o`
    when `KEYMAP` changes.

### /dev/fb0 (`drivers/fbdev.c`)

- ioctls: `FBIOGET_VSCREENINFO`/`FBIOGET_FSCREENINFO` with the Linux struct
  layouts (static-asserted 160/80 bytes). `FBIOPUT_VSCREENINFO` accepts only
  the current mode and returns `-EINVAL` for anything else. `FBIOPAN_DISPLAY`
  works at 0,0. `FBIOBLANK` is a no-op.
- `read`/`write` copy at a byte offset.
- `smem_start` is reported as 0, so no physical address leaks to user space.
- mmap goes through a new mm path (below): MAP_SHARED only, `PROT_EXEC`
  refused at mmap and at mprotect, offset and length checked against
  `smem_len`, user RW/RO, NX, Write-Combining (PAT slot 1).
- `SYS_MMAP` now also takes the file offset in `r9`, like Linux; only device
  mappings use it. libc: `secos_mmap()`.

### Device memory in the VMM (`mm/vmm.{c,h}`, `mm/vma.{c,h}`)

- New `VMA_TYPE_PHYS` (with `phys_base`) and a new software PTE bit
  `VMM_FLAG_DEVICE` (bit 11). The framebuffer frames are not PMM-owned, so
  every user-frame walker treats a `DEVICE` leaf specially:
  - `vmm_unmap_in_space` / `vmm_space_destroy` drop the PTE without freeing
    or unref'ing the frame;
  - `vmm_fork_space` copies the PTE verbatim: the mapping stays shared, no
    refcount, no COW;
  - `vmm_protect_in_space` keeps the `DEVICE`/PWT/PCD bits and never adds COW.
- `vma_split` rebases `phys_base`, so a partial munmap keeps the tail.
- W^X and "never USER on kernel pages" still hold: these are user VAs in the
  mmap arena, NX is always set, and the `vmm_map_in_space` gates apply.

### Console handoff (`drivers/fb_console.c`, `drivers/fbdev.c`, `kernel/tty.c`)

- `KDSETMODE(KD_GRAPHICS)` sets `fb_gfx`. Every fb-console drawing path is
  then a no-op: cells, scroll, clear, cursor blink, logo and glow, and flush.
  The vt still writes its cell grid.
- `KD_TEXT` repaints the whole console from the grid (`fb_console_redraw`:
  background, cells, logo, cursor, full flush).
- The owner's exit or death restores text mode automatically.
  `vtcon_proc_exit` is called from `process_release_pipes`, which every exit
  path runs.
- A panic always shows: `fb_console_panic_detach_dbuf` clears `fb_gfx`, and
  `fb_console_init` (`terminal_initialize`) resets it.

## Tests

`tools/gui_drive.py` is a new QMP + serial driver. The selftest gate is
"M40 GUI GATE" at the end of `tools/selftest.sh`, with two QEMU runs.

1. **Main run.** PS/2 keyboard and mouse, plus `qemu-xhci`, `usb-tablet` and
   `usb-mouse`.
   - `guitest fb`:
     - ioctl contents, FBIOPUT accept/refuse, ENOTTY;
     - pixels written through the mapping read back with `read()`, and
       `write()` visible through the mapping;
     - an offset map shows the right pixels;
     - refusals: map larger than fb, offset at end, unaligned offset,
       PROT_EXEC (EACCES), MAP_PRIVATE, a map straddling the end;
     - mprotect(EXEC) refused, mprotect(RO) works;
     - fork: the child's pixel write is visible to the parent;
     - 20 map/unmap cycles plus a fork leave MemFree within page-table noise
       (~170 KB).
   - `guitest_nocap nocap`: fb0, event0 and KD_GRAPHICS are refused, with
     `[AUDIT] DENY` lines.
   - `guitest input`, with values checked exactly:
     - PS/2 mouse `mouse_move 10 5` / `-3 -7`, button 1, wheel +1;
     - USB mouse `12 -6`, right button, wheel -1;
     - tablet `input-send-event` abs (16384, 8192) plus left button,
       `EVIOCGABS` range 0..32767;
     - PS/2 KEY_A press and release.
     - Also checked: short read EINVAL, SYN_REPORT, SYN_DROPPED after a
       200-move flood while not reading, EVIOCGRAB exclusivity (EBUSY) and
       release.
   - `loadkeys it` + `guitest keys` over `sendkey`: `less`, `shift-less`,
     `shift-7`, `shift-dot`, `shift-slash`, AltGr combinations for @ # [ ] { } ` ~,
     `ctrl-alt_r-semicolon` (Windows AltGr) giving `@`, à è é ì ò ù, `"`, £
     and €. The bytes are checked exactly.
   - `guitest gfx`, then `guitest gfx crash`:
     - `screendump` shows the four colour quadrants and the magenta centre;
     - the bottom text rows stay pure client pixels while the client prints
       20 lines (console suppressed);
     - after exit, and after the SIGSEGV kill, the kernel logs the restore
       and a second `screendump` shows the black text console with text on it.
2. **`KEYMAP=it` build plus `usb-kbd`.**
   - The boot log shows `[KBD] keymap it` with no `loadkeys`.
   - The same IT key bytes come through the USB HID keyboard.
   - USB keyboard KEY_A shows up on evdev.

## Known gaps

- USB boot keyboards have no host-side autorepeat (as before M40). PS/2
  typematic repeat works and is reported as value 2.
- The keyboard LEDs (Caps Lock and Num Lock) are not driven.
- Only one graphics owner, and no VT switching (Ctrl+Alt+Fn).
- No double buffering or page flipping on fb0 (`yres_virtual == yres`), and no
  vsync. The compositor must do its own back buffer.
- Only the boot mode (GRUB `gfxmode`, UEFI GOP) is available; there is no mode
  setting.
- The HID parser covers mice and tablets (buttons, X/Y, wheel, AC Pan). It
  does not handle multitouch, digitizer pressure or composite devices with
  several interfaces: only the first interface of a USB device is bound, as
  before.
- USB HID input still depends on someone polling it: a reader of
  `/dev/input/*`, or the console tty when bash waits for input. Polling is
  every ~2 ms while an evdev read or poll is blocked.
- The evdev ring is per open, 256 events; there is no `EVIOCSMASK`.
- `smem_start` is reported as 0 on purpose.
- Manifest bits 13/14 are new. A parallel branch that also adds manifest bits
  must pick others.
