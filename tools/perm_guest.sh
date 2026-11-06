#!/bin/bash
# tools/perm_guest.sh - permission-enforcement regression checks, run INSIDE
# SecOS by an unprivileged user (tools/selftest.sh, PERM gate:
# `su eve -c "/bin/bash /opt/perm_guest.sh"`). Every line it prints is
#   PT <case> rc=<exit status> <first line of the command's output>
# and the host side asserts the expected exit status and errno message.
# Setup done by root before (see the gate): users eve (not in wheel, video,
# disk) and bob (home /home/bob, 0700); /tmp/rid/id = a root-owned 4755 copy
# of /bin/usertools (its manifest has no SETID flag); NTFS read-only at /mnt;
# exFAT at /media mounted with uid=<eve>,umask=027.

t() {
    local name=$1; shift
    local out rc
    out=$("$@" 2>&1 </dev/null); rc=$?
    printf 'PT %s rc=%s %s\n' "$name" "$rc" "$(printf '%s\n' "$out" | head -n 1 | cut -c1-100)"
}

echo "PT-BEGIN uid=$(id -u) euid=$(id -un) groups=$(id -Gn | tr ' ' ,)"

# ---- must be denied -------------------------------------------------------
t shadow-read      cat /etc/shadow
t passwd-write     bash -c 'echo x >> /etc/passwd'
t kill-root        kill -0 1
t home-other       ls /home/bob
t dev-disk         head -c 1 /dev/vda
t dev-fb           head -c 1 /dev/fb0
touch "$HOME/mine"
t chown-to-root    chown root "$HOME/mine"
t setid-root       /tmp/rid/id -u
cp /bin/usertools "$HOME/id" && chmod u+s "$HOME/id"
t setid-own        "$HOME/id" -u
t sudo-nowheel     sudo id
t ntfs-write       bash -c 'echo x > /mnt/new.txt'
t ntfs-rm          rm -f /mnt/hello.txt

# ---- positive controls ----------------------------------------------------
t ok-passwd-read   head -n 1 /etc/passwd
t ok-home-write    bash -c 'echo hello > "$HOME/note" && cat "$HOME/note"'
t ok-ntfs-read     cat /mnt/hello.txt
t ok-own-chmod     chmod 600 "$HOME/mine"
t ok-exfat-write   bash -c 'echo exfat-ok > /media/eve.txt && cat /media/eve.txt'
t ok-exfat-owner   stat -c '%U %a' /media/eve.txt
echo "PT-END"
