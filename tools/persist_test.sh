#!/usr/bin/env bash
# tools/persist_test.sh — M41 persistent-root gate (QEMU, MB2/ISO path).
#
# Builds two images and drives a fresh system disk (tools/mkroot.sh) through:
#   1. boot A on a fresh disk: write files in /root and /home, `loadkeys it`,
#      add an SSH key by hand, edit /etc/inittab, fetch the SSH host key
#      (ssh-keyscan), poweroff -> clean shutdown, e2fsck clean;
#   2. boot A again: nothing rewritten, everything persisted (files, keymap
#      applied from /etc/vconsole.conf, keys, /etc edit), same host key;
#   3. boot B (one embedded program changed, one more baked SSH key): exactly
#      that program is rewritten, the baked key is appended, user key and
#      /etc edit survive, same host key;
#   4. N hard kills (kill -9 QEMU) while a shell loop rewrites files: after
#      each, the host's e2fsck replays the journal on a copy and finds nothing
#      else to fix, and SecOS mounts the disk, replays, and shuts down clean;
#   5. a disk with a broken superblock checksum and one with a lost primary
#      superblock: the boot falls back to RAMFS with a warning and the disk is
#      left byte-identical.
# Output: "  [PASS] ..." / "  [FAIL] ..." lines (tools/selftest.sh counts them).
# Everything lives in a private mktemp directory; the host port is per run
# (24000-24999), never 2222. Only QEMU processes started here are killed.
#
# Usage: tools/persist_test.sh [--crash-runs N] [--keep]
set -uo pipefail
CRASH_RUNS=3
KEEP=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --crash-runs) shift; CRASH_RUNS=$1 ;;
        --keep) KEEP=1 ;;
        *) echo "persist_test: unknown option $1" >&2; exit 2 ;;
    esac
    shift
done
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"
W=$(mktemp -d "${TMPDIR:-/tmp}/secos_persist.XXXXXX")
PORT=$((24000 + $$ % 1000))
IMG=$W/root.img
PASSN=0; FAILN=0
ok() { if [[ "$2" -eq 0 ]]; then echo "  [PASS] PERSIST $1"; PASSN=$((PASSN+1));
       else echo "  [FAIL] PERSIST $1"; FAILN=$((FAILN+1)); fi; }
QPID=
cleanup() {
    [[ -n "$QPID" ]] && kill -9 "$QPID" 2>/dev/null
    if [[ $KEEP -eq 0 ]]; then rm -rf "$W"; else echo "persist_test: artifacts kept in $W"; fi
}
trap cleanup EXIT

# ---- builds ------------------------------------------------------------------
for k in k1 k2 k3; do ssh-keygen -q -t ed25519 -N '' -C "persist-$k" -f "$W/$k"; done
build() { # build <out.iso> <probe 1|2> <keys...>
    local out=$1 probe=$2; shift 2
    make clean >/dev/null 2>&1
    make iso CFLAGS_EXTRA="-DSECOS_SYSROOT_TEST=$probe" SSH_AUTHORIZED_KEYS="$*" >"$W/build.log" 2>&1 \
        || { echo "persist_test: BUILD ERROR (see $W/build.log)"; tail -20 "$W/build.log"; KEEP=1; exit 2; }
    cp myos.iso "$out"
}
echo "[persist] building image A (probe=hello, key k1) and B (probe=m16_child, keys k1+k3)..."
build "$W/a.iso" 1 "$W/k1.pub"
build "$W/b.iso" 2 "$W/k1.pub" "$W/k3.pub"
make clean >/dev/null 2>&1

# ---- VM helpers ----------------------------------------------------------------
start_vm() { # start_vm <iso> <tag> [disk]
    local disk=${3:-$IMG}
    rm -f "$W/in"; mkfifo "$W/in"
    : > "$W/$2.log"; : > "$W/$2.ser"
    qemu-system-x86_64 -cdrom "$1" -boot d -m 512M -cpu max \
        -drive file="$disk",if=virtio,format=raw \
        -netdev user,id=n0,hostfwd=tcp:127.0.0.1:$PORT-:22 -device e1000,netdev=n0 \
        -debugcon file:"$W/$2.log" -global isa-debugcon.iobase=0xe9 \
        -no-reboot -display none -serial stdio <"$W/in" >"$W/$2.ser" 2>&1 &
    QPID=$!
    exec 3>"$W/in"
    TAG=$2
}
send() { printf '%s\r' "$1" >&3; }
waitfor() { # waitfor <regex> <seconds> [file]   (grep -E on the serial log)
    local f=${3:-$W/$TAG.ser} n=$(( $2 * 5 ))
    while (( n-- > 0 )); do
        grep -aEq -- "$1" "$f" && return 0
        kill -0 "$QPID" 2>/dev/null || break
        sleep 0.2
    done
    grep -aEq -- "$1" "$f"
}
stop_vm() { # wait for QEMU to exit by itself (poweroff); kill after <s>; prints rc
    local n=$(( $1 * 5 )) rc
    while kill -0 "$QPID" 2>/dev/null && (( n-- > 0 )); do sleep 0.2; done
    if kill -0 "$QPID" 2>/dev/null; then kill -9 "$QPID"; wait "$QPID" 2>/dev/null; rc=124
    else wait "$QPID"; rc=$?; fi
    exec 3>&-; QPID=; return $rc
}
crash_vm() { kill -9 "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null; exec 3>&-; QPID=; }
PROMPT='bash-5\.2# '
hostkey() { # the guest's ed25519 host key as seen by an SSH client
    command -v ssh-keyscan >/dev/null 2>&1 || { echo "no-keyscan"; return; }
    local i k
    for i in $(seq 1 20); do
        k=$(ssh-keyscan -T 10 -t ed25519 -p "$PORT" 127.0.0.1 2>/dev/null | awk '{print $2" "$3}')
        [[ -n "$k" ]] && { echo "$k"; return; }
        sleep 3
    done
    echo "none"
}
# e2fsck -fn exits 0 for some benign differences (a lagging superblock free
# count is "PR_NO_OK"): clean means exit 0 AND no message besides the passes.
fsck_clean() {
    e2fsck -fn "$1" >"$W/fsck.out" 2>&1 || return 1
    ! grep -vE '^e2fsck [0-9]|^Pass [1-5]|^SECOSROOT: [0-9]+/[0-9]+ files|^$' "$W/fsck.out" | grep -q .
}
dumpf() { debugfs -R "dump $1 $2" "$IMG" >/dev/null 2>&1; }

# ---- 1. first boot on a fresh disk -------------------------------------------------
bash tools/mkroot.sh "$IMG" 1G >/dev/null
echo "[persist] boot 1 (image A, fresh disk)..."
start_vm "$W/a.iso" p1
waitfor "$PROMPT" 150; ok "boot 1 reaches the bash prompt" $?
send 'echo persist-data > /root/notes.txt; mkdir -p /home/alice; echo hello-alice > /home/alice/f.txt; echo M1-$((20+1))'
waitfor '^M1-21' 30
send 'loadkeys it; echo M2-$((20+2))'
waitfor '^M2-22' 30
send "echo '$(cat "$W/k2.pub")' >> /root/.ssh/authorized_keys; echo '# user edit 77' >> /etc/inittab; echo M3-\$((20+3))"
waitfor '^M3-23' 30; ok "boot 1 user changes made (files, loadkeys it, key, inittab edit)" $?
waitfor 'started sshd' 60
HK1=$(hostkey)
send 'poweroff'
stop_vm 90; rc=$?
ok "boot 1 poweroff makes QEMU exit by itself (rc=$rc)" $rc
grep -aq "\[ROOT\] persistent root fs=ext4 dev=vda journal=on csum=on" "$W/p1.log"; ok "boot 1 root is the journalled metadata_csum ext4 disk" $?
grep -aEq "\[SYSROOT\] persistent root: written=32 unchanged=0 linked=[1-9][0-9]* .*failed=0" "$W/p1.log"
ok "boot 1 installs the system files (32 programs + symlinks, no failure)" $?
grep -aq "loadkeys: keyboard layout it (saved in /etc/vconsole.conf)" "$W/p1.ser"; ok "boot 1 loadkeys saves the layout" $?
fsck_clean "$IMG"; ok "boot 1 disk e2fsck-clean after poweroff" $?
dumpf /etc/dropbear/dropbear_ed25519_host_key "$W/hk1"
[[ -s "$W/hk1" ]]; ok "boot 1 SSH host key generated on the disk" $?
[[ "$HK1" != none && -n "$HK1" ]]; ok "boot 1 ssh-keyscan gets the host key ($HK1)" $?

# Make it look like a disk from before permission enforcement: every program
# in /bin 0644, /tmp 0755. Give user files unusual modes that must survive.
# (debugfs sif keeps mtime, so the programs still match the manifest.)
debugfs -R "ls -p /bin" "$IMG" 2>/dev/null | awk -F/ '$3 ~ /^100/ {print $6}' >"$W/binfiles"
NBIN=$(wc -l <"$W/binfiles")
{ sed 's|.*|sif "/bin/&" mode 0100644|' "$W/binfiles"
  echo 'sif /tmp mode 040755'
  echo 'sif /root/notes.txt mode 0100604'
  echo 'sif /home/alice mode 040750'; } >"$W/oldmodes.cmds"
debugfs -w -f "$W/oldmodes.cmds" "$IMG" >/dev/null 2>&1
[[ "$NBIN" -ge 25 ]] && debugfs -R "stat /bin/bash" "$IMG" 2>/dev/null | grep -q "Mode:  0644"
ok "old-format disk simulated ($NBIN programs set to 0644, /tmp 0755)" $?

# ---- 2. second boot, same image -------------------------------------------------
echo "[persist] boot 2 (image A again)..."
start_vm "$W/a.iso" p2
waitfor "$PROMPT" 150
send 'cat /root/notes.txt /home/alice/f.txt; loadkeys; grep -c ssh-ed25519 /root/.ssh/authorized_keys; tail -n1 /etc/inittab; echo M4-$((20+4))'
waitfor '^M4-24' 30
send "stat -c 'MODE %a %U:%G %n' /bin/bash /bin/busybox /bin/ls /tmp /root /root/notes.txt /home/alice /etc/shadow; echo M6-\$((20+6))"
waitfor '^M6-26' 30
waitfor 'started sshd' 60
HK2=$(hostkey)
send 'poweroff'
stop_vm 90; rc=$?
grep -aEq "\[SYSROOT\] persistent root: written=0 unchanged=[1-9][0-9]* linked=0 stale_removed=0 configs_created=0 modes_fixed=$((NBIN+1)) failed=0" "$W/p2.log"
ok "boot 2 rewrites nothing, fixes exactly the $NBIN program modes + /tmp" $?
grep -aq "^MODE 755 root:root /bin/bash" "$W/p2.ser" && grep -aq "^MODE 755 root:root /bin/busybox" "$W/p2.ser" && \
  grep -aq "^MODE 1777 root:root /tmp" "$W/p2.ser" && grep -aEq "^MODE 700 root:root /root.?$" "$W/p2.ser" && \
  grep -aq "^MODE 600 root:root /etc/shadow" "$W/p2.ser"
ok "boot 2 system modes standard (programs 0755, /tmp 1777, /root 0700, shadow 0600)" $?
grep -aq "^MODE 604 root:root /root/notes.txt" "$W/p2.ser" && grep -aq "^MODE 750 root:root /home/alice" "$W/p2.ser"
ok "boot 2 user file modes untouched (/root/notes.txt 0604, /home/alice 0750)" $?
grep -aq "^persist-data" "$W/p2.ser" && grep -aq "^hello-alice" "$W/p2.ser"; ok "boot 2 files in /root and /home persisted" $?
grep -aq "\[KEYMAP\] /etc/vconsole.conf: KEYMAP=it (applied)" "$W/p2.log" && grep -aq "^it" "$W/p2.ser"
ok "boot 2 keyboard layout restored from /etc/vconsole.conf" $?
grep -aEq "^2.?$" "$W/p2.ser"; ok "boot 2 authorized_keys keeps the baked + the user key (2 lines)" $?
grep -aq "^# user edit 77" "$W/p2.ser"; ok "boot 2 /etc/inittab edit persisted" $?
dumpf /etc/dropbear/dropbear_ed25519_host_key "$W/hk2"
cmp -s "$W/hk1" "$W/hk2" && [[ "$HK2" == "$HK1" ]]; ok "boot 2 SSH host key unchanged (file and fingerprint)" $?
[[ $rc -eq 0 ]] && fsck_clean "$IMG"; ok "boot 2 clean poweroff, e2fsck clean" $?

# ---- 3. upgraded kernel image ------------------------------------------------------
echo "[persist] boot 3 (image B: one program changed, one more baked key)..."
start_vm "$W/b.iso" p3
waitfor "$PROMPT" 150
send '/bin/sysroot_probe one; grep -c ssh-ed25519 /root/.ssh/authorized_keys; grep -c persist-k2 /root/.ssh/authorized_keys; cat /root/notes.txt; tail -n1 /etc/inittab; loadkeys; echo M5-$((20+5))'
waitfor '^M5-25' 30
waitfor 'started sshd' 60
HK3=$(hostkey)
send 'poweroff'
stop_vm 90; rc=$?
grep -aq "\[SYSROOT\] wrote /bin/sysroot_probe" "$W/p3.log" && \
  grep -aEq "\[SYSROOT\] persistent root: written=1 unchanged=[1-9][0-9]* linked=0 .* modes_fixed=0 failed=0" "$W/p3.log"
ok "boot 3 rewrites exactly the changed program" $?
grep -aq "\[child\] arg: one" "$W/p3.ser"; ok "boot 3 the upgraded program runs" $?
grep -aq "\[SSH\] baked authorized_keys installed to /root/.ssh (added=1)" "$W/p3.log" && grep -aEq "^3.?$" "$W/p3.ser"
ok "boot 3 new baked key appended, user key kept (3 lines)" $?
grep -aq "^persist-data" "$W/p3.ser" && grep -aq "^# user edit 77" "$W/p3.ser" && grep -aq "^it" "$W/p3.ser"
ok "boot 3 files, /etc edit and keymap survive the upgrade" $?
dumpf /etc/dropbear/dropbear_ed25519_host_key "$W/hk3"
cmp -s "$W/hk1" "$W/hk3" && [[ "$HK3" == "$HK1" ]]; ok "boot 3 SSH host key unchanged" $?
[[ $rc -eq 0 ]] && fsck_clean "$IMG"; ok "boot 3 clean poweroff, e2fsck clean" $?

# ---- 4. hard kills mid-write ------------------------------------------------------------
echo "[persist] $CRASH_RUNS hard kills while a loop rewrites files..."
replays=0; hostrep=0; crash_ok=1
STRESS='mkdir -p /root/stress; i=0; while :; do echo $i > /root/stress/f$((i%40)); cp /bin/lua /root/stress/big$((i%3)); rm -f /root/stress/f$(((i+20)%40)); i=$((i+1)); done &'
start_vm "$W/b.iso" c0
for run in $(seq 1 "$CRASH_RUNS"); do
    waitfor "$PROMPT" 150 || { crash_ok=0; break; }
    if (( run > 1 )); then
        grep -aq "\[ROOT\] persistent root fs=ext4" "$W/$TAG.log" || { crash_ok=0; echo "    run $run: disk not mounted after the crash"; }
        grep -aEq "\[M27\] journal recover: txns=0x0*[1-9]" "$W/$TAG.log" && replays=$((replays+1))
    fi
    send "$STRESS"
    sleep $(( 3 + RANDOM % 8 ))
    crash_vm
    cp --sparse=always "$IMG" "$W/copy.img"
    # Ground truth: the host replays the journal (checking its v3 checksums);
    # after that there must be nothing to fix, not even a free-count.
    e2fsck -fy "$W/copy.img" >"$W/crash$run.fsck" 2>&1
    grep -q "recovering journal" "$W/crash$run.fsck" && hostrep=$((hostrep+1))
    if grep -q "Fix?\|Clear?\|Salvage?\|Recreate?\|Relocate?" "$W/crash$run.fsck" || ! fsck_clean "$W/copy.img"; then
        crash_ok=0; echo "    run $run: host e2fsck found damage beyond the journal:"; grep -B1 "?" "$W/crash$run.fsck" | head -6
    fi
    rm -f "$W/copy.img"
    start_vm "$W/b.iso" c$run
done
waitfor "$PROMPT" 150 || crash_ok=0
grep -aq "\[ROOT\] persistent root fs=ext4" "$W/$TAG.log" || crash_ok=0
grep -aEq "\[M27\] journal recover: txns=0x0*[1-9]" "$W/$TAG.log" && replays=$((replays+1))
send 'cat /root/notes.txt; ls /root/stress | wc -l; echo C-$((60+6))'
waitfor '^C-66' 30 && grep -aq "^persist-data" "$W/$TAG.ser" || crash_ok=0
send 'poweroff'
stop_vm 90 || crash_ok=0
fsck_clean "$IMG" || crash_ok=0
echo "    crash runs=$CRASH_RUNS, SecOS journal replays=$replays, host e2fsck saw a journal to replay=$hostrep"
[[ $crash_ok -eq 1 ]]; ok "hard kills: disk always mounts, journal replays, e2fsck clean ($CRASH_RUNS runs, $replays replays)" $?

# ---- 5. unusable disk -> RAMFS fallback, disk untouched ------------------------------
corrupt_boot() { # corrupt_boot <tag> <expected reason>
    local d=$W/$1.img before after
    before=$(md5sum <"$d")
    start_vm "$W/a.iso" "$1" "$d"
    waitfor "$PROMPT" 120
    send 'echo RAMFS-$((6*7)); cat /proc/cmdline'
    waitfor '^RAMFS-42' 30; local r1=$?
    send 'poweroff'
    stop_vm 90
    after=$(md5sum <"$d")
    grep -aq "\[ROOT\] WARNING: system disk vda refused: $2" "$W/$1.log" && \
      grep -aq "\[ROOT\] root=ramfs (fallback" "$W/$1.log" && grep -aq "root=ramfs" "$W/$1.ser" && [[ $r1 -eq 0 ]]
    ok "$1: falls back to RAMFS with a warning ($2)" $?
    [[ "$before" == "$after" ]]; ok "$1: the refused disk is left byte-identical" $?
}
echo "[persist] corrupt disks..."
cp --sparse=always "$IMG" "$W/badcsum.img"
printf 'X' | dd of="$W/badcsum.img" bs=1 seek=$((1024 + 120)) conv=notrunc status=none   # label byte: sb csum mismatch
corrupt_boot badcsum "superblock checksum mismatch"
cp --sparse=always "$IMG" "$W/nosb.img"
dd if=/dev/zero of="$W/nosb.img" bs=1 seek=$((1024 + 56)) count=2 conv=notrunc status=none # primary magic gone
corrupt_boot nosb "primary superblock damaged"

echo "[persist] RESULT: $PASSN passed, $FAILN failed"
[[ $FAILN -eq 0 ]]
