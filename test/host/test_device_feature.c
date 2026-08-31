#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "device_feature.h"

static int checks;
static int failures;
#define CHECK(expr) do { checks++; if (!(expr)) { failures++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); } } while (0)

static bool s_value;
static int read_value(void *context, bool *out_value)
{
    (void)context;
    if (out_value == NULL) return -1;
    *out_value = s_value;
    return 0;
}

int device_event_publish_feature_bool(const char *feature_id,
                                      uint8_t property_id, bool value)
{
    CHECK(strcmp(feature_id, "led_main") == 0);
    CHECK(property_id == GW_PROP_ON_OFF);
    CHECK(value == s_value);
    return 0;
}

int main(void)
{
    device_feature_on_off_light_config_t config = {
        .feature_id = "led_main",
        .set_command = "set_led",
        .read_on_off = read_value,
    };
    CHECK(device_feature_init() == 0);
    CHECK(device_feature_register_on_off_light(&config) == 0);
    CHECK(device_feature_count() == 1);

    const device_feature_descriptor_t *feature = device_feature_get(0);
    CHECK(feature != NULL);
    CHECK(strcmp(feature->feature_id, "led_main") == 0);
    CHECK(feature->type == GW_FEATURE_ON_OFF_LIGHT);
    CHECK(feature->schema_version == 1);
    CHECK(feature->property.id == GW_PROP_ON_OFF);
    CHECK(feature->property.value_type == 1);
    CHECK(feature->property.readable && feature->property.writable);
    CHECK(strcmp(feature->write_tool, "set_led") == 0);

    CHECK(device_feature_register_on_off_light(&config) != 0);
    CHECK(device_feature_find("missing") == NULL);
    s_value = true;
    bool out = false;
    CHECK(device_feature_read_bool("led_main", GW_PROP_ON_OFF, &out) == 0);
    CHECK(out);
    CHECK(device_feature_publish_bool("led_main", GW_PROP_ON_OFF, true) == 0);
    CHECK(device_feature_read_bool("led_main", GW_PROP_LEVEL, &out) != 0);

    printf("device_feature: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
