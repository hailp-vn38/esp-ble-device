#!/bin/sh
# Cross-interop check: Device codec vs the REAL Gateway QCBOR codec stack.
# Usage: sh test/host/run_gateway_interop_check.sh [path-to-esp-ble-gateway]
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
GATEWAY="${1:-$HOME/Desktop/esp32-ble-gateway}"
QCBOR="$GATEWAY/components/qcbor_lib/QCBOR"
OUT="$DIR/build"
mkdir -p "$OUT"

if [ ! -d "$QCBOR/src" ]; then
    echo "error: QCBOR not found under $GATEWAY" >&2
    exit 2
fi

cc -std=c11 -Wall -Wextra -Werror -O2 -DGW_HOST_TEST \
   -I"$DIR/../../components/gateway_protocol/include" \
   -I"$QCBOR/inc" \
   "$DIR/../../components/gateway_protocol/gateway_protocol.c" \
   "$QCBOR/src/qcbor_encode.c" \
   "$QCBOR/src/qcbor_decode.c" \
   "$QCBOR/src/qcbor_err_to_str.c" \
   "$QCBOR/src/UsefulBuf.c" \
   "$QCBOR/src/ieee754.c" \
   "$DIR/test_gateway_interop.c" \
   -o "$OUT/test_gateway_interop"

"$OUT/test_gateway_interop"
