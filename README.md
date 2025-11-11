## POSIX Message Queue Benchmark (C++)

This repository contains a safe, configurable benchmark for POSIX message queues (mq_*). It measures throughput (messages/sec), bandwidth (MiB/s), and latency percentiles, and lets you sweep configurations (message sizes, producers/consumers).

Important: macOS does not support POSIX mqueues; we run mqueue tests via Docker and run GCD/NSOperation natively.

### Features
- Safe defaults (short duration, conservative queue sizes)
- Producer/consumer thread scaling
- Message size sweeps
- Human-readable summary and CSV output
- Graceful shutdown on SIGINT/SIGTERM

### Quick start (macOS)
```bash
# Runs mqueue tests in Docker + GCD and NSOperation natively; writes results/results_all.csv
bash scripts/run_all.sh
```
Requirements:
- Docker Desktop (or Colima) for the mqueue runs
- Xcode command line tools for native mac builds

### Configuration knobs (applies to all backends)
- duration, message sizes, producer/consumer counts, latency sampling, CSV path
- See scripts for environment variables; all results append to:
```
backend,queueName,duration,messageSize,maxMessages,producers,consumers,nonBlocking,randomPayload,latencySample,elapsedSec,recv,bytesRecv,msgPerSec,MiBps,p50us,p90us,p95us,p99us,p999us
```
Safety:
- Conservative defaults (3–5s runs, bounded queue depth), graceful Ctrl+C handling, auto-cleanup of queues.

### Collected results (from results/results_all.csv)
- **Environment**: mqueue inside Ubuntu 24.04 container; GCD/NSOperation on macOS host
- **Matrix**: durations 3s; message sizes 64/256/1024/4096/8192; producers and consumers ∈ {1,2,4}
- All results below are approximate (top lines from the CSV).

- **POSIX mqueue (Linux, in Docker)**
  - **Peak bandwidth**: ~5069 MiB/s at `message-size=8192`, `producers=4`, `consumers=1` (throughput ~649k msg/s)
  - **Peak msg/s**: ~649k msg/s at `message-size=8192`, `producers=4`, `consumers=1` (bandwidth ~5069 MiB/s)
  - **Latency (typical p50)**: ~7–23 µs depending on size/threads; e.g., `8192B, 1x4` shows p50 ~7.38 µs
  - **Scaling**:
    - Throughput rises with `producers` and moderate `consumers`, but can drop at `4x4` due to contention (e.g., `64B 4x4` ~199k msg/s).
    - Larger `message-size` yields much higher bandwidth while keeping sub-~30 µs p99 in many cases.

- **Grand Central Dispatch (macOS)**
  - **Peak bandwidth**: ~15084 MiB/s at `message-size=8192`, `producers=4`, `consumers=1` (throughput ~1.93M msg/s)
  - **Peak msg/s**: ~4.92M msg/s at `message-size=64`, `producers=2`, `consumers=1` (bandwidth ~300 MiB/s)
  - **Latency (typical p50)**: single-digit µs for larger messages; often 1–7 µs p50
  - **Scaling**:
    - Best throughput generally with more producers; extra consumer queues can reduce throughput for small messages.

- **NSOperationQueue (macOS)**
  - **Peak bandwidth**: ~1953 MiB/s at `message-size=8192`, `producers=4`, `consumers=2` (throughput ~250k msg/s)
  - **Peak msg/s**: ~628k msg/s at `message-size=64`, `producers=2`, `consumers=1` (bandwidth ~38 MiB/s)
  - **Latency (typical p50)**: much higher than GCD; hundreds to thousands of µs p50 in many cases
  - **Scaling**:
    - Improves with more producers; increasing `maxConcurrentOperationCount` helps less and can increase latency.

Notes:
- The mqueue benchmark uses real POSIX IPC, while the macOS GCD/NSOperation results reflect in-process scheduling/queues, so the latter will appear much faster.
- The mqueue runner inside Docker caps queue attributes to kernel limits automatically.

### Analysis and insights
- **Kernel IPC vs in-process queues**
  - mqueue is kernel-mediated IPC; each message incurs syscalls/context switches. This bounds msg/s compared to in-process queues like GCD. GCD achieves multi-million msg/s because it schedules work within a single process.
  - Use mqueue results to size real interprocess pipelines; use GCD results for in-process stage fan-out.

- **Message size trade-offs**
  - For mqueue, increasing `message-size` boosts bandwidth (MiB/s) substantially while p50 latency remains in the tens of microseconds; p99 tails grow with contention.
  - For GCD, larger messages yield very high bandwidth while keeping very low p50 latencies; tails depend on queue contention.
  - NSOperationQueue has much higher per-message overhead and is not ideal for high-rate micro-message workloads.

- **Thread scaling behavior**
  - mqueue scales with additional producers; many consumers can reduce throughput due to contention and wakeups (e.g., `64B 4x4` drop). Favor more producers with 1–2 consumers for best throughput.
  - GCD often performs best with more producers and fewer consumer queues for small messages; too many consumer queues can hurt small-message throughput.
  - NSOperationQueue benefits from producers but exhibits higher latency overall; prefer limited concurrency for stability.

- **Latency takeaways**
  - GCD has the lowest p50 across sizes; mqueue p50 is typically tens of microseconds; NSOperationQueue p50 is hundreds–thousands of microseconds.
  - If tail latency matters, keep consumer parallelism modest and avoid queue overload (smaller queue depth with backpressure).

- **Practical guidance**
  - For cross-process throughput on Linux, mqueue can reach multi-GB/s with 8–16 KiB messages; consider running outside Docker to remove container overhead and tune `msgsize_max`/`msg_max` if allowed.
  - For macOS in-process pipelines, prefer GCD for hot paths; use NSOperationQueue for higher-level orchestration, not ultra-low-latency message pumps.
  - Start with `producers=2`, `consumers=1–2`, and sizes 1–8 KiB; adjust based on observed tails and CPU saturation.

### Run components separately (optional)
```bash
# Only mqueue (Docker)
bash scripts/docker_build_and_run.sh

# Only GCD + NSOperation (macOS)
make
bash scripts/run_gcd_macos.sh
```

### Files
- `src/mq_benchmark.cpp`: benchmark implementation
- `Makefile`: builds GCD/NSOperation on macOS; Linux build is used only inside Docker
- `scripts/run_matrix.sh`: quick sweep across sizes and thread counts
- `scripts/docker_build_and_run.sh`: build and run the matrix inside Docker on macOS
- `Dockerfile`: Ubuntu-based container to build and run the benchmark

### FAQ
- Can I measure latency? Yes — messages include a send timestamp; consumers compute end-to-end latency (requires message size >= header).
