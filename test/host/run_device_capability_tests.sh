#!/bin/bash
# Run device capability tests (DEV-CAP suite).
# Spec: ESP_BLE_Device_Development_Spec_v1.3.md §26.3
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"

echo "=== Building device capability tests ==="
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# Compile test
gcc -std=c11 -Wall -Wextra -pedantic \
    -I"$SCRIPT_DIR/../../components/gateway_protocol/include" \
    "$SCRIPT_DIR/test_device_capability.c" \
    "$SCRIPT_DIR/../../components/gateway_protocol/gateway_protocol.c" \
    -o test_device_capability

echo "=== Running device capability tests ==="
./test_device_capability

echo ""
echo "=== All device capability tests passed ==="
