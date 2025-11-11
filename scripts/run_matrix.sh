#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$ROOT/build/mq_benchmark"
RESULTS_DIR="$ROOT/results"
CSV="$RESULTS_DIR/results_all.csv"

mkdir -p "$RESULTS_DIR"

if [[ ! -x "$BIN" ]]; then
  echo "Building benchmark..."
  make -C "$ROOT" all
fi

if [[ ! -f "$CSV" ]]; then
  echo "backend,queueName,duration,messageSize,maxMessages,producers,consumers,nonBlocking,randomPayload,latencySample,elapsedSec,recv,bytesRecv,msgPerSec,MiBps,p50us,p90us,p95us,p99us,p999us" > "$CSV"
fi

DURATION="${DURATION:-3}"
MAXMSGS="${MAXMSGS:-10}"
LAT_SAMPLE="${LAT_SAMPLE:-50000}"
NONBLOCK="${NONBLOCK:-false}"
RANDPAY="${RANDPAY:-false}"

MSG_SIZES="${MSG_SIZES:-64 256 1024 4096 8192}"
THREADS_P="${THREADS_P:-1 2 4}"
THREADS_C="${THREADS_C:-1 2 4}"

echo "Running matrix:"
echo "  duration:    $DURATION s"
echo "  msg sizes:   $MSG_SIZES"
echo "  producers:   $THREADS_P"
echo "  consumers:   $THREADS_C"
echo "  nonblocking: $NONBLOCK  random-payload: $RANDPAY"
echo

for ms in $MSG_SIZES; do
  for p in $THREADS_P; do
    for c in $THREADS_C; do
      echo "==> size=$ms producers=$p consumers=$c"
      "$BIN" \
        --queue-name "/mq_bench" \
        --duration-seconds "$DURATION" \
        --message-size "$ms" \
        --max-messages "$MAXMSGS" \
        --producers "$p" \
        --consumers "$c" \
        --nonblocking "$NONBLOCK" \
        --random-payload "$RANDPAY" \
        --latency-sample "$LAT_SAMPLE" \
        --csv "$CSV" \
        --unlink-start true \
        --unlink-end true \
        --print-interval 1
      echo
    done
  done
done

echo "Done. Results at $CSV"


