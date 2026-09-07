#ifndef DEMO_SENSOR_H
#define DEMO_SENSOR_H
#include <stdbool.h>
#include <stdint.h>
typedef struct { int32_t temperature_raw; int32_t humidity_raw; } demo_environment_sample_t;
typedef void (*demo_sensor_cb_t)(const demo_environment_sample_t *sample, void *context);
typedef uint32_t (*demo_sensor_interval_fn)(void *context);
typedef bool (*demo_sensor_enabled_fn)(void *context);
int demo_sensor_start(demo_sensor_cb_t callback, void *context,
                      demo_sensor_interval_fn interval_fn,
                      demo_sensor_enabled_fn enabled_fn,
                      void *config_context);
#endif
