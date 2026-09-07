#!/bin/sh
# Host unit tests for Settings v2 Transaction Commands (Phase 3).
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
OUT="$DIR/build"
mkdir -p "$OUT"

# Create mock esp_err.h for host testing
cat > "$OUT/esp_err.h" << 'MOCK_EOF'
#ifndef ESP_ERR_H
#define ESP_ERR_H

typedef int esp_err_t;

#define ESP_OK 0
#define ESP_ERR_INVALID_ARG -1
#define ESP_ERR_INVALID_STATE -2
#define ESP_ERR_NO_MEM -3
#define ESP_ERR_NOT_FOUND -4
#define ESP_ERR_INVALID_SIZE -5
#define ESP_ERR_INVALID_VERSION -6
#define ESP_ERR_NVS_NO_FREE_PAGES -100
#define ESP_ERR_NVS_NEW_VERSION_FOUND -101
#define ESP_ERR_NVS_NOT_FOUND -102

const char *esp_err_to_name(esp_err_t code);

#endif /* ESP_ERR_H */
MOCK_EOF

# Create mock NVS implementation for host testing
cat > "$OUT/mock_nvs.c" << 'MOCK_EOF'
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "esp_err.h"

typedef int nvs_handle_t;

#define NVS_READONLY 0
#define NVS_READWRITE 1

const char *esp_err_to_name(esp_err_t code) {
    (void)code;
    return "UNKNOWN";
}

static struct {
    uint8_t data[4096];
    size_t size;
    int handle_counter;
} s_nvs_mock = { .size = 0, .handle_counter = 1 };

int nvs_open(const char *name, int mode, nvs_handle_t *handle) {
    (void)name;
    if (mode == NVS_READONLY && s_nvs_mock.size == 0) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    *handle = s_nvs_mock.handle_counter++;
    return 0;
}

int nvs_close(nvs_handle_t handle) {
    (void)handle;
    return 0;
}

int nvs_get_blob(nvs_handle_t handle, const char *key, void *out_value, size_t *length) {
    (void)handle;
    (void)key;
    if (s_nvs_mock.size == 0) return ESP_ERR_NVS_NOT_FOUND;
    if (*length < s_nvs_mock.size) return -1;
    memcpy(out_value, s_nvs_mock.data, s_nvs_mock.size);
    *length = s_nvs_mock.size;
    return 0;
}

int nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t length) {
    (void)handle;
    (void)key;
    if (length > sizeof(s_nvs_mock.data)) return -1;
    memcpy(s_nvs_mock.data, value, length);
    s_nvs_mock.size = length;
    return 0;
}

int nvs_commit(nvs_handle_t handle) {
    (void)handle;
    return 0;
}
MOCK_EOF

# Create mock nvs_flash.h
cat > "$OUT/nvs_flash.h" << 'MOCK_EOF'
#ifndef NVS_FLASH_H
#define NVS_FLASH_H
#include "esp_err.h"
int nvs_flash_init(void);
int nvs_flash_erase(void);
#endif
MOCK_EOF

cat > "$OUT/mock_nvs_flash.c" << 'MOCK_EOF'
#include "nvs_flash.h"
int nvs_flash_init(void) { return 0; }
int nvs_flash_erase(void) { return 0; }
MOCK_EOF

# Create mock nvs.h
cat > "$OUT/nvs.h" << 'MOCK_EOF'
#ifndef NVS_H
#define NVS_H
#include "esp_err.h"
typedef int nvs_handle_t;
#define NVS_READONLY 0
#define NVS_READWRITE 1
int nvs_open(const char *name, int mode, nvs_handle_t *handle);
int nvs_close(nvs_handle_t handle);
int nvs_get_blob(nvs_handle_t handle, const char *key, void *out_value, size_t *length);
int nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t length);
int nvs_commit(nvs_handle_t handle);
#endif
MOCK_EOF

# Create mock device_app (Phase 4: device_app_schedule_restart)
cat > "$OUT/mock_device_app.c" << 'MOCK_EOF'
#include <stdint.h>
typedef int esp_err_t;
#define ESP_OK 0
esp_err_t device_app_schedule_restart(uint32_t delay_ms) {
    (void)delay_ms;
    return ESP_OK;
}
MOCK_EOF

# Create mock esp_log.h
cat > "$OUT/esp_log.h" << 'MOCK_EOF'
#ifndef ESP_LOG_H
#define ESP_LOG_H
#include <stdio.h>
#define ESP_LOGI(tag, fmt, ...) printf("I [%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) printf("W [%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGE(tag, fmt, ...) printf("E [%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) printf("D [%s] " fmt "\n", tag, ##__VA_ARGS__)
#endif
MOCK_EOF

# Compile
cc -std=c11 -Wall -Wextra -Werror -O2 \
   -DGW_HOST_TEST \
   -I"$OUT" \
   -I"$DIR/../../components/gateway_protocol/include" \
   -I"$DIR/../../components/device_settings/include" \
   -I"$DIR/../../components/device_command/include" \
   -I"$DIR/../../components/device_feature/include" \
   -I"$DIR/../../components/ble_peripheral/include" \
   "$OUT/mock_nvs.c" \
   "$OUT/mock_nvs_flash.c" \
   "$OUT/mock_device_app.c" \
   "$DIR/../../components/gateway_protocol/gateway_protocol.c" \
   "$DIR/../../components/gateway_protocol/gateway_settings.c" \
   "$DIR/../../components/device_settings/device_settings_registry.c" \
   "$DIR/../../components/device_settings/device_settings_transaction.c" \
   "$DIR/../../components/device_settings/device_settings_persistence.c" \
   "$DIR/../../components/device_settings/device_settings_restart.c" \
   "$DIR/test_settings_tx.c" \
   -o "$OUT/test_settings_tx"

"$OUT/test_settings_tx"
