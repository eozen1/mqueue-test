#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RESULTS_DIR="$ROOT/results"
CSV="$RESULTS_DIR/results_all.csv"
mkdir -p "$RESULTS_DIR"

if [[ ! -f "$CSV" ]]; then
  echo "backend,queueName,duration,messageSize,maxMessages,producers,consumers,nonBlocking,randomPayload,latencySample,elapsedSec,recv,bytesRecv,msgPerSec,MiBps,p50us,p90us,p95us,p99us,p999us" > "$CSV"
fi

OS="$(uname -s)"
if [[ "$OS" == "Darwin" ]]; then
  echo "Running POSIX mqueue inside Docker (Linux) and GCD/NSOperation natively (macOS)..."
  bash "$ROOT/scripts/docker_build_and_run.sh"
  bash "$ROOT/scripts/run_gcd_macos.sh"
elif [[ "$OS" == "Linux" ]]; then
  echo "Running POSIX mqueue natively on Linux..."
  bash "$ROOT/scripts/run_matrix.sh"
  echo "GCD/NSOperation benchmarks are macOS-only and will be skipped."
else
  echo "Unsupported OS: $OS"
  exit 2
fi

echo "All runs complete. Combined results at $CSV"


