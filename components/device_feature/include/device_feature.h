#ifndef DEVICE_FEATURE_H
#define DEVICE_FEATURE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gateway_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DEVICE_FEATURE_MAX_PER_DEVICE 8

typedef int (*device_feature_read_bool_fn)(void *context, bool *out_value);

typedef struct {
    uint8_t id;
    uint8_t value_type;
    bool readable;
    bool writable;
} device_feature_property_t;

typedef struct {
    char feature_id[GW_FEATURE_ID_LEN];
    gw_feature_type_t type;
    uint16_t schema_version;
    uint16_t flags;
    device_feature_property_t property;
    char write_tool[GW_MSG_COMMAND_LEN];
    device_feature_read_bool_fn read_bool;
    void *context;
} device_feature_descriptor_t;

typedef struct {
    const char *feature_id;
    const char *set_command;
    device_feature_read_bool_fn read_on_off;
    void *context;
} device_feature_on_off_light_config_t;

int device_feature_init(void);
int device_feature_register_on_off_light(
    const device_feature_on_off_light_config_t *config);
size_t device_feature_count(void);
const device_feature_descriptor_t *device_feature_get(size_t index);
const device_feature_descriptor_t *device_feature_find(const char *feature_id);
int device_feature_read_bool(const char *feature_id, uint8_t property_id,
                             bool *out_value);
int device_feature_publish_bool(const char *feature_id, uint8_t property_id,
                                bool value);

#ifdef __cplusplus
}
#endif

#endif
