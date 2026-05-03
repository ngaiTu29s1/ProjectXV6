#!/usr/bin/env python3
"""Boot xv6 in QEMU, run bench_ipc, print results, exit."""

import os, select, signal, subprocess, sys, time

BOOT_TIMEOUT = 30   # seconds to wait for xv6 shell prompt
CMD_TIMEOUT  = 30   # seconds to wait for bench_ipc to finish
IDLE_WINDOW  = 0.5  # seconds of silence after marker before stopping

def read_until(fd, marker, timeout):
    """Read from fd until marker appears, then drain for IDLE_WINDOW seconds.
    Returns as soon as marker is seen + no new data for IDLE_WINDOW, rather
    than always waiting the full timeout."""
    buf = bytearray()
    deadline = time.time() + timeout
    marker_seen_at = None

    while True:
        now = time.time()
        if now >= deadline:
            break
        if marker_seen_at is not None and now - marker_seen_at >= IDLE_WINDOW:
            break

        wait = min(IDLE_WINDOW if marker_seen_at is not None else 1.0,
                   deadline - now)
        ready, _, _ = select.select([fd], [], [], wait)
        if ready:
            chunk = os.read(fd, 4096)
            if not chunk:
                break
            buf.extend(chunk)
            text = buf.decode("utf-8", "replace")
            if marker_seen_at is None and marker in text:
                marker_seen_at = time.time()
        elif marker_seen_at is not None:
            break  # silence after marker — done

    return buf.decode("utf-8", "replace")

def kill_qemu(proc):
    """Kill the entire process group so the QEMU child doesn't linger."""
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
        output = read_until(proc.stdout.fileno(), "$ ", BOOT_TIMEOUT)
        if "$ " not in output:
            print("ERROR: xv6 did not boot (no shell prompt)")
            print(output[-500:])
            sys.exit(1)
        print("xv6 booted OK")

        print("\n=== running bench_ipc ===")
        proc.stdin.write(b"bench_ipc\n")
        proc.stdin.flush()

        # Wait for the prompt to reappear — means bench_ipc has exited.
        result = read_until(proc.stdout.fileno(), "$ ", CMD_TIMEOUT)
        print(result)

    finally:
        kill_qemu(proc)

    for line in result.splitlines():
        if "rounds" in line and "ticks" in line:
            print("=== RESULT:", line.strip(), "===")
            return
    print("WARNING: could not find benchmark result line in output")

if __name__ == "__main__":
    main()
