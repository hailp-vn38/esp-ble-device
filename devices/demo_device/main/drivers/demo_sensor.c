#include "demo_sensor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
static demo_sensor_cb_t s_callback; static void *s_context;
static void demo_sensor_task(void *arg) {
    (void)arg; static const int32_t temp[] = {250,251,252,253,252,251}; static const int32_t hum[] = {55,56,57,58,57,56}; static size_t i;
    for (;;) { demo_environment_sample_t sample={temp[i],hum[i]}; if (s_callback) s_callback(&sample,s_context); i=(i+1)%6; vTaskDelay(pdMS_TO_TICKS(3000)); }
}
int demo_sensor_start(demo_sensor_cb_t callback, void *context) { s_callback=callback; s_context=context; return xTaskCreate(demo_sensor_task,"demo_sensor",2048,NULL,2,NULL)==pdPASS?0:-1; }
