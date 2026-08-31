#include "device_feature.h"

#include <ctype.h>
#include <string.h>

#include "device_event.h"

static device_feature_descriptor_t s_features[DEVICE_FEATURE_MAX_PER_DEVICE];
static size_t s_feature_count;

static bool valid_id(const char *id)
{
    if (id == NULL || id[0] == '\0' ||
        strnlen(id, GW_FEATURE_ID_LEN) >= GW_FEATURE_ID_LEN) return false;
    for (size_t i = 0; id[i] != '\0'; i++) {
        unsigned char c = (unsigned char)id[i];
        if (!isalnum(c) && c != '_' && c != '-' && c != '.') return false;
    }
    return true;
}

static bool valid_command(const char *command)
{
    if (command == NULL || command[0] == '\0' ||
        strnlen(command, GW_MSG_COMMAND_LEN) >= GW_MSG_COMMAND_LEN) return false;
    for (size_t i = 0; command[i] != '\0'; i++) {
        unsigned char c = (unsigned char)command[i];
        if (!isalnum(c) && c != '_' && c != '-' && c != '.') return false;
    }
    return true;
}

int device_feature_init(void)
{
    memset(s_features, 0, sizeof(s_features));
    s_feature_count = 0;
    return 0;
}

int device_feature_register_on_off_light(
    const device_feature_on_off_light_config_t *config)
{
    if (config == NULL || !valid_id(config->feature_id) ||
        !valid_command(config->set_command) ||
        config->read_on_off == NULL ||
        s_feature_count >= DEVICE_FEATURE_MAX_PER_DEVICE ||
        device_feature_find(config->feature_id) != NULL) {
        return -1;
    }

    device_feature_descriptor_t *feature = &s_features[s_feature_count++];
    strlcpy(feature->feature_id, config->feature_id,
            sizeof(feature->feature_id));
    feature->type = GW_FEATURE_ON_OFF_LIGHT;
    feature->schema_version = 1;
    feature->property.id = GW_PROP_ON_OFF;
    feature->property.value_type = 1; /* BOOL */
    feature->property.readable = true;
    feature->property.writable = true;
    strlcpy(feature->write_tool, config->set_command,
            sizeof(feature->write_tool));
    feature->read_bool = config->read_on_off;
    feature->context = config->context;
    return 0;
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

int device_feature_read_bool(const char *feature_id, uint8_t property_id,
                             bool *out_value)
{
    const device_feature_descriptor_t *feature = device_feature_find(feature_id);
    if (feature == NULL || out_value == NULL || property_id != GW_PROP_ON_OFF ||
        !feature->property.readable || feature->read_bool == NULL) return -1;
    return feature->read_bool(feature->context, out_value);
}

int device_feature_publish_bool(const char *feature_id, uint8_t property_id,
                                bool value)
{
    const device_feature_descriptor_t *feature = device_feature_find(feature_id);
    if (feature == NULL || property_id != GW_PROP_ON_OFF ||
        !feature->property.readable) return -1;
    return device_event_publish_feature_bool(feature_id, property_id, value);
}
