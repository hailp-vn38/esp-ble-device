#ifndef DEMO_SENSOR_H
#define DEMO_SENSOR_H
#include <stdint.h>
typedef struct { int32_t temperature_raw; int32_t humidity_raw; } demo_environment_sample_t;
typedef void (*demo_sensor_cb_t)(const demo_environment_sample_t *sample, void *context);
int demo_sensor_start(demo_sensor_cb_t callback, void *context);
#endif
