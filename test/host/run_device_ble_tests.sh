#!/bin/bash
# Run device BLE tests (DEV-BLE suite).
# Spec: ESP_BLE_Device_Development_Spec_v1.3.md §26.5
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$SCRIPT_DIR/../.."
BUILD_DIR="$SCRIPT_DIR/build"

echo "=== Building device BLE tests ==="
mkdir -p "$BUILD_DIR"
cd "$PROJECT_DIR"

# Compile test (run from project root to read source files)
gcc -std=c11 -Wall -Wextra -pedantic \
    test/host/test_device_ble.c \
    -o test/host/build/test_device_ble

echo "=== Running device BLE tests ==="
cd "$SCRIPT_DIR"
./build/test_device_ble

echo ""
echo "=== All device BLE tests passed ==="
