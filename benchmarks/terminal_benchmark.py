"""Measure a real nitro demo TUI in a PTY, including drawing and startup costs."""
import argparse
import fcntl
import os
from pathlib import Path
import pty
import resource
import select
import struct
import subprocess
import termios
import time

parser = argparse.ArgumentParser()
parser.add_argument("binary", type=Path)
parser.add_argument("--seconds", type=float, default=10)
parser.add_argument("--interval", type=int, default=100)
args = parser.parse_args()
if not 1 <= args.seconds <= 3600:
    parser.error("seconds must be 1..3600")
master, slave = pty.openpty()
fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 42, 132, 0, 0))
start = time.monotonic()
process = subprocess.Popen(
    [str(args.binary.resolve()), "--demo", "--interval", str(args.interval)],
    stdin=slave, stdout=slave, stderr=slave,
    env=dict(os.environ, TERM="xterm-256color", LANG="C.UTF-8"),
)
output_bytes = 0
try:
    while time.monotonic() - start < args.seconds and process.poll() is None:
        if select.select([master], [], [], 0.05)[0]:
            output_bytes += len(os.read(master, 65536))
    os.write(master, b"q")
    deadline = time.monotonic() + 3
    while process.poll() is None and time.monotonic() < deadline:
        if select.select([master], [], [], 0.05)[0]:
            output_bytes += len(os.read(master, 65536))
    process.wait(timeout=1)
    elapsed = time.monotonic() - start
    usage = resource.getrusage(resource.RUSAGE_CHILDREN)
    cpu = 100 * (usage.ru_utime + usage.ru_stime) / elapsed
    print(f"132x42 demo, {args.interval}ms sampling, {elapsed:.2f}s")
    print(f"CPU: {cpu:.2f}% of one core; child peak RSS: {usage.ru_maxrss / 1024:.2f} MiB")
    print(f"Terminal output: {output_bytes / elapsed / 1024:.1f} KiB/s; exit: {process.returncode}")
    if process.returncode:
        raise SystemExit(process.returncode)
finally:
    if process.poll() is None:
        process.kill()
        process.wait()
    os.close(master)
    os.close(slave)
