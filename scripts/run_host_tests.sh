#!/usr/bin/env bash
# Host golden tests for the tunnel's pure code (no MCU required):
# MeshCoreTunnelCodec and PropPolicy.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "${ROOT}/.pio"

OUT="${ROOT}/.pio/host_test_codec"
c++ -std=c++17 -Wall -Wextra -O0 \
  -I "${ROOT}/examples/rns_gateway" \
  "${ROOT}/test/host/test_codec.cpp" \
  "${ROOT}/examples/rns_gateway/MeshCoreTunnelCodec.cpp" \
  -o "${OUT}"
"${OUT}"

OUT="${ROOT}/.pio/host_test_prop_policy"
c++ -std=c++17 -Wall -Wextra -O0 \
  -I "${ROOT}/examples/rns_gateway" \
  "${ROOT}/test/host/test_prop_policy.cpp" \
  "${ROOT}/examples/rns_gateway/PropPolicy.cpp" \
  "${ROOT}/examples/rns_gateway/MeshCoreTunnelCodec.cpp" \
  -o "${OUT}"
"${OUT}"
