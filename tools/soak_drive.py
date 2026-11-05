#!/usr/bin/env python3
# tools/soak_drive.py — randomized interactive bash-session driver for SecOS.
# SPDX-License-Identifier: MIT
#
# Boots the SecOS ISO in QEMU (into the signed GNU bash login shell) and drives
# a long, seeded, realistic session over the serial console: coreutils, loops,
# background jobs, file I/O in /tmp (ramfs), /proc and /dev reads, and (when
# available) the signed syscall fuzzer. Every logical step ends with a unique
# sentinel `echo OK-<n>` that must appear in the output — a missing sentinel,
# a CPU exception/panic/kill, a heap-corruption marker, or a stall all FAIL.
#
# Invoked by tools/soak.sh, which assembles the QEMU argv. Not meant to be run
# by hand (use tools/soak.sh).
#
# Protocol with soak.sh: argv after "--" is the full QEMU command. Options
# before "--" configure the session. On failure the driver writes a reproducer
# bundle (seed, config, the exact command list, last output) next to the log.
import sys, os, time, random, subprocess, threading, re

def parse(argv):
    o = {"seed":1234,"iters":400,"settle":25.0,"stall":20.0,"cmdsub":1,"pipes":1,"fuzz":0,
         "disk":0,"log":"/tmp/soak.log","smp":1}
    i=0
    while i < len(argv):
        a=argv[i]
        if a=="--": return o, argv[i+1:]
        k=a.lstrip("-")
        if k in ("seed","iters","smp"): o[k]=int(argv[i+1]); i+=2; continue
        if k in ("settle","stall"):     o[k]=float(argv[i+1]); i+=2; continue
        if k in ("cmdsub","pipes","fuzz","disk"): o[k]=1; i+=1; continue
        if k in ("no-cmdsub","no-pipes"): o[k[3:]]=0; i+=1; continue
        if k=="log": o["log"]=argv[i+1]; i+=2; continue
        i+=1
    return o, []

# ---- command-fragment generators (each returns a bash command string) --------
WORDS=["alpha","bravo","charlie","delta","echo1","foxtrot","42","secos","x","hello world"]
def rnd_word(r): return r.choice(WORDS)

def gen_command(r, st):
    """Return (cmd, expect_substr_or_None). cmd must be a single bash line."""
    f=r.randint(0,24)
    tmp=st["tmp"]
    if f==0:  return (f"echo {rnd_word(r)}", None)
    if f==1:  return ("seq 1 %d" % r.randint(1,40), None)
    if f==2:  return ("seq 1 %d | wc -l" % r.randint(1,40), None) if st["pipes"] else ("ls /bin", None)
    if f==3:  return ("ls /bin", None)
    if f==4:  return ("ls /", None)
    if f==5:  return (f"echo {r.randint(0,99999)} > {tmp}/f{st['fc']}; cat {tmp}/f{st['fc']}", None)
    if f==6:
        n=st['fc']; st['fc']+=1
        return (f"echo data{n} > {tmp}/f{n}", None)
    if f==7:  return (f"cat {tmp}/f{max(0,st['fc']-1)} 2>/dev/null || echo NOFILE", None)
    if f==8:  return (f"rm -f {tmp}/f{max(0,st['fc']-1)}", None)
    if f==9:  return (f"mkdir -p {tmp}/d{st['dc']}; ls {tmp}", None)
    if f==10: return ("for i in 1 2 3 4 5; do echo L$i; done", None)
    if f==11: return ("x=0; for i in 1 2 3; do x=$((x+i)); done; echo SUM=$x", "SUM=6")
    if f==12: return ("cat /proc/meminfo", "MemTotal")
    if f==13: return ("cat /proc/uptime", None)
    if f==14: return ("uname -a", "SecOS")
    if f==15: return ("uptime", None)
    if f==16: return ("head -c 32 /dev/zero | wc -c" if st["pipes"] else "stat /bin/echo", None)
    if f==17: return ("cat /dev/null; echo NULLOK", "NULLOK")
    if f==18: return ("seq 1 60", None)   # long output, scrolls the fb console
    if f==19: return ("W" * (120 + r.randint(0,80)), None)  # long wrapped line (echo'd by tty)
    if f==20: return ("sleep 1 & echo BG$!", None)
    if f==21: return ("true && echo ANDOK || echo FAIL", "ANDOK")
    if f==22: return ("stat %s/f%d 2>/dev/null || echo NOSTAT" % (tmp, max(0,st['fc']-1)), None)
    if f==23 and st["cmdsub"]: return ("echo S-$(echo SUB)", "S-SUB")
    if f==24 and st["pipes"]:  return ("seq 1 10 | head -3 | tail -1", None)
    return ("echo tick", None)

def main():
    o, qemu = parse(sys.argv[1:])
    r = random.Random(o["seed"])
    st = {"tmp":"/tmp","fc":0,"dc":0,
          "pipes": bool(o["pipes"]),  # pipelines on unless --no-pipes
          "cmdsub": o["cmdsub"]}       # $(...) on unless --no-cmdsub
    # legacy override: SOAK_PIPES=0 disables pipelines
    if os.environ.get("SOAK_PIPES") is not None:
        st["pipes"] = bool(int(os.environ["SOAK_PIPES"]))

    print(f"[soak] seed={o['seed']} iters={o['iters']} smp={o['smp']} disk={o['disk']} "
          f"cmdsub={o['cmdsub']} fuzz={o['fuzz']} pipes={st['pipes']}", flush=True)

    p = subprocess.Popen(qemu, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT)
    buf = bytearray()
    lock = threading.Lock()
    alive = [True]
    def rd():
        while True:
            b = p.stdout.read1(65536)
            if not b:
                alive[0]=False; break
            with lock: buf.extend(b)
    threading.Thread(target=rd, daemon=True).start()

    def wait_for(needle, timeout, since=0):
        nb = needle.encode()
        t0=time.time()
        while time.time()-t0 < timeout:
            with lock:
                if nb in buf[since:]: return True
            if not alive[0]: return False
            time.sleep(0.1)
        return False

    def tail(nbytes=4000):
        with lock: return bytes(buf[-nbytes:]).decode("latin1")

    FAIL_MARKERS = ["[EXC] int=", "[PANIC]", "nested fault", "stack smashing",
                    "[HEAP] CORRUPT", "UNHANDLED fault", "triple"]
    def check_crash():
        with lock: s = bytes(buf).decode("latin1")
        for m in FAIL_MARKERS:
            if m in s:
                # a ring-3 [KILL] alone is fine; only kernel-side markers fail.
                if m=="[EXC] int=" and "ring0" not in s and "ring3" in s:
                    continue
                return m
        return None

    def early_exit_fail():
        return not alive[0]

    # wait for the bash prompt (process table banner then bash)
    if not wait_for("[PROC] process table", o["settle"]):
        print("[soak] FAIL: kernel did not reach the shell within settle window")
        bundle(o, [], tail(), "no-boot"); p.kill(); sys.exit(1)
    time.sleep(3)

    sent = []  # (index, cmd, expect)
    n = o["iters"]
    i = 0
    while i < n:
        cmd, expect = gen_command(r, st)
        sentinel = f"OK-{i}"
        line = f"{cmd}; echo {sentinel}"
        sent.append((i, line, expect))
        with lock: since = len(buf)
        try:
            p.stdin.write(line.encode()+b"\r"); p.stdin.flush()
        except BrokenPipeError:
            print(f"[soak] FAIL: QEMU stdin closed (crash?) at cmd {i}: {cmd}")
            bundle(o, sent, tail(), "stdin-closed"); p.kill(); sys.exit(1)

        # every command must reach its sentinel within the stall window
        ok = wait_for(sentinel, o["stall"], since=since)
        crash = check_crash()
        if crash:
            print(f"[soak] FAIL: kernel-side fault marker {crash!r} after cmd {i}: {cmd}")
            bundle(o, sent, tail(), "crash:"+crash); p.kill(); sys.exit(1)
        if early_exit_fail():
            print(f"[soak] FAIL: QEMU exited (triple fault?) at cmd {i}: {cmd}")
            bundle(o, sent, tail(), "qemu-exit"); p.kill(); sys.exit(1)
        if not ok:
            print(f"[soak] FAIL: no progress (sentinel {sentinel} missing) within "
                  f"{o['stall']}s after cmd {i}: {cmd}")
            bundle(o, sent, tail(), "stall"); p.kill(); sys.exit(1)
        if expect:
            with lock: s = bytes(buf[since:]).decode("latin1")
            if expect not in s:
                print(f"[soak] FAIL: expected output {expect!r} missing after cmd {i}: {cmd}")
                bundle(o, sent, tail(), "missing-output:"+expect); p.kill(); sys.exit(1)
        i += 1
        if i % 50 == 0: print(f"[soak] {i}/{n} commands ok", flush=True)

    # optional: run the signed syscall fuzzer at the end (safe mode, bounded)
    if o["fuzz"]:
        with lock: since=len(buf)
        p.stdin.write(b"/bin/sysfuzz -s %d -b 10 -c 120 -S; echo FUZZDONE\r" % (o["seed"] & 0xffff))
        p.stdin.flush()
        wait_for("FUZZDONE", 60, since=since)
        crash = check_crash()
        if crash:
            print(f"[soak] FAIL: fuzzer tripped kernel fault {crash!r}")
            bundle(o, sent, tail(), "fuzz-crash:"+crash); p.kill(); sys.exit(1)

    print(f"[soak] PASS: {n} commands, no kernel fault, all sentinels present")
    p.kill()
    sys.exit(0)

def bundle(o, sent, tailtxt, reason):
    base = o["log"] + ".repro"
    try:
        with open(base, "w") as f:
            f.write(f"# soak reproducer\nreason={reason}\nseed={o['seed']}\n"
                    f"iters={o['iters']} smp={o['smp']} disk={o['disk']} "
                    f"cmdsub={o['cmdsub']} fuzz={o['fuzz']}\n\n")
            f.write("# last commands sent (newest last):\n")
            for idx, line, exp in sent[-40:]:
                f.write(f"{idx}: {line}\n")
            f.write("\n# last serial output:\n")
            f.write(tailtxt)
        print(f"[soak] reproducer written to {base}")
    except OSError as e:
        print(f"[soak] could not write reproducer: {e}")

if __name__ == "__main__":
    main()
