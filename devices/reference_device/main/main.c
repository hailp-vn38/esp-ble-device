/*
 * reference_device — Golden Peripheral (docs §53..#57, contract §122..#126).
 *
 * Boot: set profile -> device_app_start().
 * All lifecycle handled by device_app composition root.
 */
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "device_app.h"
#include "reference_product.h"

static const char *TAG = "reference_device";

void app_main(void)
{
    ESP_LOGI(TAG, "reference_device booting");

    device_app_set_profile(reference_product_profile());

    device_app_result_t rc = device_app_start();
    if (rc != DEVICE_APP_OK) {
        ESP_LOGE(TAG, "device_app_start failed: %d", rc);
        return;
    }

    ESP_LOGI(TAG, "boot complete, advertising");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
