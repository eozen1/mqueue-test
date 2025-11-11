#!/usr/bin/env bash
set -euo pipefail

if [[ "$(uname -s)" != "Darwin" ]]; then
  echo "This script is intended for macOS."
  exit 2
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RESULTS_DIR="$ROOT/results"
CSV="$RESULTS_DIR/results_all.csv"
mkdir -p "$RESULTS_DIR"

echo "Building GCD and NSOperation benchmarks..."
make -C "$ROOT" all

GCD_BIN="$ROOT/build/gcd_benchmark"
NSOP_BIN="$ROOT/build/nsop_benchmark"

if [[ ! -f "$CSV" ]]; then
  echo "backend,queueName,duration,messageSize,maxMessages,producers,consumers,nonBlocking,randomPayload,latencySample,elapsedSec,recv,bytesRecv,msgPerSec,MiBps,p50us,p90us,p95us,p99us,p999us" > "$CSV"
fi

DURATION="${DURATION:-3}"
LAT_SAMPLE="${LAT_SAMPLE:-50000}"
RANDPAY="${RANDPAY:-false}"
MSG_SIZES="${MSG_SIZES:-64 256 1024 4096 8192}"
THREADS_P="${THREADS_P:-1 2 4}"
THREADS_C="${THREADS_C:-1 2 4}"
INFLIGHT="${INFLIGHT:-1024}"

echo "Running GCD matrix..."
for ms in $MSG_SIZES; do
  for p in $THREADS_P; do
    for c in $THREADS_C; do
      echo "==> GCD size=$ms producers=$p consumers=$c"
      "$GCD_BIN" \
        --duration-seconds "$DURATION" \
        --message-size "$ms" \
        --max-inflight "$INFLIGHT" \
        --producers "$p" \
        --consumers "$c" \
        --random-payload "$RANDPAY" \
        --latency-sample "$LAT_SAMPLE" \
        --print-interval 1 \
        --csv "$CSV"
      echo
    done
  done
done

echo "Running NSOperationQueue matrix..."
for ms in $MSG_SIZES; do
  for p in $THREADS_P; do
    for c in $THREADS_C; do
      echo "==> NSOperation size=$ms producers=$p consumers=$c"
      "$NSOP_BIN" \
        --duration-seconds "$DURATION" \
        --message-size "$ms" \
        --max-inflight "$INFLIGHT" \
        --producers "$p" \
        --consumers "$c" \
        --random-payload "$RANDPAY" \
        --latency-sample "$LAT_SAMPLE" \
        --print-interval 1 \
        --csv "$CSV"
      echo
    done
  done
done

echo "Done. Results at $CSV"


