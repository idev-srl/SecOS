#!/usr/bin/env bash
# tools/mu_ports_test.sh — multi-user ports gate (BusyBox, bash, Dropbear).
#
# Boots the current myos.iso (MB2, ramfs root, run `make iso` first) with an
# ext2 data disk at /mnt (SATA) and a user-net NIC forwarding a host port to the
# guest's :22. The disk carries a test account written straight into
# /etc/passwd, /etc/shadow and /etc/group by a setup script (no account tool
# needed): tuser (uid 1000, SHA-512-crypt password generated here with
# `openssl passwd -6`, an SSH key in ~tuser/.ssh), a locked account (same
# password behind '!'), a known root password, and a directory of 1000 files.
# Then it checks, from the console bash (root) and over SSH from the host:
#   BusyBox  ls -l / stat owner names, chown user:group, chmod, the kernel's
#            errno reaching the message (ENOTDIR; EROFS when the kernel has a
#            read-only filesystem), non-root EACCES/EPERM;
#   bash     a glob over 1000 directory entries;
#   Dropbear key login and password login as tuser (uid/gid/groups, HOME,
#            SHELL, cwd), the pty slave owned by tuser group tty mode 0620,
#            /etc/shadow unreadable, wrong/locked passwords refused, root key
#            login works and root password login is refused.
# Output: "  [PASS] ..." / "  [FAIL] ..." lines (tools/selftest.sh counts them).
# Everything lives in a private mktemp directory; the host port is per run
# (26000-26999), never 2222 or 8000. Only the QEMU started here is killed.
#
# Usage: tools/mu_ports_test.sh [--keep]
set -uo pipefail
KEEP=0
[[ "${1:-}" == "--keep" ]] && KEEP=1
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"
W=$(mktemp -d "${TMPDIR:-/tmp}/secos_muports.XXXXXX")
PORT=$((26000 + $$ % 1000))
PASSN=0; FAILN=0
ok() { if [[ "$2" -eq 0 ]]; then echo "  [PASS] MUPORTS $1"; PASSN=$((PASSN+1));
       else echo "  [FAIL] MUPORTS $1"; FAILN=$((FAILN+1)); fi; }
cleanup() { if [[ $KEEP -eq 0 ]]; then rm -rf "$W"; else echo "mu_ports_test: artifacts kept in $W"; fi; }
trap cleanup EXIT

for t in ssh ssh-keygen openssl mkfs.ext2 qemu-system-x86_64; do
    command -v "$t" >/dev/null 2>&1 || { echo "  [SKIP] MUPORTS gate (host has no $t)"; exit 0; }
done
[[ -f myos.iso ]] || { echo "mu_ports_test: no myos.iso (run make iso)"; exit 2; }

# ---- the test account and the data disk -------------------------------------
UPW='Secos-Test-Pw-41'                     # tuser's (and locked's) password
RPW='Root-Test-Pw-42'                      # root's password: must still be refused
UHASH=$(openssl passwd -6 -salt mUportsTuser001 "$UPW")
RHASH=$(openssl passwd -6 -salt mUportsRoot0001 "$RPW")
ssh-keygen -q -t ed25519 -N '' -C mu-tuser -f "$W/ukey"
ssh-keygen -q -t ed25519 -N '' -C mu-root -f "$W/rkey"
D=$W/stage; mkdir -p "$D/many"
for i in $(seq -w 0 999); do : > "$D/many/f$i"; done
printf 'tuser:x:1000:1000:Test User:/home/tuser:/bin/bash\nlocked:x:1001:1001:Locked:/home/locked:/bin/bash\n' > "$D/passwd.add"
printf 'tuser:%s:20000:0:99999:7:::\nlocked:!%s:20000:0:99999:7:::\n' "$UHASH" "$UHASH" > "$D/shadow.add"
printf 'root:%s:20000:0:99999:7:::\n' "$RHASH" > "$D/root.shadow"
printf 'tuser:x:1000:\nlocked:x:1001:\nmutest:x:1500:tuser\n' > "$D/group.add"
cp "$W/ukey.pub" "$D/tuser.keys"; cp "$W/rkey.pub" "$D/root.keys"
cat > "$D/setup.sh" <<'EOF'
# Run as root from the console: add the accounts by editing the files directly.
cat /mnt/passwd.add >> /etc/passwd
cat /mnt/group.add >> /etc/group
grep -v '^root:' /etc/shadow > /tmp/shadow.rest
cat /mnt/root.shadow /tmp/shadow.rest /mnt/shadow.add > /etc/shadow
rm -f /tmp/shadow.rest
mkdir -p /home/tuser/.ssh /home/locked /root/.ssh
cp /mnt/tuser.keys /home/tuser/.ssh/authorized_keys
cp /mnt/root.keys /root/.ssh/authorized_keys
echo private > /home/tuser/secret
chown -R tuser:tuser /home/tuser
chown locked:locked /home/locked
chmod 700 /home/tuser/.ssh; chmod 600 /home/tuser/.ssh/authorized_keys /home/tuser/secret
chmod 755 /home/tuser
echo "SETUP-DONE-$((40+2))"
EOF
cat > "$D/bbcheck.sh" <<'EOF'
# BusyBox as root: owner names, chown user:group, chmod, errno messages.
echo "LSL-BEGIN"; ls -l --color=never /home; ls -ln --color=never /home; echo "LSL-END"
echo "STAT-$(stat -c '%U:%G:%a' /home/tuser/secret)"
echo "STATSH-$(stat -c '%U:%G:%a' /etc/shadow)"
touch /tmp/own; chown tuser:mutest /tmp/own; chmod 640 /tmp/own
echo "CHOWN-$(stat -c '%U:%G:%a:%u:%g' /tmp/own)"
echo "OWNLS-$(ls -l /tmp/own | cut -c1-10,11-40 | tr -s ' ')"
touch /etc/passwd/x 2>&1 | sed 's/^/ENOTDIR1-/'
mkdir /etc/passwd/d 2>&1 | sed 's/^/ENOTDIR2-/'
if grep -qw ntfs /proc/filesystems && mkdir -p /ro && mount -t ntfs /dev/sdb /ro 2>/dev/null; then
    touch /ro/x 2>&1 | sed 's/^/EROFS1-/'
    mkdir /ro/d 2>&1 | sed 's/^/EROFS2-/'
else
    echo "EROFS-NOFS"
fi
cd /mnt/many && set -- * && echo "GLOB-$#" && echo "GLOBLAST-${!#}"
echo "BBLS-$(ls /mnt/many | wc -l)"
echo "FIND-$(find /mnt/many -type f | wc -l)"
cd /
echo "BBCHECK-DONE-$((40+3))"
EOF
dd if=/dev/zero of="$W/disk.img" bs=1M count=24 status=none
mkfs.ext2 -q -N 2048 -L SECOSDATA -d "$D" "$W/disk.img" || { echo "mu_ports_test: mkfs.ext2 failed"; exit 2; }
# Both disks are SATA (the kernel probes AHCI only without virtio-blk): the
# ext2 one on port 0 (sda, mounted at /mnt at boot) and, for the EROFS message,
# an empty NTFS volume on port 1 (sdb), mounted by hand when the kernel has the
# read-only NTFS driver.
DISKS=(-device ahci,id=ahci0 -drive file="$W/disk.img",if=none,id=d0,format=raw -device ide-hd,drive=d0,bus=ahci0.0)
if command -v mkntfs >/dev/null 2>&1; then
    dd if=/dev/zero of="$W/ro.img" bs=1M count=16 status=none
    if mkntfs -F -q -f -L SECOSRO "$W/ro.img" >/dev/null 2>&1; then
        DISKS+=(-drive file="$W/ro.img",if=none,id=d1,format=raw -device ide-hd,drive=d1,bus=ahci0.1)
    fi
fi

# ---- host side ssh helpers ---------------------------------------------------
SSHO=(-p "$PORT" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=20 -o LogLevel=ERROR)
skey() { # skey <out> <key> <user> <command>
    timeout 45 ssh "${SSHO[@]}" -o BatchMode=yes -i "$2" -o IdentitiesOnly=yes "$3"@localhost "$4" </dev/null >"$1" 2>&1
    echo "RC=$?" >>"$1"
}
spass() { # spass <out> <password> <user> <command>
    printf '#!/bin/sh\necho %s\n' "'$2'" > "$W/askpass"; chmod 700 "$W/askpass"
    SSH_ASKPASS="$W/askpass" SSH_ASKPASS_REQUIRE=force DISPLAY=none timeout 45 ssh "${SSHO[@]}" \
        -o PubkeyAuthentication=no -o PreferredAuthentications=password -o NumberOfPasswordPrompts=1 \
        "$3"@localhost "$4" </dev/null >"$1" 2>&1
    echo "RC=$?" >>"$1"
}
UCMD='grep -E "^(Uid|Gid|Groups):" /proc/self/status; echo "HOME=$HOME SHELL=$SHELL USER=$USER UID=$UID EUID=$EUID"; echo "PWD=$(pwd)"; if command -v id >/dev/null; then id; fi; cat /etc/shadow; echo "CAT-RC=$?"; touch /etc/mu-x; chmod 777 /etc/passwd; chown tuser /root; ls -l --color=never /home/tuser/secret; cat /home/tuser/secret; echo "[ -w /etc/passwd ]=$([ -w /etc/passwd ] && echo y || echo n) [ -r secret ]=$([ -r /home/tuser/secret ] && echo y || echo n)"'

LOG=$W/debugcon.log; SER=$W/serial.log
echo "[muports] boot (mb2, ext2 /mnt, hostfwd $PORT->22, ~170s)..."
( sleep 32
  printf '  bash /mnt/setup.sh\r'; sleep 6
  printf '  bash /mnt/bbcheck.sh\r'; sleep 14
  skey "$W/k1" "$W/ukey" tuser "$UCMD"
  timeout 45 python3 tools/ssh_winch.py 8 "${SSHO[@]}" -tt -o BatchMode=yes -i "$W/ukey" -o IdentitiesOnly=yes \
      tuser@localhost 'T=$(tty); echo "TTY=$T"; echo "PTYOWN-$(stat -c %U:%G:%a "$T")"' >"$W/k2" 2>&1; echo "RC=$?" >>"$W/k2"
  timeout 45 python3 tools/ssh_winch.py 8 "${SSHO[@]}" -tt -o BatchMode=yes -i "$W/rkey" -o IdentitiesOnly=yes \
      root@localhost 'T=$(tty); echo "RPTY0-$(stat -c %U:%G:%a "$T")"; chown tuser:tty "$T"; chmod 620 "$T"; echo "RPTY1-$(stat -c %U:%G:%a "$T")"' >"$W/k5" 2>&1; echo "RC=$?" >>"$W/k5"
  spass "$W/p1" "$UPW" tuser 'echo PWLOGIN-$((6*7)); grep "^Uid:" /proc/self/status; echo "HOME=$HOME"'
  spass "$W/p2" "wrong-$UPW" tuser 'echo PWWRONG-RAN'
  spass "$W/p3" "$UPW" locked 'echo PWLOCKED-RAN'
  spass "$W/p4" "$RPW" root 'echo PWROOT-RAN'
  skey "$W/k3" "$W/rkey" root 'grep "^Uid:" /proc/self/status; echo ROOTKEY-$((7*6)); cat /var/log/sshd.log'
  skey "$W/k4" "$W/rkey" tuser 'echo WRONGKEY-RAN'
  printf '  poweroff\r'; sleep 5 ) | \
    timeout 240 qemu-system-x86_64 -cdrom myos.iso -boot d -m 512M \
        "${DISKS[@]}" \
        -netdev user,id=n0,hostfwd=tcp:127.0.0.1:"$PORT"-:22 -device e1000,netdev=n0 \
        -debugcon file:"$LOG" -global isa-debugcon.iobase=0xe9 \
        -no-reboot -display none -serial stdio >"$SER" 2>&1
tr -d '\r' < "$SER" > "$SER.txt"
for f in k1 k2 k3 k4 k5 p1 p2 p3 p4; do { echo "--- $f"; cat "$W/$f" 2>/dev/null; } >>"$W/ssh.txt"; done
S=$SER.txt

# ---- setup + BusyBox (console, root) ----------------------------------------
grep -q "^SETUP-DONE-42" "$S"; ok "test accounts written to /etc/passwd, /etc/shadow, /etc/group" $?
sed -n '/^LSL-BEGIN/,/^LSL-END/p' "$S" | grep -Eq '^drwxr-xr-x +[0-9]+ tuser +tuser .* tuser$'
ok "BusyBox ls -l shows owner and group names from /etc/passwd and /etc/group" $?
sed -n '/^LSL-BEGIN/,/^LSL-END/p' "$S" | grep -Eq '^drwxr-xr-x +[0-9]+ 1000 +1000 .* tuser$'
ok "BusyBox ls -ln shows the numeric ids (1000:1000)" $?
grep -q "^STAT-tuser:tuser:600" "$S" && grep -q "^STATSH-root:root:600" "$S"
ok "BusyBox chown -R user:group, chmod and stat %U:%G:%a" $?
grep -q "^CHOWN-tuser:mutest:640:1000:1500" "$S"; ok "BusyBox chown user:group with a supplementary group, chmod 640" $?
grep -q "^ENOTDIR1-touch: /etc/passwd/x: Not a directory" "$S" && grep -q "^ENOTDIR2-mkdir: can't create directory '/etc/passwd/d': Not a directory" "$S"
ok "BusyBox prints the kernel's errno (ENOTDIR), not a guessed EACCES" $?
if grep -q "^EROFS-NOFS" "$S"; then
    echo "  [SKIP] MUPORTS EROFS message (no read-only filesystem: kernel without NTFS, or no host mkntfs)"
else
    grep -q "^EROFS1-touch: /ro/x: Read-only file system" "$S" && grep -q "^EROFS2-mkdir: can't create directory '/ro/d': Read-only file system" "$S"
    ok "BusyBox prints EROFS as \"Read-only file system\"" $?
fi

# ---- bash glob + BusyBox over 1000 entries ----------------------------------
grep -q "^GLOB-1000" "$S" && grep -q "^GLOBLAST-f999" "$S"; ok "bash glob * expands all 1000 directory entries" $?
grep -q "^BBLS-1000" "$S" && grep -q "^FIND-1000" "$S"; ok "BusyBox ls / find list all 1000 entries" $?
grep -q "^BBCHECK-DONE-43" "$S"; ok "BusyBox/bash console checks ran to completion" $?

# ---- Dropbear: key login as tuser -------------------------------------------
K1=$W/k1
grep -Eq "^Uid:\s+1000\s+1000\s+1000\s+1000" "$K1" && grep -Eq "^Gid:\s+1000\s+1000\s+1000\s+1000" "$K1"
ok "SSH key login as tuser: session runs as uid 1000 / gid 1000 (real, effective, saved)" $?
grep -Eq "^Groups:\s+1000 1500\s*$|^Groups:\s+1500 1000\s*$|^Groups:\s+1500\s*$" "$K1"
ok "SSH key login: supplementary groups from /etc/group (initgroups)" $?
grep -q "^HOME=/home/tuser SHELL=/bin/bash USER=tuser UID=1000 EUID=1000" "$K1" && grep -q "^PWD=/home/tuser" "$K1"
ok "SSH key login: HOME, SHELL, USER and cwd from /etc/passwd" $?
grep -q "^CAT-RC=1" "$K1" && grep -q "cat: can't open '/etc/shadow': Permission denied" "$K1" && ! grep -q '^root:\$6\$' "$K1"
ok "SSH tuser cannot read /etc/shadow (Permission denied)" $?
grep -q "touch: /etc/mu-x: Permission denied" "$K1" && grep -q "chmod: /etc/passwd: Operation not permitted" "$K1" && grep -q "chown: /root: Operation not permitted" "$K1"
ok "SSH tuser: BusyBox touch/chmod/chown report EACCES/EPERM" $?
grep -Eq "^-rw------- +1 tuser +tuser .*secret$" "$K1" && grep -q "^private$" "$K1" && grep -q "\[ -w /etc/passwd \]=n \[ -r secret \]=y" "$K1"
ok "SSH tuser reads its own 0600 file; bash test -r/-w use the real permissions" $?
if grep -q "^uid=" "$K1"; then
    grep -q "^uid=1000(tuser) gid=1000(tuser)" "$K1"; ok "SSH id shows tuser's uid/gid" $?
fi
grep -q "^TTY=/dev/pts/[0-9]" "$W/k2" && grep -q "^PTYOWN-tuser:tty:620" "$W/k2"
ok "SSH pty slave owned by tuser, group tty, mode 0620" $?

grep -q "^RPTY0-root:tty:620" "$W/k5" && grep -q "^RPTY1-tuser:tty:620" "$W/k5"
ok "SSH root's pty slave is root:tty 0620 and root can chown it to a user" $?

# ---- Dropbear: password logins ----------------------------------------------
grep -q "^PWLOGIN-42" "$W/p1" && grep -Eq "^Uid:\s+1000\s+1000" "$W/p1" && grep -q "^HOME=/home/tuser" "$W/p1"
ok "SSH password login as tuser (SHA-512-crypt from /etc/shadow, libc crypt)" $?
! grep -q "PWWRONG-RAN" "$W/p2" && grep -q "Permission denied" "$W/p2"; ok "SSH wrong password refused" $?
! grep -q "PWLOCKED-RAN" "$W/p3" && grep -q "Permission denied" "$W/p3"; ok "SSH locked account ('!' in /etc/shadow) refused for password auth" $?
! grep -q "PWROOT-RAN" "$W/p4" && grep -q "Permission denied" "$W/p4"; ok "SSH root password login refused (correct password)" $?
grep -Eq "^Uid:\s+0\s+0\s+0\s+0" "$W/k3" && grep -q "^ROOTKEY-42" "$W/k3"; ok "SSH root key login still works (uid 0)" $?
! grep -q "WRONGKEY-RAN" "$W/k4" && grep -q "Permission denied" "$W/k4"; ok "SSH root's key does not open tuser's account" $?
grep -q "Pubkey auth succeeded for 'tuser'" "$W/k3" && grep -q "Password auth succeeded for 'tuser'" "$W/k3"
ok "sshd log: tuser key and password logins" $?
grep -q "Bad password attempt for 'tuser'" "$W/k3" && grep -q "User account 'locked' is locked" "$W/k3"
ok "sshd log: wrong password and locked account" $?
! grep -q "Password auth succeeded for 'root'" "$W/k3"; ok "sshd log: no root password success" $?
! grep -q "\[EXC\]\|\[PANIC\]" "$LOG"; ok "no CPU exception or panic" $?

if [[ $FAILN -ne 0 ]]; then
    KEEP=1
    echo "    --- serial (tail)"; tail -60 "$S" | sed 's/^/    /'
    echo "    --- ssh"; sed 's/^/    /' "$W/ssh.txt"
fi
echo "RESULT: $PASSN passed, $FAILN failed"
[[ $FAILN -eq 0 ]]
