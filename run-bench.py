#!/usr/bin/env python3
"""Boot xv6 in QEMU, run bench_ipc, print results, exit."""

import os, signal, subprocess, sys, time

BOOT_TIMEOUT = 30   # seconds to wait for xv6 shell prompt
CMD_TIMEOUT  = 20   # seconds to wait for bench_ipc to finish

def read_all(fd, timeout):
    """Non-blocking read for `timeout` seconds."""
    import select
    buf = bytearray()
    deadline = time.time() + timeout
    while time.time() < deadline:
        ready, _, _ = select.select([fd], [], [], 0.2)
        if ready:
            chunk = os.read(fd, 4096)
            if not chunk:
                break
            buf.extend(chunk)
    return buf.decode("utf-8", "replace")

def kill_qemu(proc):
    """Kill the entire process group so QEMU child doesn't linger."""
    try:
        os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
    except ProcessLookupError:
        pass
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
        proc.wait()

def main():
    print("=== booting xv6 ===")
    proc = subprocess.Popen(
        ["make", "qemu"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        start_new_session=True,   # own process group — lets killpg reach QEMU
    )

    try:
        # Wait for shell prompt
        output = read_all(proc.stdout.fileno(), BOOT_TIMEOUT)
        if "$ " not in output:
            print("ERROR: xv6 did not boot (no shell prompt)")
            print(output[-500:])
            sys.exit(1)
        print("xv6 booted OK")

        # Run bench_ipc
        print("\n=== running bench_ipc ===")
        proc.stdin.write(b"bench_ipc\n")
        proc.stdin.flush()

        result = read_all(proc.stdout.fileno(), CMD_TIMEOUT)
        print(result)

    finally:
        kill_qemu(proc)

    # Parse results
    for line in result.splitlines():
        if "rounds" in line and "ticks" in line:
            print("=== RESULT:", line.strip(), "===")
            return
    print("WARNING: could not find benchmark result line in output")

if __name__ == "__main__":
    main()
