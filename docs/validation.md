# Local validation — 2 October 2026

The implementation was built and exercised in the development environment with
GCC 13.3, Clang 21.1, CMake 3.28 and ncursesw 6.4.

- GCC Debug and Release builds passed with warnings treated as errors.
- Clang Debug passed with warnings treated as errors.
- The C++ suite passed all 19 behavioral test groups, including 10,000
  deterministic malformed-netlink fuzz inputs.
- CLI integration passed: argument validation, NDJSON, device/port filters,
  bounded capture, CSV, Perfetto JSON, exclusive creation and signal cleanup.
- PTY integration passed in Unicode and ASCII modes, exercising navigation,
  search, mouse selection, help, diagnostics, counters, process inspection,
  recording during display pause, resizing and terminal restoration.
- ASan, UBSan and LeakSanitizer passed. Leak checking was run outside the
  ptrace-restricted sandbox, where LeakSanitizer itself cannot operate.
- The configured clang-tidy checks and clang-format checks completed cleanly.
- CMake installation into a temporary prefix produced a working binary, man
  page and license files.
- A live unprivileged collection outside the sandbox returned ordinary network
  interfaces, real byte and packet rates, and no collector warnings. RDMA
  device enumeration returned no hardware on this machine.

The release demo at 132 × 42, sampling every 100 ms over approximately six
seconds, measured **0.71% of one CPU core**, **11.37 MiB child peak RSS**, and
**10.2 KiB/s of terminal output**. The standalone demo-and-metrics benchmark
processed 70,000 port samples in approximately 295 ms (**0.0295 ms per
seven-port sampling pass**). These are short local synthetic measurements;
driver latency, terminal emulation cost and real fabrics require separate
measurements. Child peak RSS may include process startup before exec.

Reproduce measurements with:

```sh
cmake --preset release -DNITRO_BUILD_BENCHMARKS=ON
cmake --build --preset release
./build/release/nitro_benchmark
python3 benchmarks/terminal_benchmark.py ./build/release/nitro --seconds 10 --interval 100
```

The GitHub Actions workflow is provided but has not been run on GitHub from this
workspace. Physical ConnectX/EFA/erdma validation remains outstanding; use the
[hardware checklist](architecture.md#hardware-validation) on an RDMA host.
