/*
 * device_event — event TX pipeline (docs §32..#35).
 *
 * Pipeline:
 *   driver -> device_event_publish()
 *     -> event worker task
 *       -> gw_build_event() + gw_message_encode()
 *       -> ble_peripheral_notify()
 *
 * Priority ordering (highest first):
 *   ACK > CRITICAL_EVENT > EDGE_EVENT > STATE_EVENT > TELEMETRY
 *
 * Backpressure:
 *   ACK:         never dropped, reserves capacity
 *   CRITICAL:    try preserve
 *   TELEMETRY:   coalesce to latest value
 */
#ifndef DEVICE_EVENT_H
#define DEVICE_EVENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gateway_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ *
 * Event classes (docs §33, contract §38)
 * ------------------------------------------------------------------ */

typedef enum {
    DEVICE_EVENT_ACK         = 0,  /* highest */
    DEVICE_EVENT_CRITICAL    = 1,
    DEVICE_EVENT_EDGE        = 2,
    DEVICE_EVENT_STATE       = 3,
    DEVICE_EVENT_TELEMETRY   = 4,  /* lowest */
} device_event_class_t;

/* ------------------------------------------------------------------ *
 * Public API
 * ------------------------------------------------------------------ */

/*
 * Initialize the event pipeline.
 * notify_fn: sends encoded CBOR via ABF2 (provided by ble_peripheral).
 * device_id: logical identity for all events (non-owning pointer).
 * Returns 0 on success.
 */
int device_event_init(int (*notify_fn)(const uint8_t *, size_t),
                      const char *device_id);

/*
 * Publish an event to the event pipeline.
 * event_name: e.g. "button_pressed", "state_changed"
 * event_class: determines priority ordering.
 * int_value, bool_value: payload fields per protocol.
 */
int device_event_publish(const char *event_name,
                         device_event_class_t event_class,
                         int int_value, bool bool_value);

/*
 * Convenience: publish a state-changed event.
 */
int device_event_publish_state(const char *event_name, int value);

/* Publish a protocol-v4 structured boolean feature state event. */
int device_event_publish_feature_bool(const char *feature_id,
                                      uint8_t property_id, bool value);
int device_event_publish_feature_int(const char *feature_id,
                                     uint8_t property_id, int32_t value);

/*
 * Convenience: publish a telemetry event.
 * Coalesces: if telemetry with same name is already pending,
 * it is replaced (not enqueued again).
 */
int device_event_publish_telemetry(const char *event_name, int value);

/*
 * Flush all pending events. Called during stop sequence.
 */
int device_event_flush(void);

#ifdef __cplusplus
}
#endif

#endif /* DEVICE_EVENT_H */
