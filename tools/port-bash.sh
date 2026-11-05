#!/usr/bin/env bash
# SecOS - reproducible GNU bash port. Fetches bash 5.2, builds it natively to
# generate its derived sources, then CROSS-compiles every object against SecOS's
# freestanding libc (-nostdinc + user/port-include stubs + user/bash_port.h force
# include + user/bash_compat.c shim), links a signed ring-3 bash.elf and embeds it
# as crypto/user_bash_elf.h. See docs/BASH_PORT.md.  SPDX-License-Identifier: MIT
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BV=bash-5.2
WORK="${BASH_PORT_WORK:-/tmp/$BV}"
URL="https://ftp.gnu.org/gnu/bash/$BV.tar.gz"

GCCINC="$(echo /usr/lib/gcc/x86_64-linux-gnu/*/include | tr ' ' '\n' | head -1)"
DDEFS=(-DCONF_MACHTYPE='"x86_64-secos"' -DCONF_VENDOR='"secos"' -DCONF_OSTYPE='"secos"'
       -DCONF_CPUTYPE='"x86_64"' -DCONF_HOSTTYPE='"x86_64"' -DRELSTATUS='"release"'
       -DLOCALEDIR='"/usr/share/locale"' -DPACKAGE='"bash"' -DDEFAULT_PATH_VALUE='"/bin:/usr/bin"'
       -DSTANDARD_UTILS_PATH='"/bin"' -DSYS_BASHRC='"/etc/bash.bashrc"' -DSYS_BASH_LOGOUT='"/etc/bash.bash_logout"')
BFLAGS=(-ffreestanding -nostdlib -nostdinc -fno-pie -no-pie -mno-red-zone -mcmodel=large -m64 -O2 ${BASH_PORT_WFLAGS:--Wno-discarded-qualifiers -Wno-misleading-indentation -Wno-format-zero-length -Werror=implicit-function-declaration -Werror=int-conversion -Werror=int-to-pointer-cast}
        -DHAVE_CONFIG_H -DSHELL -I"$WORK" -I"$WORK/include" -I"$WORK/builtins" -I"$WORK/lib"
        -I"$ROOT/user/port-include" -I"$ROOT/user" -I"$ROOT/user/include" -isystem "$GCCINC"
        "${DDEFS[@]}" -include "$ROOT/user/bash_port.h")
# Files we don't compile (net redirections off, no locale/random.h/iconv, host tool)
EXCLUDE="random.c netopen.c netconn.c fpurge.c fnxform.c mkbuiltins.c mailstat.c shtty.c locale.c getenv.c strtoimax.c strtoumax.c getcwd.c"

echo "[port-bash] 1/6 fetch + native build (generates derived sources)"
if [ ! -d "$WORK" ]; then
    mkdir -p "$(dirname "$WORK")"; ( cd "$(dirname "$WORK")"; wget -q "$URL"; tar xzf "$BV.tar.gz" )
fi
cd "$WORK"
[ -f config.h ] || ./configure --disable-nls --without-bash-malloc --enable-minimal-config \
    --disable-history --disable-readline --disable-progcomp --disable-net-redirections >/dev/null 2>&1
[ -f bash ] || make >/dev/null 2>&1            # native build = generates y.tab.c, version.h, etc.

echo "[port-bash] 2/6 patch config.h for SecOS (disable wide-char/regex/select/locale/union-wait)"
python3 - "$WORK/config.h" <<'PY'
import sys
f=sys.argv[1]; s=open(f).read()
if 'SecOS port overrides' not in s:
    ov='\n'.join('#undef '+m for m in (
      'HAVE_MBSTATE_T HAVE_MBRTOWC HAVE_MBSRTOWCS HAVE_WCWIDTH HAVE_WCHAR_H HAVE_WCTYPE_H '
      'HAVE_WCSDUP HAVE_WCSCMP HAVE_LANGINFO_H HAVE_NL_LANGINFO HAVE_LANGINFO_CODESET '
      'HAVE_REGCOMP HAVE_REGEX_H HAVE_POSIX_REGEXP HAVE_SELECT HAVE_SYS_SELECT_H HAVE_LOCALE_H '
      'HAVE_PSELECT HAVE_DEV_FD HAVE_DEV_STDIN HAVE_STRUCT_STAT_ST_ATIM '
      'HAVE_STRUCT_STAT_ST_ATIMESPEC HAVE_STRUCT_STAT_ST_ATIM_TV_NSEC HAVE_STRUCT_STAT_ST_MTIM_TV_NSEC '
      'HAVE_STRUCT_DIRENT_D_INO HAVE_STRUCT_DIRENT_D_FILENO HAVE_DLOPEN HAVE_DLFCN_H '
      'HAVE_UNION_WAIT HAVE_TERMIO_H').split())
    s=s.replace('#include "config-bot.h"', '/* SecOS port overrides */\n'+ov+'\n#include "config-bot.h"',1)
    open(f,'w').write(s)
PY

echo "[port-bash] 2b/6 rebuild signames.h for SecOS NSIG=32"
# The native build's signames.h lists the host's 65 Linux signals, but bash's
# trap code indexes signal_names[NSIG + {0,1,2}] for DEBUG/ERR/RETURN with the
# SecOS NSIG (user/include/signal.h). Keep signals 0..31 (same numbering as
# Linux) followed by the three pseudo-signals and the terminator.
python3 - "$WORK/signames.h" <<'PY'
import re, sys
f = sys.argv[1]; s = open(f).read()
if 'SecOS NSIG=32' not in s:
    body = s[s.index('{') + 1 : s.index('};')]
    names = re.findall(r'"([^"]*)"', body)
    keep = names[:32] + ['DEBUG', 'ERR', 'RETURN']
    assert len(names) > 32 and names[-3:] == ['DEBUG', 'ERR', 'RETURN'], names[-3:]
    arr = ''.join('    "%s",\n' % n for n in keep) + '    (char *)0x0\n'
    s = s[: s.index('{') + 1] + '\n' + arr + s[s.index('};'):]
    s = s.replace('/* A translation list', '/* SecOS NSIG=32 */\n/* A translation list', 1)
    open(f, 'w').write(s)
PY

echo "[port-bash] 3/6 regenerate builtin .c from .def"
( cd builtins && for d in *.def; do ./mkbuiltins -D . "$d" >/dev/null 2>&1; done )

echo "[port-bash] 4/6 cross-compile objects against SecOS libc"
OBJ="${BASH_PORT_OBJ:-/tmp/secos-bashobj}"; rm -rf "$OBJ"; mkdir -p "$OBJ"
top=$(sed -n '/-o bash /p' <(make -n bash 2>/dev/null) | grep -oE '[a-z_0-9.]+\.o' | grep -v '^bash.o' | sed 's/\.o$/.c/' | sort -u)
[ -n "$top" ] || top=$(ls *.c | grep -vE 'mksyntax|mksignames|buildversion|bashversion')
for c in $top; do bn=$(basename "$c"); case " $EXCLUDE " in *" $bn "*) continue;; esac
  [ -f "$c" ] && gcc "${BFLAGS[@]}" -c "$c" -o "$OBJ/$bn.o"; done
for d in builtins lib/sh lib/glob lib/tilde; do for o in "$d"/*.o; do c="${o%.o}.c"; [ -f "$c" ] || continue
  bn=$(basename "$c"); case " $EXCLUDE " in *" $bn "*) continue;; esac
  gcc "${BFLAGS[@]}" -I"$WORK/$d" -c "$c" -o "$OBJ/${d//\//_}_$bn.o"; done; done

echo "[port-bash] 5/6 compile SecOS shim + link"
gcc "${BFLAGS[@]}" -c "$ROOT/user/bash_compat.c" -o "$OBJ/zz_compat.o"
gcc -ffreestanding -nostdlib -fno-pie -no-pie -mno-red-zone -mcmodel=large -m64 -c "$ROOT/user/setjmp.S" -o "$OBJ/zz_setjmp.o"
ld --gc-sections -T "$ROOT/user/user.ld" -o "$ROOT/user/bash.elf" \
   "$ROOT/user/crt0.o" "$ROOT/user/note.o" "$OBJ"/*.o \
   "$ROOT/user/libsecos.o" "$ROOT/user/libc.o" "$ROOT/user/libm.o"

echo "[port-bash] 6/6 sign + embed -> crypto/user_bash_elf.h"
python3 "$ROOT/tools/secos-sign" "$ROOT/user/bash.elf" --dev
python3 "$ROOT/tools/elf2h.py" "$ROOT/user/bash.elf" user_bash_elf "$ROOT/crypto/user_bash_elf.h"
echo "[port-bash] DONE: $(ls -la "$ROOT/user/bash.elf" | awk '{print $5}') bytes -> crypto/user_bash_elf.h"
