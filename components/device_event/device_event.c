/*
 * device_event — event TX pipeline (docs §32..#35).
 *
 * Priority queue with coalescing telemetry. Worker task sends events
 * in priority order via ble_peripheral_notify().
 */
#include "device_event.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "gateway_protocol.h"

static const char *TAG = "device_event";

/* ------------------------------------------------------------------ *
 * Constants
 * ------------------------------------------------------------------ */

#define EVENT_QUEUE_DEPTH    16
#define EVENT_TASK_STACK     4096
#define EVENT_TASK_PRIORITY  3
#define EVENT_NAME_MAX_LEN   32
#define TELEMETRY_SLOTS      8

/* ------------------------------------------------------------------ *
 * Internal event slot
 * ------------------------------------------------------------------ */

typedef struct {
    char event_name[EVENT_NAME_MAX_LEN];
    char feature_id[GW_FEATURE_ID_LEN];
    uint8_t property_id;
    bool structured_feature;
    bool structured_feature_int;
    device_event_class_t event_class;
    int int_value;
    bool bool_value;
    bool occupied;
} event_slot_t;

/* ------------------------------------------------------------------ *
 * Internal state
 * ------------------------------------------------------------------ */

static struct {
    QueueHandle_t queue;
    TaskHandle_t worker_task;

    int (*notify_fn)(const uint8_t *, size_t);
    const char *device_id;

    /* Telemetry coalescing. */
    char telemetry_names[TELEMETRY_SLOTS][EVENT_NAME_MAX_LEN];
    int telemetry_values[TELEMETRY_SLOTS];
    bool telemetry_pending[TELEMETRY_SLOTS];
    int telemetry_count;
    SemaphoreHandle_t telemetry_mutex;
} s_event;

/* ------------------------------------------------------------------ *
 * Telemetry coalescing helpers
 * ------------------------------------------------------------------ */

static int find_telemetry_slot(const char *name)
{
    for (int i = 0; i < s_event.telemetry_count; i++) {
        if (strcmp(s_event.telemetry_names[i], name) == 0) return i;
    }
    return -1;
}

static int alloc_telemetry_slot(const char *name)
{
    if (s_event.telemetry_count >= TELEMETRY_SLOTS) return -1;
    int idx = s_event.telemetry_count++;
    strlcpy(s_event.telemetry_names[idx], name,
            sizeof(s_event.telemetry_names[idx]));
    return idx;
}

/* ------------------------------------------------------------------ *
 * Event worker task
 * ------------------------------------------------------------------ */

static void event_worker(void *arg)
{
    event_slot_t slot;

    while (1) {
        if (xQueueReceive(s_event.queue, &slot, portMAX_DELAY) != pdTRUE)
            continue;

        if (slot.event_class == DEVICE_EVENT_TELEMETRY &&
            s_event.telemetry_mutex != NULL &&
            xSemaphoreTake(s_event.telemetry_mutex, portMAX_DELAY) == pdTRUE) {
            int idx = find_telemetry_slot(slot.event_name);
            if (idx >= 0) {
                slot.int_value = s_event.telemetry_values[idx];
                s_event.telemetry_pending[idx] = false;
            }
            xSemaphoreGive(s_event.telemetry_mutex);
        }

        /* Build and encode event message. */
        gw_message_t msg;
        if (slot.structured_feature) {
            if (slot.structured_feature_int) {
                gw_build_feature_event_int(&msg, s_event.device_id,
                                           slot.feature_id, slot.property_id,
                                           slot.int_value);
            } else {
                gw_build_feature_event_bool(&msg, s_event.device_id,
                                            slot.feature_id, slot.property_id,
                                            slot.bool_value);
            }
        } else {
            gw_build_event(&msg, s_event.device_id, slot.event_name,
                           slot.int_value, slot.bool_value);
        }

        uint8_t buf[GW_MSG_MAX_LEN];
        int encoded = gw_message_encode(&msg, buf, sizeof(buf));
        if (encoded <= 0) {
            ESP_LOGE(TAG, "event encode failed: %d", encoded);
            continue;
        }

        if (s_event.notify_fn) {
            int rc = s_event.notify_fn(buf, (size_t)encoded);
            if (rc != 0) {
                ESP_LOGW(TAG, "event notify failed: %d", rc);
            }
        }
    }
}

/* ------------------------------------------------------------------ *
 * Public API
 * ------------------------------------------------------------------ */

int device_event_init(int (*notify_fn)(const uint8_t *, size_t),
                      const char *device_id)
{
    memset(&s_event, 0, sizeof(s_event));
    s_event.notify_fn = notify_fn;
    s_event.device_id = device_id;

    s_event.queue = xQueueCreate(EVENT_QUEUE_DEPTH, sizeof(event_slot_t));
    if (s_event.queue == NULL) return -1;
    s_event.telemetry_mutex = xSemaphoreCreateMutex();
    if (s_event.telemetry_mutex == NULL) return -1;

    xTaskCreate(event_worker, "event_worker", EVENT_TASK_STACK,
                NULL, EVENT_TASK_PRIORITY, &s_event.worker_task);

    ESP_LOGI(TAG, "initialized");
    return 0;
}

int device_event_publish(const char *event_name,
                         device_event_class_t event_class,
                         int int_value, bool bool_value)
{
    if (event_name == NULL) return -1;
    if (s_event.queue == NULL) return -1;

    /* Telemetry coalescing (doc §33): replace pending telemetry
     * with same name instead of enqueueing. */
    if (event_class == DEVICE_EVENT_TELEMETRY) {
        if (xSemaphoreTake(s_event.telemetry_mutex, pdMS_TO_TICKS(100)) !=
            pdTRUE) {
            return -1;
        }
        int idx = find_telemetry_slot(event_name);
        if (idx >= 0 && s_event.telemetry_pending[idx]) {
            s_event.telemetry_values[idx] = int_value;
            xSemaphoreGive(s_event.telemetry_mutex);
            return 0; /* coalesced */
        }
        if (idx < 0) idx = alloc_telemetry_slot(event_name);
        if (idx >= 0) {
            s_event.telemetry_pending[idx] = true;
            s_event.telemetry_values[idx] = int_value;
        }
        xSemaphoreGive(s_event.telemetry_mutex);
    }

    event_slot_t slot = {
        .event_class = event_class,
        .int_value = int_value,
        .bool_value = bool_value,
        .occupied = true,
    };
    strlcpy(slot.event_name, event_name, sizeof(slot.event_name));

    if (xQueueSend(s_event.queue, &slot, 0) != pdTRUE) {
        if (event_class == DEVICE_EVENT_TELEMETRY &&
            xSemaphoreTake(s_event.telemetry_mutex,
                           pdMS_TO_TICKS(100)) == pdTRUE) {
            int idx = find_telemetry_slot(event_name);
            if (idx >= 0) s_event.telemetry_pending[idx] = false;
            xSemaphoreGive(s_event.telemetry_mutex);
        }
        ESP_LOGW(TAG, "event queue full, dropping: %s", event_name);
        return -1;
    }
    return 0;
}

int device_event_publish_state(const char *event_name, int value)
{
    return device_event_publish(event_name, DEVICE_EVENT_STATE, value, false);
}

int device_event_publish_feature_bool(const char *feature_id,
                                      uint8_t property_id, bool value)
{
    if (feature_id == NULL || feature_id[0] == '\0' ||
        strnlen(feature_id, GW_FEATURE_ID_LEN) >= GW_FEATURE_ID_LEN ||
        s_event.queue == NULL) return -1;

    event_slot_t slot = {
        .property_id = property_id,
        .structured_feature = true,
        .event_class = DEVICE_EVENT_STATE,
        .bool_value = value,
        .occupied = true,
    };
    strlcpy(slot.event_name, GW_EVENT_FEATURE_STATE,
            sizeof(slot.event_name));
    strlcpy(slot.feature_id, feature_id, sizeof(slot.feature_id));
    if (xQueueSend(s_event.queue, &slot, 0) != pdTRUE) {
        ESP_LOGW(TAG, "feature event queue full: %s", feature_id);
        return -1;
    }
    return 0;
}

int device_event_publish_feature_int(const char *feature_id,
                                     uint8_t property_id, int32_t value)
{
    if (feature_id == NULL || feature_id[0] == '\0' ||
        strnlen(feature_id, GW_FEATURE_ID_LEN) >= GW_FEATURE_ID_LEN ||
        s_event.queue == NULL) return -1;

    event_slot_t slot = {
        .property_id = property_id,
        .structured_feature = true,
        .structured_feature_int = true,
        .event_class = DEVICE_EVENT_STATE,
        .int_value = value,
        .occupied = true,
    };
    strlcpy(slot.event_name, GW_EVENT_FEATURE_STATE,
            sizeof(slot.event_name));
    strlcpy(slot.feature_id, feature_id, sizeof(slot.feature_id));
    if (xQueueSend(s_event.queue, &slot, 0) != pdTRUE) {
        ESP_LOGW(TAG, "feature INT event queue full: %s", feature_id);
        return -1;
    }
    return 0;
}

int device_event_publish_telemetry(const char *event_name, int value)
{
    return device_event_publish(event_name, DEVICE_EVENT_TELEMETRY, value, false);
}

int device_event_flush(void)
{
    if (s_event.queue == NULL) return -1;
    xQueueReset(s_event.queue);
    if (xSemaphoreTake(s_event.telemetry_mutex, portMAX_DELAY) == pdTRUE) {
        memset(s_event.telemetry_pending, 0,
               sizeof(s_event.telemetry_pending));
        xSemaphoreGive(s_event.telemetry_mutex);
    }
    ESP_LOGI(TAG, "flushed");
    return 0;
}
