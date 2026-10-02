# Architecture

`nitro_core` is a C++20 library with no terminal dependency. `nitro` adds the
command-line frontend and ncursesw renderer. The monitor is read-only with
respect to hardware and processes.

## Data path

1. `LinuxCollector` enumerates RDMA devices through `NETLINK_RDMA`, augments the
   result with sysfs ports, queries hardware counters, and reads standard IB
   counters. Independent ordinary network counters come from `/proc/net/dev`.
2. Counter aliases normalize EFA, erdma and IB providers. Original driver totals
   are retained. IB word-to-byte conversion happens only for canonical aliases.
3. Each port gets a steady-clock timestamp immediately after its counters are
   read. `Metrics` keys baselines by kind, device, port and counter name; it
   computes rates using that port's actual elapsed time.
4. The worker streams each measured snapshot to an active recording before
   publishing it. The UI reads an immutable latest snapshot from a single-slot
   mailbox. Slow rendering cannot create an unbounded queue or skip recording
   samples.
5. The UI combines visible port histories for the aggregate graph. ncurses
   computes terminal differences with `wnoutrefresh` / `doupdate`. Input is
   polled at 50 ms; data collection is independently adjustable from 100 ms to
   60 seconds. The screen redraws on input, new data, or the one-second status
   tick. No shell utilities are invoked while monitoring.

Histories contain at most 240 points per port. At the default interval this is
about four minutes; changing the interval changes the represented duration.
The visible graph shows the newest points fitting its width. Aggregation aligns
sampling passes, so its ports can have slightly different read timestamps.
Process enumeration runs every three seconds; metadata is read once per PID
within each pass. The process view groups QPs by PID and device, not port.

## Failure behavior

Unknown counters have no rate. Initial totals are baselines, never traffic.
Decreasing totals mark a reset/wrap and start a new baseline. A newly appearing
counter also requires a baseline. A transient missing counter retains its last
valid baseline; recovery uses the full intervening duration. A disappeared port
is pruned and must warm up if it returns. Changing counter sources resets its
baselines to avoid false deltas. Equal or decreasing sample timestamps do not
replace a newer baseline.

A netlink request has a 300 ms deadline and a 64 MiB response limit. Message
sequence, sender, lengths, multipart completion, kernel errors and interrupted
dumps are checked. Receive truncation is reported. Stop requests are checked
between ports and device QP queries; an in-flight kernel query may delay shutdown
until its timeout. Sysfs accesses still depend on the kernel/driver returning.
Normal unsupported-statistics errors use sysfs. Other failures become visible
warnings; they do not disable ordinary network collection. Unreadable counters
remain empty instead of becoming fabricated traffic.

Recorded files use exclusive creation and reject symlinks. Exports are streamed
per sample; memory use does not grow with recording duration. A failed write
stops the recording and surfaces an error. SIGINT, SIGTERM and SIGHUP request
normal cleanup. Forced termination and storage failure cannot guarantee a
complete JSON footer. Recording paths entered by a CLI flag are used as given.

The display is the only component that pauses. The background collector and
recorder continue when Space freezes the view. This makes paused inspection
consistent without losing the capture window.

## Module map

| Files | Responsibility |
| --- | --- |
| `model.*` | Data contracts, safe formatting, counter aliases |
| `netlink.*` | Checked RDMA wire parsing and socket RAII |
| `collector.*` | Linux I/O, process metadata, explicit demo provider |
| `metrics.*` | Baselines, rates, bounded histories, weighted averages |
| `sampler.*` | Worker lifecycle, single-slot publication, recording controls |
| `trace.*` | Exclusive trace/CSV writer and NDJSON serializer |
| `options.*` | CLI validation and device filtering |
| `tui.cpp` | Terminal lifecycle, panels, input, resize and mouse behavior |

## Hardware validation

These checks require an actual RDMA device and are not substitutes for automated
fixture tests:

1. Compare enumeration and logical port state with `rdma link show` and
   `/sys/class/infiniband/*/ports/*/state`.
2. Compare raw hardware totals in Enter's inspector with `rdma statistic show`.
   Compare standard counters with sysfs. Canonical IB byte totals must be four
   times `port_xmit_data` / `port_rcv_data`.
3. Generate traffic with an existing test such as `ib_write_bw` on a controlled
   fabric. Compare interval deltas, Gbps and packet rates, allowing for sampling
   window differences and protocol overhead. Nitro itself generates no traffic.
4. Compare QP owners and counts with `rdma resource show qp`; validate both
   unprivileged and privileged access. Process rows are per device.
5. Stop traffic, retrain a link, reset a device, and remove/re-add a device.
   Verify no rate spikes, stale baselines, or terminal stalls.
6. Record during workload changes, pause the UI, and inspect the result in
   Perfetto. Confirm recording continues during display pause.
7. Measure nitro's CPU, RSS and collection latency at the intended device count
   and refresh interval. Repeat on EFA and erdma before claiming provider-tested
   support. All three provider mappings currently have synthetic coverage.
