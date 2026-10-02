"""CLI contract and recording integration tests; Python standard library only."""
import csv
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time

binary = str(Path(sys.argv[1]).resolve())

def run(*args, ok=True):
    result = subprocess.run([binary, *args], capture_output=True, text=True, timeout=8)
    assert (result.returncode == 0) == ok, (args, result.returncode, result.stderr)
    return result

assert "nitro" in run("--version").stdout
assert "--demo" in run("--help").stdout
for args in [("--unknown",), ("--interval", "0"), ("--count", "-1"), ("--duration", "nan"), ("--device", ""), ("--theme", "bad")]:
    run(*args, ok=False)
assert "terminal" in run(ok=False).stderr.lower()
with tempfile.TemporaryDirectory(prefix="nitro-cli-") as tmp:
    trace, counters = Path(tmp) / "trace.json", Path(tmp) / "counters.csv"
    result = run("--demo", "--json", "--count", "3", "--interval", "100", "--device", "mlx5_0/1", "--record", str(trace), "--csv", str(counters))
    rows = [json.loads(line) for line in result.stdout.splitlines()]
    assert len(rows) == 3
    for row in rows:
        assert row["demo"] is True and len(row["ports"]) == 1
        assert row["ports"][0]["device"] == "mlx5_0" and row["ports"][0]["tx_gbps"] > 0
        assert row["ports"][0]["counters"]["tx_bytes"]["total"] > 0
    events = json.loads(trace.read_text())["traceEvents"]
    rates = [e for e in events if e["ph"] == "C"]
    assert len(rates) == 15
    assert sorted(e["ts"] for e in rates) == [e["ts"] for e in rates]
    assert all(0 < e["ts"] < 1_000_000 for e in rates)
    with counters.open() as file:
        records = list(csv.DictReader(file))
    assert len(records) == 3 * len(rows[0]["ports"][0]["counters"])
    original = trace.read_bytes()
    run("--demo", "--count", "1", "--record", str(trace), ok=False)
    assert trace.read_bytes() == original
    run("--demo", "--json", "--device", "nonexistent", ok=False)
    duration = run("--demo", "--duration", "0.36", "--interval", "100")
    assert 2 <= len(duration.stdout.splitlines()) <= 3
    stopped = Path(tmp) / "stopped.json"
    process = subprocess.Popen([binary, "--demo", "--headless", "--interval", "100", "--record", str(stopped)], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    deadline = time.monotonic() + 4
    while (not stopped.exists() or stopped.stat().st_size < 200) and time.monotonic() < deadline:
        time.sleep(0.02)
    process.send_signal(signal.SIGTERM)
    _, error = process.communicate(timeout=3)
    assert process.returncode == 0, error
    assert json.loads(stopped.read_text())["traceEvents"]
print("CLI, NDJSON, CSV, Perfetto, filters, deadlines, and SIGTERM passed")
