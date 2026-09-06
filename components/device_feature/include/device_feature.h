#ifndef DEVICE_FEATURE_H
#define DEVICE_FEATURE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gateway_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DEVICE_FEATURE_MAX_PER_DEVICE 12

typedef enum {
    DEVICE_FEATURE_VALUE_NONE = 0,
    DEVICE_FEATURE_VALUE_BOOL = 1,
    DEVICE_FEATURE_VALUE_INT = 2,
} device_feature_value_type_t;

typedef int (*device_feature_read_bool_fn)(void *context, bool *out_value);
typedef int (*device_feature_read_int_fn)(void *context, int32_t *out_value);

typedef union {
    device_feature_read_bool_fn read_bool;
    device_feature_read_int_fn read_int;
} device_feature_reader_t;

typedef struct {
    uint8_t id;
    device_feature_value_type_t value_type;
    bool readable;
    bool writable;
} device_feature_property_t;

typedef struct {
    device_feature_value_type_t type;
    union {
        bool bool_value;
        int32_t int_value;
    } value;
} device_feature_value_t;

typedef struct {
    char feature_id[GW_FEATURE_ID_LEN];
    char title[GW_MSG_CAP_LABEL_LEN];
    char unit[GW_MSG_CAP_UNIT_LEN];
    gw_feature_type_t type;
    uint16_t schema_version;
    uint16_t flags;
    uint8_t decimals;
    device_feature_property_t property;
    char write_tool[GW_MSG_COMMAND_LEN];
    device_feature_reader_t reader;
    /* Compatibility alias for existing BOOL product code. */
    device_feature_read_bool_fn read_bool;
    void *context;
} device_feature_descriptor_t;

typedef struct {
    const char *feature_id;
    const char *title;
    const char *unit;
    gw_feature_type_t type;
    uint16_t schema_version;
    uint16_t flags;
    uint8_t property_id;
    device_feature_value_type_t value_type;
    uint8_t decimals;
    const char *write_tool;
    device_feature_reader_t reader;
    void *context;
} device_feature_config_t;

/* Compatibility registration API for the original ON_OFF_LIGHT feature. */
typedef struct {
    const char *feature_id;
    const char *set_command;
    device_feature_read_bool_fn read_on_off;
    void *context;
} device_feature_on_off_light_config_t;

int device_feature_init(void);
int device_feature_register(const device_feature_config_t *config);
int device_feature_register_on_off_light(
    const device_feature_on_off_light_config_t *config);
size_t device_feature_count(void);
const device_feature_descriptor_t *device_feature_get(size_t index);
const device_feature_descriptor_t *device_feature_find(const char *feature_id);
int device_feature_read(const char *feature_id, uint8_t property_id,
                        device_feature_value_t *out);
int device_feature_read_bool(const char *feature_id, uint8_t property_id,
                             bool *out_value);
int device_feature_read_int(const char *feature_id, uint8_t property_id,
                            int32_t *out_value);
int device_feature_publish_bool(const char *feature_id, uint8_t property_id,
                                bool value);
int device_feature_publish_int(const char *feature_id, uint8_t property_id,
                               int32_t value);

#ifdef __cplusplus
}
#endif

#endif
