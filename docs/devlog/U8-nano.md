# U8 — ncurses + GNU nano (`feat/port-nano`)

Userland plan §3 U8, design §2 D12. GNU nano 8.7.1 runs on SecOS: built from
source against unmodified musl (U1) and ncurses 6.6 (wide characters), signed,
embedded in the kernel image and installed as `/usr/bin/nano` with its syntax
files, a small terminfo database and `/etc/nanorc`. It works over SSH
(Dropbear pty), in a secwm terminal and on the kernel console, with UTF-8
text, syntax colours, search, cut/paste, window resizing and saving.

## Quick start

```
make port-nano            # = tools/port-musl.sh + tools/port-ncurses.sh + tools/port-nano.sh
NANO_SPKG=1 tools/port-nano.sh   # also build/nano-8.7.1.spkg (U6 v2, signed, verified)
tools/nano_test.sh [--iso FILE] [--smp N] [--only ssh,con,gui] [--keep]   # the NANO gate
```

On SecOS: `nano file`. With `SECOS_SYSCALL_TRACE=2` every Linux call a musl
program makes is logged to `/tmp/secos-trace.<pid>` (new in U8, below).

## Sources and how they were verified

| Package | Version | Licence | Source | SHA-256 |
|---|---|---|---|---|
| ncurses | 6.6 (latest stable) | MIT-X11 | `https://ftp.gnu.org/gnu/ncurses/ncurses-6.6.tar.gz` | `355b4cbbed880b0381a04c46617b7656e362585d52e9cf84a67e2009b749ff11` |
| GNU nano | 8.7.1 (latest 8.x) | GPL-3.0-or-later | `https://ftp.gnu.org/gnu/nano/nano-8.7.1.tar.xz` | `76f0dcb248f2e2f1251d4ecd20fd30fb400a360a3a37c6c340e0a52c2d1cdedf` |

Each script pins the tarball's SHA-256 (the build refuses a mismatch) and also
checks upstream's detached GPG signature against the GNU keyring
(`https://ftp.gnu.org/gnu/gnu-keyring.gpg`) when `gpg` is available, matching
the signing key's fingerprint:

- ncurses: `ncurses-6.6.tar.gz.sig`, good signature from Thomas E. Dickey,
  key `19882D92DDA4C400C22C0D56CC2AF4472167BE03` (the key has expired, the
  signature is good). The same tarball downloaded from invisible-island.net
  (Dickey's own site, `ncurses-6.6.tar.gz` + `.asc`) is byte-identical and its
  `.asc` verifies with the same key. invisible-island answers 403 to a plain
  `curl`, so the script fetches from the GNU mirror.
- nano: `nano-8.7.1.tar.xz.sig`, good signature from Benno Schulenberg, key
  `168E6F4297BFD7A79AFD4496514BBE2EB8E1961F`.

A bad signature stops the build; a missing `gpg` only prints a note (the
SHA-256 pin is the gate). Neither tree is committed (`build/ncurses-src`,
`build/nano-src`); the licences are in `docs/licenses/`.

## The build

### ncurses (`tools/port-ncurses.sh`)

1. Builds `tic`/`infocmp` 6.6 for the host (`build/ncurses-host`): the
   fallback entries and the database are compiled with the same version as
   the library.
2. Cross-builds with `tools/secos-cc`: `--enable-widec` (ncursesw), static
   only, no C++/Ada/programs/tests/gpm/dlsym/termcap,
   `--enable-ext-colors --enable-ext-mouse --enable-sigwinch`,
   `--with-terminfo-dirs=/usr/share/terminfo`, and
   `--with-fallbacks=linux,xterm,xterm-256color,vt100,screen,screen-256color,tmux,tmux-256color,dumb`
   (the entries are compiled into the library, so a program finds them even
   without the database: ramfs root, a broken `/usr/share/terminfo`).
3. Installs headers (top level and `ncursesw/`) and `libncursesw.a`,
   `libpanelw.a`, `libmenuw.a`, `libformw.a` into `build/sysroot`, with the
   usual non-`w` and `tinfo` names as symlinks.
4. Compiles 13 entries (`linux xterm xterm-256color xterm-color vt100 vt102
   vt220 screen screen-256color tmux tmux-256color ansi dumb`, `tic -x`) into
   `kernel/terminfo_db.h`.

`tools/port-musl.sh` recreates the sysroot, so run port-ncurses after it
(`make port-nano` does all three in order).

### nano (`tools/port-nano.sh`)

`--disable-nls --disable-libmagic --disable-speller --enable-utf8`, ncursesw
from the sysroot. gnulib's run-time probes that cannot run in a cross build are
answered the way musl behaves (mktime, getcwd, fnmatch, strerror, memchr,
malloc/realloc(0)). The result is a 1 MB signed static-PIE, embedded as
`crypto/user_nano_elf.h`; 22 syntax files (sh c makefile python lua json xml
html css javascript markdown patch nanorc default man awk perl ruby rust go
cmake yaml) and the default `/etc/nanorc` (`user/nano/nanorc`) go into
`kernel/nano_syntax.h`. The build is reproducible (rebuilding gives the same
bytes).

`<linux/vt.h>`: musl's `<sys/vt.h>` includes the kernel UAPI header, which the
sysroot does not carry; nano only uses `VT_GETSTATE` (SecOS answers ENOTTY =
"not on a Linux VT"). The script copies the build host's UAPI header into a
private include directory for the nano build only.

**Regex: gnulib's, not musl's.** Forcing gnulib to use the C library's regex
(`--without-included-regex`) is wrong on musl: musl has no GNU `re_*` API, so
gnulib still puts its own `<regex.h>` first on the include path, whose
`regoff_t` is an `int`, while musl's `regexec` writes `long` offsets. nano's
8-byte `regmatch_t` on the stack received 16 bytes: a stack-protector abort in
`check_the_multis` on the first keystroke. nano now uses gnulib's bundled
regex (what Alpine does too): header and implementation agree.

### Delivery (`kernel/sysroot.c`)

nano is embedded in the kernel image like the other system programs (U6
packages are the route for software in general; `NANO_SPKG=1` produces the
same files as `build/nano-8.7.1.spkg`). The table installs:

| Path | What |
|---|---|
| `/usr/bin/nano` | the program, 0755 |
| `/usr/share/nano/*.nanorc` | 22 syntax files, 0644 |
| `/usr/share/terminfo/<c>/<name>` | 13 compiled entries, 0644 |
| `/etc/nanorc` | created when missing, then the administrator's: `constantshow`, `autoindent`, `tabsize 4`, no lock files, `include "/usr/share/nano/*.nanorc"` |

Data files are system files like programs: rewritten when the kernel carries a
different version, removed when no longer shipped, recorded in
`/var/lib/secos/manifest` (helper `sf_data`). `/usr/bin` and `/usr/share` are
created at boot, and every missing parent of a table entry is created
root:root 0755 (`ensure_parents`). The blob-hash cache grows to 128 entries.
Boot marker: `[NANO] installed GNU nano to /usr/bin/nano (+ syntax files,
terminfo)`. `-DSECOS_NO_NANO` leaves it all out. A persistent root gets nano
on the first boot of a kernel that carries it (an upgrade boot), like any new
system program.

### Terminals and locale

- secwm terminals set `TERM=xterm`; Dropbear passes the client's `TERM` from
  the pty request (typically `xterm-256color`); the console has `TERM=linux`
  (init, and `/etc/bash.bashrc` when unset). All three entries are both in the
  database and compiled in.
- No `LANG` is needed: musl's locale defaults to `C.UTF-8` when `LC_ALL`,
  `LC_CTYPE` and `LANG` are unset, so nano starts in UTF-8 mode. (`LANG=C`
  would select byte mode.) D12 suggested `LANG=C.UTF-8` in `/etc/profile`;
  that file is the user's once created, so an upgraded system would not get
  it, and it is not needed.

## Gaps found and fixed

| Gap | Symptom | Fix | Commit |
|---|---|---|---|
| VMA slots (64 per process) exhausted by musl's allocator: every anonymous `mmap` took a slot, and mallocng maps a group per size class | nano with the syntax files: "Bad regex …: Memory exhausted", "Nano is out of memory!"; with musl's regex, a `regfree` crash on the half-built result | `mmap` grows a live anonymous VMA that ends exactly where the new map starts, with the same flags (Linux merges adjacent anonymous maps the same way); the bump allocator makes that the common case | `mm: merge an anonymous mmap into the adjacent anonymous VMA` |
| `munmap` of a hole inside a VMA re-added the tail by hand (ANON only, a FILE tail was dropped) and silently dropped it when the set was full | present pages without a VMA (latent; holes are more common with merged VMAs) | count the holes first, `-ENOMEM` before unmapping anything when slots are short (Linux at its map limit), split every type with `vma_split` | `mm: munmap splits a hole with vma_split, or fails with ENOMEM` |
| ramfs root table full (256 entries) | sysroot `failed=3`; no file could be created in `/tmp` (nano: "Error writing … No such file or directory", also bash `>`) | `RAMFS_MAX_FILES` 512; the kernel shell's ramfs listings no longer keep a 512-pointer array on the stack (static for the flat ones, kmalloc per level for `rftree`) | `ramfs: 512 entries` |
| gnulib `<regex.h>` vs musl regex | stack-protector abort in nano | port script: gnulib's regex (above) | `tools: port ncurses 6.6 and GNU nano 8.7.1 on musl (U8)` |
| no way to see which calls a full-screen program makes | — | `SECOS_SYSCALL_TRACE=2`: one line per call (number, first four arguments, the path for path calls, the result) to `/tmp/secos-trace.<pid>` | `musl: SECOS_SYSCALL_TRACE=2 …` |

Checked and already right (no change needed): raw/cbreak termios via
`TCSETS` on the console tty and on ptys (M39's "raw mode not honoured" is long
gone: `kernel/tty.c` has a full line discipline), `VMIN`/`VTIME` reads
(ncurses polls with VMIN=0), `TIOCGWINSZ`, `SIGWINCH` on a window change from
both Dropbear (SSH window-change → `TIOCSWINSZ`) and secwm (resize grip),
`select`/`poll` on the tty, saving a new file. Not exercised by the gate:
nano's file browser (`^T` in the Write Out prompt, `getdents64` emulation),
backups (`-B`) and saving over a file owned by someone else. With
`SECOS_SYSCALL_TRACE=1` nano reports no ENOSYS; it prints the known
`partial linux syscall 13` (rt_sigaction flags, U1 worklist), which does not
affect it.

## Gate NANO (`tools/nano_test.sh` + `tools/nano_drive.py`, in `tools/selftest.sh`)

Builds `make iso CONSOLE=root` (or `--iso`), then one boot (`-smp 2`, no root
disk) with a user-net e1000 (host port 23000–23999 → guest :22; never
2222/8000), PS/2, `qemu-xhci` + `usb-tablet` and a private QMP socket, in a
private `/tmp`. The same edit three ways — type `L1⏎L2⏎L3` (UTF-8 `à è` in
L1), `^W` search for L2 (from the last line: `Search Wrapped`), `^K` cut it,
Down to the last (magic) line, `^U` paste, `^O` save, `^X` exit — and the
saved bytes are compared with `od` against `L1\nL3\nL2\n`:

1. **ssh**: key installed from the console, `ssh -tt` on a host pty 30x90 with
   `TERM=xterm-256color`; nano draws for 30 rows; the host pty is resized to
   40x100 + SIGWINCH to ssh (window-change) and nano must repaint (title and a
   cursor address on row 39/40); `Wrote 3 lines`; exit; bytes over ssh.
2. **con**: the kernel console (`TERM=linux` checked), serial input; a
   screendump of the framebuffer console shows nano's reverse-video title bar
   across the screen and the typed text (à and è render with the console's
   glyphs); `Wrote 3 lines`; bytes via the console bash.
3. **gui**: `startgui --trace`, the first terminal (`TERM=xterm`); `loadkeys
   it` in it (secwm picks the layout up live) so the à and è keys type the
   accents; screendump: title bar and help bar of an 80x24 nano; the window is
   shrunk with the resize grip to 60x18 while nano runs and the screendump
   must show the help bar on row 17 and a 60-column title (nano repainted for
   the new size); save, exit, `loadkeys us`, `od` in the terminal (secwm's
   `SW-TTY` mirror).

Plus: `[NANO]` boot marker, no ring-3 fault, no panic, no ENOSYS. 31 checks.

## Tests run

| Run | Result |
|---|---|
| `tools/nano_test.sh` (fresh `make clean` + `make iso CONSOLE=root`, `-smp 2`) | 34/34, then 34/34 again on the same image |
| selftest sections run on the CONSOLE=root image: console termios + readline, pseudo-terminals, Dropbear SSH (incl. pty window change), GUI compositor gate | 113/113 |
| `tools/musl_base_test.sh` | 55/55 |
| `tools/persist_test.sh` (CONSOLE=root, as selftest.sh exports it) | 30/30 (boot 1 writes 75 system files) |
| `tools/smoke.sh --mb2` / `--uefi` | PASS / PASS |
| warnings (`make iso CONSOLE=root`, `make CFLAGS_EXTRA=-DSECOS_LOCKDEP`) | 0 / 0 (base 0 / 0) |

## Notes for later steps

- `make port-nano` needs the network on the first run (cached in `build/dl`).
- The persistent-root gate counts 75 written system files on a fresh disk
  (40 programs + 35 data files).
- The ramfs root now has 512 slots; it holds about 260 entries with nano.
- U4a (mmap rework) should keep the adjacent-anonymous merge (or reuse
  addresses); without it, a musl program with many live allocations runs out
  of VMA slots well before memory.
- Other ncurses programs (htop, less, vim, dialog) can link `-lncursesw` from
  the sysroot after `tools/port-ncurses.sh`; the terminfo database and the
  fallbacks are already there.
