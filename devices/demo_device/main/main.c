#include "device_app.h"
#include "demo_product.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "demo_device";

void app_main(void)
{
    ESP_LOGI(TAG, "demo_device booting");
    device_app_set_profile(demo_product_profile());
    if (device_app_start() != DEVICE_APP_OK) {
        ESP_LOGE(TAG, "device_app_start failed");
        return;
    }
    ESP_LOGI(TAG, "GW-DEMO advertising");
    while (true) vTaskDelay(pdMS_TO_TICKS(10000));
}
