#!/usr/bin/env bash
# tools/musl_base_test.sh — U1 MUSL-BASE gate: musl programs on SecOS.
#
#   host: build the musl sysroot (tools/port-musl.sh, cached), then with
#         tools/secos-cc: user/musltest/mtest.c (signed static-PIE), an
#         unsigned copy, a tampered copy and an executable-stack build; check
#         the binaries (ELF type, PT_GNU_STACK, no raw `syscall`, signature);
#   guest: an ISO (CONSOLE=root, ROOT=ram) plus an ext2 test disk at /mnt,
#         -smp $MUSL_SMP (default 4); over the serial bash:
#         - `mtest all`: printf/scanf incl. floats, __thread, errno, auxv,
#           setlocale C.UTF-8 + mbstowcs/wcwidth, iconv UTF-8<->Latin-1,
#           qsort/bsearch, malloc stress (mallocng, 100k mixed sizes), setjmp,
#           a signal handler round trip (no SIGALRM), fork/exec/wait,
#           posix_spawn, pipes, stdio + POSIX files, opendir, chdir/getcwd,
#           time, mmap W^X, an 8 MiB stack (each line is a gate check);
#         - the static-PIE base and the TLS block differ between two runs;
#         - 6 concurrent `mtest tls` loops keep their __thread value across
#           thousands of yields (FS base saved/restored on every switch);
#         - SYS_SPAWN of a musl program (kernel-built stack + auxv, libsecos);
#         - SECOS_SYSCALL_TRACE=1 reports the deliberately unsupported call,
#           and stays silent without it;
#         - the unsigned, tampered and executable-stack binaries are refused;
#         - old-libc programs (bash, BusyBox, lua) still run;
#         - no CPU exception, panic or lockdep report on debugcon.
# Options: --lockdep (image built with -DSECOS_LOCKDEP), --iso FILE (skip the
# image build), --smp N, --keep. Output: "  [PASS] MUSL-BASE ..." /
# "  [FAIL] MUSL-BASE ..." and "RESULT:" (tools/selftest.sh counts them).
# QEMU runs with a private /tmp (unshare -rm); no network ports are used; only
# the QEMU started here is killed.
set -uo pipefail
LOCKDEP=0; ISO=""; SMP=${MUSL_SMP:-4}; KEEP=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --lockdep) LOCKDEP=1 ;;
        --iso) shift; ISO=$1 ;;
        --smp) shift; SMP=$1 ;;
        --keep) KEEP=1 ;;
        *) echo "musl_base_test: unknown option $1" >&2; exit 2 ;;
    esac
    shift
done
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"
export PYENV_VERSION=${PYENV_VERSION:-3.11.8}   # python3 'cryptography' (signing)
W=$(mktemp -d "$REPO/build/musl_base.XXXXXX")   # not /tmp: QEMU gets a private one
PASSN=0; FAILN=0
ok() { if [[ "$2" -eq 0 ]]; then echo "  [PASS] MUSL-BASE $1"; PASSN=$((PASSN+1));
       else echo "  [FAIL] MUSL-BASE $1"; FAILN=$((FAILN+1)); fi; }
QPID=
cleanup() {
    [[ -n "$QPID" ]] && kill -9 "$QPID" 2>/dev/null
    if [[ $KEEP -eq 0 ]]; then rm -rf "$W"; else echo "musl_base_test: artifacts kept in $W"; fi
}
trap cleanup EXIT
for t in mke2fs qemu-system-x86_64 objdump readelf unshare; do
    command -v $t >/dev/null || { echo "musl_base_test: $t missing"; echo "RESULT: 0 passed, 1 failed"; exit 2; }
done

# ---- host: sysroot + test binaries ------------------------------------------------
echo "[musl] musl sysroot (tools/port-musl.sh)..."
need=0
[[ -f build/sysroot/lib/libc.a ]] || need=1
for f in $(find user/musl-secos tools/port-musl.sh -type f); do
    [[ $f -nt build/sysroot/lib/libc.a ]] && need=1
done
if [[ $need -eq 1 ]]; then
    bash tools/port-musl.sh >"$W/port.log" 2>&1; rc=$?
else rc=0; fi
ok "musl sysroot built (no raw syscall instruction in libc.a)" $rc
[[ $rc -eq 0 ]] || { tail -20 "$W/port.log"; echo "[musl] RESULT: $PASSN passed, $FAILN failed"; exit 1; }

D=$W/disk; mkdir -p "$D/w"
CC=tools/secos-cc
CF=(-O2 -Wall -Wextra -o)
$CC "${CF[@]}" "$D/mtest" user/musltest/mtest.c -lm -lsecos >"$W/cc.log" 2>&1 \
  && $CC --secos-unsigned "${CF[@]}" "$D/mtest_unsigned" user/musltest/mtest.c -lm -lsecos >>"$W/cc.log" 2>&1 \
  && $CC "${CF[@]}" "$D/mtest_execstack" user/musltest/mtest.c -lm -lsecos -Wl,-z,execstack >>"$W/cc.log" 2>&1
rc=$?
ok "secos-cc builds and signs the test programs" $rc
[[ $rc -eq 0 ]] || { cat "$W/cc.log"; echo "[musl] RESULT: $PASSN passed, $FAILN failed"; exit 1; }
cp "$D/mtest" "$D/mtest_tampered"
# one byte of .text: the signature no longer matches
off=$(readelf -SW "$D/mtest" | awk '$2==".text"{print strtonum("0x"$5) + 64}')
printf '\x90' | dd of="$D/mtest_tampered" bs=1 seek="$off" conv=notrunc status=none

hdr=$(readelf -hlW "$D/mtest")
grep -q "Type: *DYN" <<<"$hdr" && ! grep -q "INTERP" <<<"$hdr" \
  && grep -E "GNU_STACK" <<<"$hdr" | grep -q "0x800000 RW " \
  && readelf -lW "$D/mtest_execstack" | grep GNU_STACK | grep -q "RWE"
ok "static-PIE, no PT_INTERP, PT_GNU_STACK RW 8 MiB (execstack build: RWE)" $?
! objdump -d "$D/mtest" | grep -qwE "syscall|sysenter"
ok "no Linux syscall instruction in the linked program" $?
python3 - "$D/mtest" "$D/mtest_unsigned" "$D/mtest_tampered" <<'EOF'
import sys
sys.path.insert(0, "tools")
from secos_signlib import find_qsig_sig_offset, compute_digest, DEV_SEED
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
pub = Ed25519PrivateKey.from_private_bytes(DEV_SEED).public_key()
def valid(p):
    d = open(p, "rb").read(); o = find_qsig_sig_offset(d)
    if o is None: return False
    try: pub.verify(bytes(d[o:o + 64]), compute_digest(d, o)); return True
    except Exception: return False
sys.exit(0 if valid(sys.argv[1]) and not valid(sys.argv[2]) and not valid(sys.argv[3]) else 1)
EOF
ok "host check: mtest signature valid, unsigned/tampered copies invalid" $?
mke2fs -q -t ext2 -d "$D" "$W/t.img" 32M >/dev/null 2>&1
ok "ext2 test disk" $?

# ---- the image ----------------------------------------------------------------------
if [[ -z "$ISO" ]]; then
    echo "[musl] building the image (CONSOLE=root$([[ $LOCKDEP -eq 1 ]] && echo ', -DSECOS_LOCKDEP'))..."
    make clean >/dev/null 2>&1
    extra=(); [[ $LOCKDEP -eq 1 ]] && extra=(CFLAGS_EXTRA=-DSECOS_LOCKDEP)
    if ! make iso CONSOLE=root "${extra[@]}" >"$W/build.log" 2>&1; then
        echo "musl_base_test: BUILD ERROR (see $W/build.log)"; tail -20 "$W/build.log"; KEEP=1
        echo "[musl] RESULT: $PASSN passed, $((FAILN+1)) failed"; exit 1
    fi
    cp myos.iso "$W/os.iso"; ISO=$W/os.iso
    make clean >/dev/null 2>&1
fi

# ---- guest ------------------------------------------------------------------------------
rm -f "$W/in"; mkfifo "$W/in"; : >"$W/ser"; : >"$W/dbg"
unshare -rm bash -c 'mount -t tmpfs tmpfs /tmp && exec qemu-system-x86_64 -cdrom "$1" -boot d -m 1G -cpu max \
    -smp "$2" -drive file="$3",if=virtio,format=raw \
    -debugcon file:"$4" -global isa-debugcon.iobase=0xe9 -no-reboot -display none -nic none \
    -serial stdio' _ "$ISO" "$SMP" "$W/t.img" "$W/dbg" <"$W/in" >"$W/ser" 2>&1 &
QPID=$!
exec 3>"$W/in"
send() { printf '%s\r' "$1" >&3; }
waitfor() { # waitfor <regex> <seconds>
    local n=$(( $2 * 5 ))
    while (( n-- > 0 )); do
        grep -aEq -- "$1" "$W/ser" && return 0
        kill -0 "$QPID" 2>/dev/null || break
        sleep 0.2
    done
    grep -aEq -- "$1" "$W/ser"
}
# run <tag> <seconds> <command>: the command, then "echo <tag>-RC-$?"
run() { send "$3; echo $1-RC-\$?"; waitfor "^$1-RC-[0-9]+" "$2"; }
rcof() { grep -a "^$1-RC-" "$W/ser" | tail -1 | sed -E "s/^$1-RC-([0-9]+).*/\1/"; }

echo "[musl] booting (-smp $SMP)..."
waitfor 'bash-[0-9.]+# ' 240; ok "boot to the root console" $?
run MNT 30 "grep -q ' /mnt ' /proc/mounts"
[[ "$(rcof MNT)" == 0 ]]; ok "test disk mounted at /mnt" $?

echo "[musl] mtest all..."
run ALL 300 "/mnt/mtest all /mnt/w"
while IFS= read -r l; do
    case "$l" in
        "MT PASS "*) ok "${l#MT PASS }" 0 ;;
        "MT FAIL "*) ok "${l#MT FAIL }" 1 ;;
    esac
done < <(grep -a "^MT PASS \|^MT FAIL " "$W/ser" | tr -d '\r')
grep -aq "^MT DONE pass=[0-9]* fail=0" "$W/ser" && [[ "$(rcof ALL)" == 0 ]]
ok "mtest all completed with no failure" $?

run A1 30 "/mnt/mtest addr"; run A2 30 "/mnt/mtest addr"
mapfile -t AD < <(grep -a "^MT ADDR " "$W/ser" | tr -d '\r' | tail -2)
a1=(${AD[0]:-}); a2=(${AD[1]:-})
[[ ${#AD[@]} -eq 2 && ${a1[2]} != "${a2[2]}" && ${a1[3]} != "${a2[3]}" ]]
ok "static-PIE base and TLS differ between two runs (${a1[2]:-?} / ${a2[2]:-?})" $?
lo=$(( 0x100000000 )); hi=$(( 0x280000000 ))
in_win=0; for a in "${a1[2]:-0}" "${a2[2]:-0}"; do v=$(( a )); (( v >= lo && v < hi )) || in_win=1; done
ok "PIE loaded inside the user image window [4 GiB, 10 GiB)" $in_win

run TLS 300 "for i in 1 2 3 4 5 6; do /mnt/mtest tls 4000 & done; wait"
# (6 writers on one console: lines can run together, so match tokens)
ntls=$(grep -ao "MT TLS pid=[0-9]* bad=[0-9]*" "$W/ser" | grep -c " bad=0$")
nbad=$(grep -ao "MT TLS pid=[0-9]* bad=[0-9]*" "$W/ser" | grep -vc " bad=0$")
[[ $ntls -eq 6 && $nbad -eq 0 ]]
ok "6 concurrent processes keep their __thread value over 4000 yields each ($ntls ok)" $?

run SP 60 "/mnt/mtest spawn"
[[ "$(rcof SP)" == 0 ]]; ok "SYS_SPAWN of a musl program (kernel-built auxv, libsecos)" $?

run EN 30 "SECOS_SYSCALL_TRACE=1 /mnt/mtest enosys"
[[ "$(rcof EN)" == 0 ]] && grep -aq "^secos-musl: ENOSYS linux syscall 294" "$W/ser"
ok "unsupported call: -ENOSYS + SECOS_SYSCALL_TRACE line" $?
before=$(grep -ac "^secos-musl: ENOSYS" "$W/ser")
run EQ 30 "/mnt/mtest enosys"
[[ "$(rcof EQ)" == 0 && $(grep -ac "^secos-musl: ENOSYS" "$W/ser") -eq $before ]]
ok "no trace line without SECOS_SYSCALL_TRACE" $?

run US 30 "/mnt/mtest_unsigned addr"
[[ "$(rcof US)" != 0 ]]
ok "unsigned musl binary refused (rc $(rcof US))" $?
run TA 30 "/mnt/mtest_tampered addr"
[[ "$(rcof TA)" != 0 ]]; ok "tampered musl binary refused (rc $(rcof TA))" $?
grep -ac "REFUSED unsigned/invalid ELF" "$W/dbg" | grep -qv '^[01]$'
ok "debugcon: both refusals logged by the signature gate" $?
run XS 30 "/mnt/mtest_execstack addr"
[[ "$(rcof XS)" != 0 ]] && grep -aq "executable stack refused" "$W/dbg"
ok "signed binary with an executable PT_GNU_STACK refused by the loader" $?
[[ $(grep -ac "^MT ADDR" "$W/ser") -eq 2 ]]
ok "none of the refused binaries ran" $?

run OLD 60 "ls /bin | grep -qx busybox && lua -e 'print(6*7)' | grep -qx 42 && echo \$(( 6 * 7 )) | grep -qx 42 && seq 1 50 | wc -l | grep -qx 50"
[[ "$(rcof OLD)" == 0 ]]; ok "old-libc programs (BusyBox, lua, bash) still run" $?

send "poweroff"
for i in $(seq 1 60); do kill -0 "$QPID" 2>/dev/null || break; sleep 1; done
kill -9 "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null; QPID=
! grep -aE "\[PANIC\]|KERNEL PANIC|\[LOCKDEP\]|\[SMAP\] violation|\[TLB\] BUG" "$W/dbg" >/dev/null
ok "no kernel exception, panic, SMAP or lockdep report on debugcon" $?
if [[ $LOCKDEP -eq 1 ]]; then
    grep -aq "LDTEST\] all" "$W/dbg"; ok "lockdep image: boot negative controls ran" $?
fi

echo "[musl] RESULT: $PASSN passed, $FAILN failed"
[[ $FAILN -eq 0 ]]
