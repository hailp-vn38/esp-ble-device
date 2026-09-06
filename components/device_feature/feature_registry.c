#include "device_feature.h"

#include <ctype.h>
#include <string.h>

#include "device_event.h"

static device_feature_descriptor_t s_features[DEVICE_FEATURE_MAX_PER_DEVICE];
static size_t s_feature_count;

static bool valid_id(const char *id, size_t capacity)
{
    if (id == NULL || id[0] == '\0' || strnlen(id, capacity) >= capacity)
        return false;
    for (size_t i = 0; id[i] != '\0'; i++) {
        unsigned char c = (unsigned char)id[i];
        if (!isalnum(c) && c != '_' && c != '-' && c != '.') return false;
    }
    return true;
}

static bool valid_optional_command(const char *command)
{
    if (command == NULL || command[0] == '\0') return true;
    return valid_id(command, GW_MSG_COMMAND_LEN);
}

static bool valid_text(const char *text, size_t capacity)
{
    return text != NULL && strnlen(text, capacity) < capacity;
}

static bool valid_property(uint8_t property_id)
{
    return property_id > GW_PROP_NONE && property_id <= GW_PROP_VALUE;
}

static bool valid_type_property(gw_feature_type_t type, uint8_t property_id,
                                device_feature_value_type_t value_type)
{
    switch (type) {
    case GW_FEATURE_GENERIC_RELAY:
    case GW_FEATURE_ON_OFF_PLUGIN_UNIT:
    case GW_FEATURE_ON_OFF_LIGHT:
        return property_id == GW_PROP_ON_OFF &&
               value_type == DEVICE_FEATURE_VALUE_BOOL;
    case GW_FEATURE_DIMMABLE_LIGHT:
        return property_id == GW_PROP_LEVEL &&
               value_type == DEVICE_FEATURE_VALUE_INT;
    case GW_FEATURE_FAN:
        return property_id == GW_PROP_PERCENT_SETTING &&
               value_type == DEVICE_FEATURE_VALUE_INT;
    case GW_FEATURE_TEMPERATURE_SENSOR:
        return property_id == GW_PROP_TEMPERATURE &&
               value_type == DEVICE_FEATURE_VALUE_INT;
    case GW_FEATURE_HUMIDITY_SENSOR:
        return property_id == GW_PROP_HUMIDITY &&
               value_type == DEVICE_FEATURE_VALUE_INT;
    case GW_FEATURE_CONTACT_SENSOR:
        return property_id == GW_PROP_CONTACT &&
               value_type == DEVICE_FEATURE_VALUE_BOOL;
    case GW_FEATURE_GENERIC_VALUE:
        return property_id == GW_PROP_VALUE &&
               value_type == DEVICE_FEATURE_VALUE_INT;
    default:
        return false;
    }
}

int device_feature_init(void)
{
    memset(s_features, 0, sizeof(s_features));
    s_feature_count = 0;
    return 0;
}

int device_feature_register(const device_feature_config_t *config)
{
    if (config == NULL || s_feature_count >= DEVICE_FEATURE_MAX_PER_DEVICE ||
        !valid_id(config->feature_id, GW_FEATURE_ID_LEN) ||
        !valid_text(config->title, GW_MSG_CAP_LABEL_LEN) ||
        !valid_text(config->unit, GW_MSG_CAP_UNIT_LEN) ||
        !valid_optional_command(config->write_tool) ||
        !valid_property(config->property_id) ||
        config->schema_version == 0 ||
        device_feature_find(config->feature_id) != NULL) {
        return -1;
    }

    if (config->value_type != DEVICE_FEATURE_VALUE_BOOL &&
        config->value_type != DEVICE_FEATURE_VALUE_INT) return -1;
    if (config->value_type == DEVICE_FEATURE_VALUE_BOOL &&
        config->decimals != 0) return -1;
    if (config->value_type == DEVICE_FEATURE_VALUE_INT &&
        config->decimals > 3) return -1;
    if (config->reader.read_bool == NULL &&
        config->value_type == DEVICE_FEATURE_VALUE_BOOL) return -1;
    if (config->reader.read_int == NULL &&
        config->value_type == DEVICE_FEATURE_VALUE_INT) return -1;
    if (!valid_type_property(config->type, config->property_id,
                             config->value_type)) return -1;

    device_feature_descriptor_t *feature = &s_features[s_feature_count++];
    memset(feature, 0, sizeof(*feature));
    strlcpy(feature->feature_id, config->feature_id,
            sizeof(feature->feature_id));
    strlcpy(feature->title, config->title, sizeof(feature->title));
    strlcpy(feature->unit, config->unit, sizeof(feature->unit));
    feature->type = config->type;
    feature->schema_version = config->schema_version;
    feature->flags = config->flags;
    feature->decimals = config->decimals;
    feature->property.id = config->property_id;
    feature->property.value_type = config->value_type;
    feature->property.readable = true;
    feature->property.writable = config->write_tool != NULL &&
                                 config->write_tool[0] != '\0';
    if (feature->property.writable) {
        strlcpy(feature->write_tool, config->write_tool,
                sizeof(feature->write_tool));
    }
    feature->reader = config->reader;
    feature->read_bool = config->value_type == DEVICE_FEATURE_VALUE_BOOL ?
                         config->reader.read_bool : NULL;
    feature->context = config->context;
    return 0;
}

int device_feature_register_on_off_light(
    const device_feature_on_off_light_config_t *config)
{
    if (config == NULL) return -1;
    device_feature_config_t generic = {
        .feature_id = config->feature_id,
        .title = config->feature_id,
        .unit = "",
        .type = GW_FEATURE_ON_OFF_LIGHT,
        .schema_version = 1,
        .property_id = GW_PROP_ON_OFF,
        .value_type = DEVICE_FEATURE_VALUE_BOOL,
        .write_tool = config->set_command,
        .reader = {.read_bool = config->read_on_off},
        .context = config->context,
    };
    return device_feature_register(&generic);
}

size_t device_feature_count(void) { return s_feature_count; }

const device_feature_descriptor_t *device_feature_get(size_t index)
{
    return index < s_feature_count ? &s_features[index] : NULL;
}

const device_feature_descriptor_t *device_feature_find(const char *feature_id)
{
    if (feature_id == NULL) return NULL;
    for (size_t i = 0; i < s_feature_count; i++) {
        if (strcmp(s_features[i].feature_id, feature_id) == 0)
            return &s_features[i];
    }
    return NULL;
}

int device_feature_read(const char *feature_id, uint8_t property_id,
                        device_feature_value_t *out)
{
    const device_feature_descriptor_t *feature = device_feature_find(feature_id);
    if (feature == NULL || out == NULL || property_id != feature->property.id ||
        !feature->property.readable) return -1;
    out->type = feature->property.value_type;
    if (out->type == DEVICE_FEATURE_VALUE_BOOL && feature->reader.read_bool) {
        return feature->reader.read_bool(feature->context,
                                          &out->value.bool_value);
    }
    if (out->type == DEVICE_FEATURE_VALUE_INT && feature->reader.read_int) {
        return feature->reader.read_int(feature->context,
                                        &out->value.int_value);
    }
    return -1;
}

int device_feature_read_bool(const char *feature_id, uint8_t property_id,
                             bool *out_value)
{
    if (out_value == NULL) return -1;
    device_feature_value_t value;
    int rc = device_feature_read(feature_id, property_id, &value);
    if (rc != 0 || value.type != DEVICE_FEATURE_VALUE_BOOL) return -1;
    *out_value = value.value.bool_value;
    return 0;
}

int device_feature_read_int(const char *feature_id, uint8_t property_id,
                            int32_t *out_value)
{
    if (out_value == NULL) return -1;
    device_feature_value_t value;
    int rc = device_feature_read(feature_id, property_id, &value);
    if (rc != 0 || value.type != DEVICE_FEATURE_VALUE_INT) return -1;
    *out_value = value.value.int_value;
    return 0;
}

int device_feature_publish_bool(const char *feature_id, uint8_t property_id,
                                bool value)
{
    const device_feature_descriptor_t *feature = device_feature_find(feature_id);
    if (feature == NULL || property_id != feature->property.id ||
        feature->property.value_type != DEVICE_FEATURE_VALUE_BOOL) return -1;
    return device_event_publish_feature_bool(feature_id, property_id, value);
}

int device_feature_publish_int(const char *feature_id, uint8_t property_id,
                               int32_t value)
{
    const device_feature_descriptor_t *feature = device_feature_find(feature_id);
    if (feature == NULL || property_id != feature->property.id ||
        feature->property.value_type != DEVICE_FEATURE_VALUE_INT) return -1;
    return device_event_publish_feature_int(feature_id, property_id, value);
}
