"""Exercise the real ncurses application through a PTY; no terminal packages needed."""
import errno
import fcntl
import json
import os
from pathlib import Path
import pty
import select
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import time

binary = str(Path(sys.argv[1]).resolve())

def exercise(ascii_mode=False, terminate=False):
    with tempfile.TemporaryDirectory(prefix="nitro-pty-") as tmp:
        master, slave = pty.openpty()
        before = termios.tcgetattr(slave)
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 40, 120, 0, 0))
        environment = dict(os.environ, TERM="xterm-256color", LANG="C.UTF-8")
        command = [binary, "--demo", "--interval", "100"] + (["--ascii"] if ascii_mode else [])
        process = subprocess.Popen(command, stdin=slave, stdout=slave, stderr=slave, cwd=tmp, env=environment, start_new_session=True)
        output = bytearray()
        def drain(seconds=0.18):
            end = time.monotonic() + seconds
            while time.monotonic() < end:
                if select.select([master], [], [], min(0.02, max(0, end - time.monotonic())))[0]:
                    try:
                        chunk = os.read(master, 65536)
                        if not chunk: break
                        output.extend(chunk)
                    except OSError as error:
                        if error.errno != errno.EIO: raise
                        break
        def key(data):
            os.write(master, data)
            drain()
        try:
            drain(0.55)
            assert b"fabric throughput" in output and b"mlx5_0" in output, output[-3000:]
            key(b"h")
            assert b"keyboard & mouse" in output
            key(b"h\r")
            assert b"COUNTER" in output
            key(b"\x1b")
            key(b"/mlx5_1\r")
            key(b"\x1b")
            key(b"2")
            assert b"network throughput" in output
            key(b"1")
            # SGR mouse click on a port row, followed by the counter inspector.
            key(b"\x1b[<0;12;20M\x1b[<0;12;20m")
            key(b"\r")
            assert b"Source: DEMO" in output
            key(b"\x1b")
            key(b"w")
            assert b"diagnostics" in output
            key(b"w\t\r")
            assert b"Queue pairs" in output
            key(b"\x1b")
            key(b"sSatg+- ")
            key(b" ")
            key(b"r")
            key(b" ")
            drain(0.3)
            key(b" ")
            key(b"r")
            records = list(Path(tmp).glob("nitro-*.json"))
            assert len(records) == 1
            events = json.loads(records[0].read_text())["traceEvents"]
            assert len({event["ts"] for event in events if event["ph"] == "C"}) >= 3
            # Small terminals and repeated resizes must remain interactive.
            for rows, columns in [(12, 45), (24, 80), (50, 160), (32, 100)]:
                fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", rows, columns, 0, 0))
                process.send_signal(signal.SIGWINCH)
                drain()
            assert b"Resize terminal" in output
            if terminate:
                process.send_signal(signal.SIGTERM)
            else:
                key(b"q")
            process.wait(timeout=3)
            drain(0.05)
            assert process.returncode == 0, output[-3000:]
            after = termios.tcgetattr(slave)
            assert before == after, "Terminal settings were not restored"
            assert b"AddressSanitizer" not in output and b"runtime error:" not in output
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            os.close(master)
            os.close(slave)

exercise()
exercise(ascii_mode=True, terminate=True)
print("TUI rendering, navigation, search, dialogs, recording, resize, ASCII, and terminal restoration passed")
