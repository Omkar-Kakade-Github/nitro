# Contributing to nitro

Use C++20 and keep the application build offline and small. The only application
dependencies are ncursesw, Linux headers and the C++ standard library.

## C++ conventions

- Own resources explicitly. File descriptors, the terminal and worker threads
  have RAII lifetimes. Never detach a sampling thread.
- Prefer value types, `std::unique_ptr`, and immutable snapshots. Shared pointers
  are used only for the bounded sampler-to-UI mailbox. Raw UI pointers are
  non-owning views into a snapshot kept alive for the entire frame.
- Use `std::optional` for unavailable measurements. Preserve integers until
  after subtracting counters; use a monotonic clock for elapsed durations.
- Keep driver I/O, rate math, rendering and export separate. A collector must
  not depend on ncurses. Rendering must not perform driver I/O.
- Parse kernel/user-controlled input defensively. Check lengths before reading,
  use `memcpy` for unaligned netlink data, bound allocations, and sanitize
  process/device text before terminal output.
- Do not fabricate zeros for missing counters, invent per-process bandwidth,
  double-count aliases, or mix ordinary interface traffic with RDMA totals.
- Keep public headers self-contained, use `const` where appropriate, and avoid
  owning raw pointers, global mutable state, and macros outside small test
  assertions. The signal-safe exit flag is the deliberate global exception.
- Use exceptions at I/O boundaries; report failures without breaking unrelated
  collectors. Destructors do not throw. Explicit recording finalization reports
  write errors; destructor finalization is best effort.
- Format with `.clang-format`. Keep warnings enabled; GCC and Clang must both
  compile with `NITRO_WARNINGS_AS_ERRORS=ON`.

## Checks

```sh
clang-format --dry-run --Werror include/nitro/*.hpp src/*.cpp tests/tests.cpp benchmarks/*.cpp
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
cmake --preset sanitize && cmake --build --preset sanitize && ctest --preset sanitize
clang-tidy -p build/debug src/*.cpp
```

The standard-library-only C++ test harness deliberately remains active in
release builds. Python integration tests use only the standard library. Add
behavioral regression cases when changing units, parsing, rate calculations,
recording, or terminal lifecycle. Fixture tests must not depend on installed
RDMA hardware or root. Keep captured counter values synthetic or anonymized.

LeakSanitizer cannot inspect processes in some ptrace-based sandboxes. On those
hosts `ASAN_OPTIONS=detect_leaks=0 ctest --preset sanitize` can still check address
and undefined behavior errors. Perform the complete leak-enabled run on an
unrestricted host or CI before treating leak checking as complete.

For a change affecting hardware queries, record kernel/driver version, hardware,
privilege level, commands used for comparison, and any unavailable counters.
Do not compare a synthetic benchmark against hardware latency or claim btop
performance parity without a controlled measurement.
