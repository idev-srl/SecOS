#!/usr/bin/env bash
# tools/pkg2_test.sh — U6 PKG2 gate: package installer v2 on a grown root.
#
#   1. a fresh 1 GiB system disk, populated by a first SecOS boot (programs,
#      user files written in the guest), poweroff, e2fsck clean;
#   2. `make grow-root SIZE=8G` on it: e2fsck -fn clean, 8 GiB, the old 1 GiB
#      then filled (debugfs fallocate) so later writes use the new groups;
#   3. boot: df ~8 GiB, the user files intact; from an exFAT transfer disk
#      (`make pkgdisk`, USB storage): a tampered copy and an out-of-tree
#      package install nothing; the ~200 MB package verifies, installs (in the
#      added block groups), `spkg check` passes, its program runs, the data
#      matches the host's; poweroff, e2fsck clean;
#   4. boot: still installed and intact; remove: no file, directory, database
#      or staging entry left, the space comes back;
#   5. kill -9 during install/upgrade (random moments + deterministic points
#      inside the commit): after `spkg recover` exactly one version is
#      installed, every file of it intact, none of the other one;
#   6. QEMU killed (power cut) during an upgrade: the host replays the
#      journal cleanly, the next boot finishes or drops the operation (init
#      runs `spkg recover`) and one whole version is there.
# Output: "  [PASS] PKG2 ..." / "  [FAIL] PKG2 ..." (tools/selftest.sh counts them).
# Private mktemp directory; no network; only QEMU processes started here are
# killed. Usage: tools/pkg2_test.sh [--kills N] [--big-mb M] [--keep]
set -uo pipefail
KILLS=6
BIGMB=200
KEEP=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --kills) shift; KILLS=$1 ;;
        --big-mb) shift; BIGMB=$1 ;;
        --keep) KEEP=1 ;;
        *) echo "pkg2_test: unknown option $1" >&2; exit 2 ;;
    esac
    shift
done
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"
W=$(mktemp -d "${TMPDIR:-/tmp}/secos_pkg2.XXXXXX")
IMG=$W/root.img
PASSN=0; FAILN=0
ok() { if [[ "$2" -eq 0 ]]; then echo "  [PASS] PKG2 $1"; PASSN=$((PASSN+1));
       else echo "  [FAIL] PKG2 $1"; FAILN=$((FAILN+1)); fi; }
QPID=
cleanup() {
    [[ -n "$QPID" ]] && kill -9 "$QPID" 2>/dev/null
    if [[ $KEEP -eq 0 ]]; then rm -rf "$W"; else echo "pkg2_test: artifacts kept in $W"; fi
}
trap cleanup EXIT
export PYENV_VERSION=${PYENV_VERSION:-3.11.8}   # python3 'cryptography' (signing)
PKG="python3 tools/secos-pkg"

# ---- build + packages ----------------------------------------------------------
echo "[pkg2] building the image (CONSOLE=root) and /bin/spkg..."
make clean >/dev/null 2>&1
if ! make iso CONSOLE=root >"$W/build.log" 2>&1 || ! make pkg-progs >>"$W/build.log" 2>&1; then
    echo "pkg2_test: BUILD ERROR (see $W/build.log)"; tail -20 "$W/build.log"; KEEP=1; exit 2
fi
cp myos.iso "$W/os.iso"; cp user/spkg.elf "$W/tool.elf"
make clean >/dev/null 2>&1

echo "[pkg2] making the packages (${BIGMB} MB test package) and the transfer disk..."
S=$W/src; mkdir -p "$S/big" "$S/k1" "$S/k2"
nb=4; per=$(( BIGMB / nb ))
for i in $(seq 0 $((nb-1))); do head -c "${per}M" /dev/urandom >"$S/big/blob$i"; done
echo "bigtest README" >"$S/big/README"; echo "license text" >"$S/big/LICENSE"; echo "level=3" >"$S/big/big.conf"
big_args=()
for i in $(seq 0 $((nb-1))); do big_args+=("$S/big/blob$i:/opt/bigtest/data/blob$i"); done
$PKG create "$W/bigtest.spkg" --name bigtest --version 1.0 "${big_args[@]}" \
    "$S/big/README:/usr/share/bigtest/README" "$S/big/LICENSE:/usr/share/doc/bigtest/LICENSE" \
    "$S/big/big.conf:/etc/bigtest.conf" "$W/tool.elf:/opt/bigtest/bin/bigtool" \
    --symlink /opt/bigtest/bin/bigtool:/usr/bin/bigtool --mkdir /opt/bigtest/empty 2>"$W/pkg.log"
# Tampered: one byte of the payload (middle of blob 1), one byte of the table.
cp "$W/bigtest.spkg" "$W/bad-data.spkg"
off=$(( 256 + 11 * 320 + per * 1048576 + per * 524288 ))
printf '\x5a' | dd of="$W/bad-data.spkg" bs=1 seek=$off conv=notrunc status=none
cp "$W/bigtest.spkg" "$W/bad-table.spkg"
printf 'X' | dd of="$W/bad-table.spkg" bs=1 seek=$(( 256 + 320 + 10 )) conv=notrunc status=none
$PKG create "$W/outside.spkg" --name outside --version 1 "$S/big/README:/root/evil" 2>>"$W/pkg.log"
$PKG create "$W/dotdot.spkg" --name dotdot --version 1 "$S/big/README:/usr/share/x" 2>>"$W/pkg.log"
python3 - "$W/dotdot.spkg" <<'EOF'
# Rewrite the path to /usr/../root/x and RE-SIGN: a signed package the installer must still refuse.
import sys, hashlib
sys.path.insert(0, "tools")
from secos_signlib import DEV_SEED
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
p = sys.argv[1]; b = bytearray(open(p, "rb").read())
new = b"/usr/../root/x"; b[256:256 + 256] = new + b"\0" * (256 - len(new))
b[-64:] = b"\0" * 64
b[-64:] = Ed25519PrivateKey.from_private_bytes(DEV_SEED).sign(hashlib.sha256(b).digest())
open(p, "wb").write(b)
EOF
# The kill-test pair: 24 files tagged with the version, files only in one of
# them, a config file.
for v in 1 2; do
    for i in $(seq -w 0 23); do { printf 'ktest-v%s-%s\n' "$v" "$i"; head -c 512K /dev/urandom; } >"$S/k$v/f$i"; done
    echo "ktest conf v$v" >"$S/k$v/conf"; echo "only in v$v" >"$S/k$v/only"
    args=(); for i in $(seq -w 0 23); do args+=("$S/k$v/f$i:/opt/ktest/f$i"); done
    if [[ $v == 1 ]]; then args+=("$S/k1/only:/opt/ktest/only1"); else args+=("$S/k2/only:/usr/share/ktest/only2"); fi
    $PKG create "$W/ktest$v.spkg" --name ktest --version "$v" "${args[@]}" "$S/k$v/conf:/etc/ktest.conf" 2>>"$W/pkg.log"
done
! $PKG verify "$W/bad-data.spkg" >/dev/null 2>&1 && ! $PKG verify "$W/bad-table.spkg" >/dev/null 2>&1 \
  && $PKG verify "$W/bigtest.spkg" >/dev/null && $PKG verify "$W/dotdot.spkg" >/dev/null
ok "host: packages built, secos-pkg verify accepts the good ones and refuses the tampered ones" $?
# Room for the two tampered copies, added by hand below.
PKGDISK_FREE_MB=$(( 2 * BIGMB + 64 )) make --no-print-directory pkgdisk PKGDISK_IMG="$W/pkg.img" \
    PKGS="$W/bigtest.spkg $W/ktest1.spkg $W/ktest2.spkg $W/outside.spkg $W/dotdot.spkg" >"$W/pkgdisk.log" 2>&1
rc=$?
# The tampered ones go in by hand (make pkgdisk refuses them, by design).
if [[ $rc -eq 0 ]]; then
    bash tools/exfat-host.sh run "$W/pkg.img" "cp $W/bad-data.spkg $W/bad-table.spkg \"\$M/\" && sync" >>"$W/pkgdisk.log" 2>&1; rc=$?
fi
ok "make pkgdisk builds the exFAT transfer disk (rc=$rc)" $rc
make --no-print-directory pkgdisk PKGDISK_IMG="$W/x.img" PKGS="$W/bad-data.spkg" >/dev/null 2>&1
[[ $? -ne 0 && ! -e "$W/x.img" ]]; ok "make pkgdisk refuses a tampered package" $?

# ---- VM helpers -----------------------------------------------------------------
start_vm() { # start_vm <tag>
    rm -f "$W/in"; mkfifo "$W/in"
    : > "$W/$1.log"; : > "$W/$1.ser"
    qemu-system-x86_64 -cdrom "$W/os.iso" -boot d -m 1G -cpu max -smp "${QEMU_SMP:-2}" \
        -drive file="$IMG",if=virtio,format=raw \
        -device qemu-xhci -drive if=none,id=pk,file="$W/pkg.img",format=raw -device usb-storage,drive=pk \
        -debugcon file:"$W/$1.log" -global isa-debugcon.iobase=0xe9 \
        -no-reboot -display none -serial stdio <"$W/in" >"$W/$1.ser" 2>&1 &
    QPID=$!
    exec 3>"$W/in"
    TAG=$1
}
send() { printf '%s\r' "$1" >&3; }
waitfor() { # waitfor <regex> <seconds>
    local f=$W/$TAG.ser n=$(( $2 * 5 ))
    while (( n-- > 0 )); do
        grep -aEq -- "$1" "$f" && return 0
        kill -0 "$QPID" 2>/dev/null || break
        sleep 0.2
    done
    grep -aEq -- "$1" "$f"
}
# run <marker> <seconds> <command>: the command, then "echo <marker>-$?"; returns
# when the marker line appears (its exit status is in the line).
run() { send "$3; echo $1-\$?-END"; waitfor "^$1-[0-9]+-END" "$2"; }
rcof() { grep -a "^$1-[0-9]*-END" "$W/$TAG.ser" | head -1 | sed -E "s/^$1-([0-9]+)-END.*/\\1/"; }
stop_vm() { # wait for QEMU to exit by itself (poweroff), kill after <s>
    local n=$(( $1 * 5 )) rc
    while kill -0 "$QPID" 2>/dev/null && (( n-- > 0 )); do sleep 0.2; done
    if kill -0 "$QPID" 2>/dev/null; then kill -9 "$QPID"; wait "$QPID" 2>/dev/null; rc=124
    else wait "$QPID"; rc=$?; fi
    exec 3>&-; QPID=; return $rc
}
crash_vm() { kill -9 "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null; exec 3>&-; QPID=; }
PROMPT='bash-5\.2# '
fsck_clean() {
    e2fsck -fn "$1" >"$W/fsck.out" 2>&1 || return 1
    ! grep -vE '^e2fsck [0-9]|^Pass [1-5]|^SECOSROOT: [0-9]+/[0-9]+ files|^$' "$W/fsck.out" | grep -q .
}
newfree() { dumpe2fs "$IMG" 2>/dev/null | awk '/^Group [0-9]+:/{g=($2+0 >= 8)} g && /free blocks,/{f+=$1} END{print f+0}'; }
MNT='mkdir -p /media/pkg && mount -t exfat /dev/usb0 /media/pkg'
# One whole version of ktest, intact, nothing of the other one, nothing pending.
KCHK='v=$(spkg list | awk "/^ktest /{print \$2}"); n=0; for f in /opt/ktest/f*; do head -c 9 $f | grep -q "ktest-v$v" && n=$((n+1)); done; o=""; [ -e /opt/ktest/only1 ] && o="${o}1"; [ -e /usr/share/ktest/only2 ] && o="${o}2"; s=$(ls /var/lib/spkg/staging | wc -l); t=0; [ -e /var/lib/spkg/txn ] && t=1; spkg check ktest >/dev/null; echo "KSTATE v=$v n=$n only=$o stage=$s txn=$t check=$?"'
kstate_ok() { # the last KSTATE line in the serial log is a consistent version
    local l; l=$(grep -a "^KSTATE" "$W/$TAG.ser" | tail -1 | tr -d '\r')
    LASTK=$l
    [[ "$l" == "KSTATE v=1 n=24 only=1 stage=0 txn=0 check=0" || "$l" == "KSTATE v=2 n=24 only=2 stage=0 txn=0 check=0" ]]
}

# ---- 1. a 1 GiB root with data ------------------------------------------------------
bash tools/mkroot.sh "$IMG" 1G >/dev/null
echo "[pkg2] boot 1: populate a 1 GiB root..."
start_vm b1
waitfor "$PROMPT" 180; ok "boot 1 reaches the bash prompt" $?
run U1 60 'echo kept-across-grow > /root/notes.txt; mkdir -p /home/alice; dd if=/dev/urandom of=/home/alice/data.bin bs=64k count=64 2>/dev/null; md5sum /home/alice/data.bin /root/notes.txt > /root/sums'
send 'poweroff'; stop_vm 90; rc=$?
ok "boot 1 user data written, poweroff (rc=$rc)" $rc
fsck_clean "$IMG"; ok "boot 1 disk e2fsck clean" $?
debugfs -R "dump /root/sums $W/sums" "$IMG" >/dev/null 2>&1

# ---- 2. grow -----------------------------------------------------------------------------------
echo "[pkg2] make grow-root SIZE=8G..."
make --no-print-directory grow-root ROOT_IMG="$IMG" SIZE=8G BACKUP=0 >"$W/grow.log" 2>&1; rc=$?
ok "make grow-root 1 GiB -> 8 GiB (rc=$rc)" $rc
fsck_clean "$IMG" && [[ "$(dumpe2fs -h "$IMG" 2>/dev/null | sed -n 's/^Block count: *//p')" == 2097152 ]]
ok "grown disk e2fsck -fn clean, 2097152 blocks (8 GiB)" $?
make --no-print-directory grow-root ROOT_IMG="$IMG" SIZE=4G BACKUP=0 >/dev/null 2>&1
[[ $? -ne 0 && $(stat -c %s "$IMG") -eq 8589934592 ]]; ok "grow-root refuses to shrink" $?
oldfree() { dumpe2fs "$IMG" 2>/dev/null | awk '/^Group [0-9]+:/{g=($2+0 < 8)} g && /free blocks,/{f+=$1} END{print f+0}'; }
printf 'write /dev/null /var/filler\nfallocate /var/filler 0 %d\n' $(( $(oldfree) - 600 )) >"$W/fill.cmds"
debugfs -w -f "$W/fill.cmds" "$IMG" >/dev/null 2>&1
fsck_clean "$IMG" && (( $(oldfree) < 2000 )); ok "old 1 GiB filled with an unwritten extent ($(oldfree) blocks left there), still clean" $?
NF0=$(newfree)

# ---- 3. install from the transfer disk ----------------------------------------------------
echo "[pkg2] boot 2: tampered/refused packages, the ${BIGMB} MB install..."
start_vm b2
waitfor "$PROMPT" 180; ok "boot 2 reaches the bash prompt on the grown disk" $?
run D1 30 "df -k / | tail -n1 | awk '{print \"DF \" \$2}'; md5sum -c /root/sums"
dfk=$(grep -a "^DF " "$W/b2.ser" | head -1 | awk '{print $2}' | tr -d '\r')
[[ -n "$dfk" ]] && (( dfk > 8000000 && dfk <= 8388608 )); ok "df shows about 8 GiB (${dfk:-?} KiB)" $?
[[ "$(rcof D1)" == 0 ]]; ok "the data written before the grow is intact (md5sum -c)" $?
run M1 30 "$MNT"
[[ "$(rcof M1)" == 0 ]]; ok "transfer disk mounts (exFAT on usb0)" $?
EMPTY='[ -z "$(ls /var/lib/spkg/db 2>/dev/null)" ] && [ -z "$(ls /var/lib/spkg/staging 2>/dev/null)" ] && [ ! -e /opt/bigtest ] && [ ! -e /etc/bigtest.conf ] && [ ! -e /usr/share/bigtest ] && [ ! -e /root/evil ] && [ ! -e /root/x ]'
run T1 300 "spkg install /media/pkg/bad-data.spkg"
run T2 30 "$EMPTY"
grep -aq "SIGNATURE INVALID - nothing installed" "$W/b2.ser" && [[ "$(rcof T1)" == 1 && "$(rcof T2)" == 0 ]]
ok "tampered payload byte: refused, nothing installed" $?
run T3 300 "spkg install /media/pkg/bad-table.spkg"
run T4 30 "$EMPTY"
[[ "$(rcof T3)" == 1 && "$(rcof T4)" == 0 ]]; ok "tampered table byte: refused, nothing installed" $?
run T5 60 "spkg install /media/pkg/outside.spkg"
run T6 60 "spkg install /media/pkg/dotdot.spkg"
run T7 30 "$EMPTY"
[[ "$(rcof T5)" == 1 && "$(rcof T6)" == 1 && "$(rcof T7)" == 0 ]] && grep -aq "refused: path /root/evil" "$W/b2.ser"
ok "signed packages with a path outside /usr /opt /etc (or ..) are refused" $?
run V1 300 "spkg verify /media/pkg/bigtest.spkg"
[[ "$(rcof V1)" == 0 ]] && grep -aq "OK bigtest 1.0" "$W/b2.ser"; ok "spkg verify: the ${BIGMB} MB package is signed by the trusted key" $?
send "t0=\$(date +%s); spkg install /media/pkg/bigtest.spkg; r=\$?; echo INSTALL-TIME \$((\$(date +%s)-t0)); echo I1-\$r-END"
waitfor '^I1-[0-9]+-END' 1800
[[ "$(rcof I1)" == 0 ]]; ok "spkg install of the ${BIGMB} MB package ($(grep -a '^INSTALL-TIME' "$W/b2.ser" | awk '{print $2}' | tr -d '\r') s)" $?
run C1 600 "spkg check bigtest"
[[ "$(rcof C1)" == 0 ]] && grep -aEq "spkg: bigtest 1.0: [0-9]+ ok, 0 bad" "$W/b2.ser"; ok "spkg check: every installed file matches the package" $?
run C2 60 "bigtool list | grep -c '^bigtest '; ls -l /usr/bin/bigtool; stat -c '%a %U' /opt/bigtest/bin/bigtool /etc/bigtest.conf"
grep -aq "^755 root" "$W/b2.ser" && grep -aq "^644 root" "$W/b2.ser" && [[ "$(rcof C2)" == 0 ]]
ok "the packaged program runs via its /usr/bin symlink; modes and owner right" $?
send "md5sum /opt/bigtest/data/blob* | sed 's/^/MD5 /'; echo C3-0-END"; waitfor '^C3-0-END' 300
good=1
for i in $(seq 0 $((nb-1))); do
    grep -aq "^MD5 $(md5sum <"$S/big/blob$i" | cut -c1-32)  /opt/bigtest/data/blob$i" "$W/b2.ser" || good=0
done
[[ $good -eq 1 ]]; ok "installed data identical to the host files (md5)" $?
send 'poweroff'; stop_vm 90; rc=$?
fsck_clean "$IMG" && [[ $rc -eq 0 ]]; ok "poweroff, e2fsck clean after the install" $?
NF1=$(newfree)
(( NF0 - NF1 > BIGMB * 256 * 9 / 10 )); ok "the package went to the block groups grow-root added ($(( (NF0 - NF1) / 256 )) MiB there)" $?

# ---- 4. reboot + remove ---------------------------------------------------------------------------
echo "[pkg2] boot 3: remove, kill -9 runs..."
start_vm b3
waitfor "$PROMPT" 180
run R0 60 "spkg list | grep -c '^bigtest *1.0'; df -k / | tail -n1 | awk '{print \"USED0 \" \$3}'"
run R1 300 "spkg check bigtest >/dev/null"
[[ "$(rcof R1)" == 0 ]]; ok "after a reboot the package is still installed and intact" $?
run R2 300 "spkg remove bigtest"
run R3 60 "$EMPTY && [ ! -e /usr/bin/bigtool ] && [ ! -e /usr/share/doc/bigtest ] && [ -z \"\$(find /opt /usr /etc /var/lib/spkg -name '*bigtest*' -o -name '*bigtool*' 2>/dev/null)\" ] && [ ! -e /var/lib/spkg/txn ]"
run R4 60 "spkg list; df -k / | tail -n1 | awk '{print \"USED1 \" \$3}'"
u0=$(grep -a "^USED0 " "$W/b3.ser" | awk '{print $2}' | tr -d '\r'); u1=$(grep -a "^USED1 " "$W/b3.ser" | awk '{print $2}' | tr -d '\r')
[[ "$(rcof R2)" == 0 && "$(rcof R3)" == 0 ]]; ok "spkg remove leaves no file, directory, database or staging entry" $?
[[ -n "$u0" && -n "$u1" ]] && (( u0 - u1 > BIGMB * 1024 * 9 / 10 )); ok "the space comes back ($(( (${u0:-0} - ${u1:-0}) / 1024 )) MiB freed)" $?

# ---- 5. kill -9 during install / upgrade -------------------------------------------------------
run K0 120 "$MNT 2>/dev/null; spkg install /media/pkg/ktest1.spkg >/dev/null; $KCHK"
kstate_ok && [[ "$LASTK" == *"v=1 "* ]]; ok "ktest 1 installed ($LASTK)" $?
send 't0=$(date +%s); spkg install /media/pkg/ktest2.spkg >/dev/null; echo KT-$(($(date +%s)-t0))-END'
waitfor '^KT-[0-9]+-END' 600
T=$(grep -a '^KT-[0-9]*-END' "$W/b3.ser" | head -1 | sed -E 's/^KT-([0-9]+)-END.*/\1/'); T=${T:-10}; (( T < 2 )) && T=2
run K1 60 "$KCHK"
kstate_ok && [[ "$LASTK" == *"v=2 "* ]]; ok "upgrade ktest 1 -> 2 (${T} s; $LASTK)" $?
cur=2; kfail=0; seen1=0; seen2=0
for k in $(seq 1 "$KILLS"); do
    want=$(( 3 - cur ))
    if (( k % 3 == 0 )); then  # deterministic: SIGKILL after N commit actions
        at=$(( (k * 7) % 26 + 1 ))
        send "SPKG_TEST_KILL_AT=$at spkg install /media/pkg/ktest$want.spkg >/dev/null 2>&1; echo KX$k-\$?-END"
        waitfor "^KX$k-[0-9]+-END" 600
        how="SIGKILL after $at commit actions"
    else                       # random moment: verify, staging or commit
        d=$(awk -v s=$RANDOM -v t="$T" 'BEGIN{srand(s); printf "%.1f", 0.3 + rand() * t}')
        us=$(awk -v d="$d" 'BEGIN{printf "%d", d * 1000000}')
        send "spkg install /media/pkg/ktest$want.spkg >/dev/null 2>&1 & p=\$!; usleep $us; kill -9 \$p; wait \$p; echo KX$k-\$?-END"
        waitfor "^KX$k-[0-9]+-END" 600
        how="kill -9 after ${d} s"
    fi
    run KR$k 600 "spkg recover; $KCHK"
    if kstate_ok; then
        v=${LASTK#KSTATE v=}; v=${v%% *}; cur=$v
        [[ $v == 1 ]] && seen1=$((seen1+1)); [[ $v == 2 ]] && seen2=$((seen2+1))
        echo "    kill run $k ($how): version $v whole"
    else
        kfail=1; echo "    kill run $k ($how): INCONSISTENT: $LASTK"
    fi
done
[[ $kfail -eq 0 ]]; ok "kill -9 during install x$KILLS: always exactly the old or the new version (old $seen1, new $seen2)" $?
run K9 60 "spkg list ktest >/dev/null && ls /var/lib/spkg"
send 'poweroff'; stop_vm 90; rc=$?
fsck_clean "$IMG" && [[ $rc -eq 0 ]]; ok "poweroff after the kill runs, e2fsck clean" $?

# ---- 6. power cut during an upgrade --------------------------------------------------------------
echo "[pkg2] power cut during an upgrade..."
start_vm b4
waitfor "$PROMPT" 180
run P0 60 "$MNT; $KCHK"
kstate_ok; v=${LASTK#KSTATE v=}; v=${v%% *}; want=$(( 3 - ${v:-1} ))
d=$(awk -v s=$RANDOM -v t="$T" 'BEGIN{srand(s); printf "%.1f", t * (0.4 + rand() * 0.6)}')
send "spkg install /media/pkg/ktest$want.spkg"
sleep "$d"; crash_vm
cp --sparse=always "$IMG" "$W/copy.img"
e2fsck -fy "$W/copy.img" >"$W/crash.fsck" 2>&1
! grep -q "Fix?\|Clear?\|Salvage?\|Recreate?\|Relocate?" "$W/crash.fsck" && fsck_clean "$W/copy.img"
ok "power cut ${d} s into the upgrade: host e2fsck finds nothing beyond the journal" $?
rm -f "$W/copy.img"
start_vm b5
waitfor "$PROMPT" 240
run P1 600 "$KCHK"
kstate_ok; ok "after the reboot one whole ktest version is installed ($LASTK)" $?
grep -aq "finishing an interrupted package operation" "$W/b5.ser" && echo "    (init ran spkg recover at boot)"
grep -aq "spkg: interrupted operation completed" "$W/b5.ser" && echo "    (the cut upgrade had committed: init completed it)"
run P2 300 "spkg remove ktest; [ ! -e /opt/ktest ] && [ ! -e /usr/share/ktest ] && [ ! -e /etc/ktest.conf ] && [ -z \"\$(ls /var/lib/spkg/db)\" ]"
[[ "$(rcof P2)" == 0 ]]; ok "ktest removed, no leftovers" $?
send 'poweroff'; stop_vm 90; rc=$?
fsck_clean "$IMG" && [[ $rc -eq 0 ]]; ok "final poweroff, e2fsck clean" $?
! grep -aq "\[EXC\]\|PANIC\|\[LOCKDEP\]" "$W"/b*.log; ok "no CPU exception, panic or lockdep report in any boot" $?

echo "[pkg2] RESULT: $PASSN passed, $FAILN failed"
[[ $FAILN -eq 0 ]]
