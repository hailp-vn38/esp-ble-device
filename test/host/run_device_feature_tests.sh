#!/bin/bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
mkdir -p "$BUILD_DIR"
cc -std=c11 -Wall -Wextra -Werror \
    -I"$SCRIPT_DIR/../../components/gateway_protocol/include" \
    -I"$SCRIPT_DIR/../../components/device_feature/include" \
    -I"$SCRIPT_DIR/../../components/device_event/include" \
    "$SCRIPT_DIR/test_device_feature.c" \
    "$SCRIPT_DIR/../../components/device_feature/feature_registry.c" \
    -o "$BUILD_DIR/test_device_feature"
"$BUILD_DIR/test_device_feature"
