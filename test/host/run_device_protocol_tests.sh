#!/bin/bash
# Run device protocol tests (DEV-PROTO suite).
# Spec: ESP_BLE_Device_Development_Spec_v1.3.md §26.1
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"

echo "=== Building device protocol tests ==="
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# Compile test
gcc -std=c11 -Wall -Wextra -pedantic \
    -I"$SCRIPT_DIR/../../components/gateway_protocol/include" \
    "$SCRIPT_DIR/test_device_protocol.c" \
    "$SCRIPT_DIR/../../components/gateway_protocol/gateway_protocol.c" \
    -o test_device_protocol

echo "=== Running device protocol tests ==="
./test_device_protocol

echo ""
echo "=== All device protocol tests passed ==="
