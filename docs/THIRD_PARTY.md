# Third-party software in SecOS images

SecOS itself is MIT-licensed (see `LICENSE.md`). The default boot image also
embeds signed user-space programs that are built **from unmodified upstream
source** by port scripts. Their licences are not MIT. This file lists each one,
where its source comes from and how to rebuild it, so anyone who receives a SecOS
image can get the corresponding source.

| Component | Version | Licence | Source in this repo? | Installed as |
|-----------|---------|---------|----------------------|--------------|
| GNU bash | 5.2 | GPL-3.0-or-later | no: fetched by `tools/port-bash.sh` | `/bin/bash` |
| BusyBox | 1.36.1 | GPL-2.0-only | no: fetched by `tools/port-busybox.sh` | `/bin/busybox` + applet stubs in `/bin` |
| musl libc (regex, fnmatch, getopt only) | 1.2.5 | MIT | no: fetched by `tools/port-busybox.sh` | linked into `/bin/busybox` |
| Lua | 5.4.7 | MIT | yes: `third_party/lua/` | `/bin/lua` |
| LVGL (with its built-in Montserrat + FontAwesome glyph tables) | 9.2.2 | MIT (fonts: SIL OFL 1.1) | no: fetched by `tools/port-lvgl.sh` | linked into `/bin/sysmon` |
| Terminus Font (8x16 regular + bold) | 4.49.1 | SIL OFL 1.1 | yes: `third_party/terminus-font/` (BDF) | glyph table `user/secwm/font16.h` in `/bin/secwm`, `/bin/sysinfo`, `/bin/swptest`, `/bin/sysmon` |

## Upstream sources and checksums

| Tarball | URL | SHA-256 |
|---------|-----|---------|
| `bash-5.2.tar.gz` | https://ftp.gnu.org/gnu/bash/bash-5.2.tar.gz | `a139c166df7ff4471c5e0733051642ee5556c1cc8a4a78f145583c5c81ab32fb` |
| `busybox-1.36.1.tar.bz2` | https://busybox.net/downloads/busybox-1.36.1.tar.bz2 | `b8cc24c9574d809e7279c3be349795c5d5ceb6fdf19ca709f80cde50e47de314` |
| `musl-1.2.5.tar.gz` | https://musl.libc.org/releases/musl-1.2.5.tar.gz | `a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4` |
| Lua 5.4.7 | https://www.lua.org/ftp/lua-5.4.7.tar.gz | vendored in `third_party/lua/` |
| `lvgl-9.2.2.tar.gz` | https://github.com/lvgl/lvgl/archive/refs/tags/v9.2.2.tar.gz | `129b4e00e06639fa79d7e8a6cab3c1ecce2445b1a246652ccd34f22e7b17ad6f` |
| `terminus-font-4.49.1.tar.gz` | https://sourceforge.net/projects/terminus-font/files/terminus-font-4.49/ | `d961c1b781627bf417f9b340693d64fc219e0113ad3a3af1a3424c7aa373ef79` (two BDF files + `OFL.TXT` vendored) |

The BusyBox checksum matches the `busybox-1.36.1.tar.bz2.sha256` file published
next to the tarball. `tools/port-busybox.sh` checks both the BusyBox and the musl
tarballs against these sums and stops on a mismatch. musl publishes GPG
signatures rather than checksums, so the musl sum above is the one the port was
built with.

## Why the GPL sources stay out of the tree

The GPL programs are never copied into this repository. Their port scripts
download the upstream tarball into a work directory under `/tmp` and build there.
The repository only holds:

- the SecOS-side glue, all MIT: `user/bash_port.h`, `user/bash_compat.c`,
  `user/port-include/`, `user/busybox_port.h`, `user/busybox_compat.c`,
  `user/busybox-include/`, `user/bbstub.c`, the configuration fragment
  `user/busybox.config` and the scripts in `tools/`;
- the **signed binaries** embedded in the kernel image: `crypto/user_bash_elf.h`
  and `crypto/user_busybox_elf.h`, which are byte arrays of the GPL programs.

Every source change SecOS makes to these programs is applied at build time by
the port scripts, and only from files in this repository:
`tools/port-bash.sh` patches bash's generated `config.h`, `signames.h` and
`y.tab.c`; `tools/bb_patch_src.py` patches two BusyBox files, `coreutils/stat.c`
and `coreutils/touch.c`, to read whole-second timestamps.

## Written offer of source code (GPL)

Every SecOS image that contains `/bin/bash` or `/bin/busybox` is distributed
under the terms of the GPL for those two programs. The **complete corresponding
source** is:

1. the upstream tarballs listed above (URLs and SHA-256 sums), plus
2. this repository at the commit the image was built from (shown by the boot
   banner `SECoS build <timestamp> git:<hash>` and in `/VERSION`), which holds
   every SecOS-side file used to configure, patch, compile and link them.

`make port-bash` and `make port-busybox` rebuild both programs from those
sources. If you received a SecOS image without access to these sources, the
SecOS maintainers will provide them on request, for at least three years after
the image was distributed and for no more than the cost of making the copy.

Licence texts: `docs/licenses/busybox-1.36.1-LICENSE` (BusyBox, GPL-2.0, copied
verbatim from the tarball), `docs/licenses/musl-1.2.5-COPYRIGHT` (musl, MIT, its
copyright notice must ship with the musl code linked into BusyBox), `COPYING`
(GPL-3.0) in the bash tarball, and the copyright notice in
`third_party/lua/src/lua.h` for Lua.
`docs/licenses/lvgl-9.2.2-LICENCE.txt` (LVGL, MIT, copied by `tools/port-lvgl.sh`;
LVGL's built-in fonts are converted from Montserrat and FontAwesome Free, both
SIL OFL 1.1), and `third_party/terminus-font/OFL.TXT` (Terminus Font, SIL OFL
1.1, Reserved Font Name "Terminus Font"; `tools/bdf2c.py` converts a glyph
subset to a C table without changing the glyphs, see
`third_party/terminus-font/README.secos`).

LVGL is MIT, so its source does not have to ship with the image. It is fetched
at build time, like the GPL ports, because the upstream tree is 19 MB: the
repository holds only `user/lvgl/lv_conf.h`, the application, the port script
and the signed `crypto/user_sysmon_elf.h`.
