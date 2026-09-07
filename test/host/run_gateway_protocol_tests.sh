#!/bin/sh
# Host unit tests for gateway_protocol (no ESP-IDF required).
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
OUT="$DIR/build"
mkdir -p "$OUT"

cc -std=c11 -Wall -Wextra -Werror -O2 \
   -DGW_HOST_TEST \
   -I"$DIR/../../components/gateway_protocol/include" \
   "$DIR/../../components/gateway_protocol/gateway_protocol.c" \
   "$DIR/../../components/gateway_protocol/gateway_settings.c" \
   "$DIR/test_gateway_protocol.c" \
   -o "$OUT/test_gateway_protocol"

"$OUT/test_gateway_protocol"
