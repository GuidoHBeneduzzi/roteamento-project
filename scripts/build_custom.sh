#!/bin/bash
# build_custom.sh — compila o algoritmo proprio como binario estatico (musl),
# para rodar dentro dos containers FRR (Alpine) sem instalar nada neles.
set -e
cd "$(dirname "$0")/.."
docker run --rm -v "$PWD/custom-algorithm":/src -w /src alpine:3.20 \
  sh -c "apk add --no-cache build-base >/dev/null && make clean && make LDFLAGS=-static"
echo "OK: custom-algorithm/router (estatico)"
