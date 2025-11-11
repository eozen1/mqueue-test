#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE="mqbench:local"

docker build -t "$IMAGE" "$ROOT"

# Use host CPU constraints; mount repo for results
docker run --rm -it \
  --name mqbench \
  -v "$ROOT:/work" \
  "$IMAGE" \
  bash -lc "make all && bash scripts/run_matrix.sh"


