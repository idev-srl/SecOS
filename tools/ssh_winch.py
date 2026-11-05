#!/usr/bin/env python3
"""SSH pty + window-change probe for the selftest (host side).

Runs `ssh -tt <args> <remote command>` on a pseudo-terminal whose size is set
to 30x90 before ssh starts (so the pty-req carries it), waits for the remote
side to print WINCH-READY, then resizes the local pty to 40x100 and sends
SIGWINCH to ssh (which turns it into an SSH window-change request). Everything
the session prints goes to stdout; the exit status is ssh's.

usage: ssh_winch.py <timeout-seconds> <ssh args...>
SPDX-License-Identifier: MIT
"""
import fcntl, os, pty, select, signal, struct, sys, termios, time


def set_size(fd, rows, cols):
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))


def main():
    timeout = float(sys.argv[1])
    args = sys.argv[2:]
    pid, master = pty.fork()
    if pid == 0:
        set_size(0, 30, 90)
        os.execvp("ssh", ["ssh"] + args)
    set_size(master, 30, 90)
    out = b""
    resized = False
    deadline = time.time() + timeout
    while time.time() < deadline:
        r, _, _ = select.select([master], [], [], 0.5)
        if r:
            try:
                data = os.read(master, 4096)
            except OSError:
                break
            if not data:
                break
            out += data
            sys.stdout.write(data.decode("utf-8", "replace"))
            sys.stdout.flush()
        if not resized and b"WINCH-READY" in out:
            time.sleep(0.5)
            set_size(master, 40, 100)
            os.kill(pid, signal.SIGWINCH)
            resized = True
        done, status = os.waitpid(pid, os.WNOHANG)
        if done:
            sys.exit(os.waitstatus_to_exitcode(status))
    try:
        os.kill(pid, signal.SIGKILL)
    except OSError:
        pass
    sys.exit(124)


if __name__ == "__main__":
    main()
