/*
 * ble_peripheral — NimBLE Peripheral / GATT Server transport.
 *
 * Responsibilities:
 *   NimBLE init, GAP Peripheral role, advertising, GATT server (ABF0/ABF1/ABF2),
 *   security (bonding, LE SC, NO_IO), connection state, CCCD/MTU tracking,
 *   bounded RX callback, notify TX, re-advertise after disconnect.
 *
 * Does NOT contain product/relay/sensor business logic.
 * Does NOT decode CBOR — caller enqueues raw bytes from the bounded callback.
 *
 * State machine:
 *   STOPPED -> INITIALIZED -> ADVERTISING -> CONNECTED -> SECURING
 *     -> WAIT_CCCD -> READY -> ADVERTISING (on disconnect)
 */
#ifndef BLE_PERIPHERAL_H
#define BLE_PERIPHERAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ *
 * States (mirror docs §16, contract §27)
 * ------------------------------------------------------------------ */

typedef enum {
    BLE_PERIPH_STOPPED = 0,
    BLE_PERIPH_INITIALIZED,
    BLE_PERIPH_ADVERTISING,
    BLE_PERIPH_CONNECTED,
    BLE_PERIPH_SECURING,
    BLE_PERIPH_WAIT_CCCD,
    BLE_PERIPH_READY,
} ble_peripheral_state_t;

/* ------------------------------------------------------------------ *
 * Configuration
 * ------------------------------------------------------------------ */

typedef struct {
    const char *device_name;          /* BLE local name (or NULL for default) */
    uint16_t preferred_mtu;           /* 0 -> 256 */
    bool require_bonding;             /* true -> persistent bonds via NVS */
    uint32_t adv_interval_ms;         /* 0 -> 100 ms */
} ble_peripheral_config_t;

/* Default config macro; ble_peripheral_init() copies the configuration. */
#define BLE_PERIPHERAL_CONFIG_DEFAULT \
    {                                 \
        .device_name = NULL,          \
        .preferred_mtu = 0,           \
        .require_bonding = true,      \
        .adv_interval_ms = 0,         \
    }

/* ------------------------------------------------------------------ *
 * Callbacks — called from a bounded context; caller must enqueue
 * into a FreeRTOS queue; heavy work goes to a separate task.
 * ------------------------------------------------------------------ */

/*
 * RX callback: raw ATT write payload from ABF1 (Write Without Response).
 * Called from NimBLE host context — must return quickly.
 * data is valid only during the call; caller must copy.
 */
typedef void (*ble_peripheral_rx_cb_t)(const uint8_t *data, size_t len);

/*
 * State change callback: notified on every transition.
 * Safe to use for LED indication, status logging, etc.
 */
typedef void (*ble_peripheral_state_cb_t)(ble_peripheral_state_t state);

/* ------------------------------------------------------------------ *
 * Public API (docs §15)
 * ------------------------------------------------------------------ */

/*
 * Initialize NimBLE, register GATT service, configure security.
 * Does NOT start advertising or FreeRTOS tasks — call ble_peripheral_start().
 * The configuration is copied; it only needs to remain valid during the call.
 */
int ble_peripheral_init(const ble_peripheral_config_t *config,
                        ble_peripheral_rx_cb_t rx_cb,
                        ble_peripheral_state_cb_t state_cb);

/*
 * Start NimBLE host task + advertising.
 * Safe to call from app_main after product init is complete.
 */
int ble_peripheral_start(void);

/*
 * Stop advertising, disconnect if connected, stop host task.
 */
int ble_peripheral_stop(void);

/*
 * Send data via STATUS characteristic notify (ABF2).
 * Encoded CBOR payload, bounded by min(protocol_max, mtu - 3).
 * Returns 0 on submission; error if not READY or queue full.
 * May return success even if the packet is dropped internally
 * due to congestion (best-effort notify, contract §91).
 */
int ble_peripheral_notify(const uint8_t *data, size_t len);

typedef struct {
    const uint8_t *data;
    size_t len;
} ble_peripheral_notify_item_t;

/* Enqueue a contiguous notification transaction. Other producers cannot
 * interleave messages in the batch. The largest supported batch is the
 * notify queue depth; use the ordered sequence API for larger transactions. */
int ble_peripheral_notify_batch(const ble_peripheral_notify_item_t *items,
                                size_t count);

/* Ordered notification sequence for transactions larger than the queue.
 * begin() acquires the submission mutex. Each send() validates the current
 * READY/CCCD/MTU state, waits for one queue slot for at most timeout, and
 * copies the frame into the queue. end() releases the mutex. Producers using
 * ble_peripheral_notify() or notify_batch() cannot interleave frames while a
 * sequence is active. */
int ble_peripheral_notify_sequence_begin(void);
int ble_peripheral_notify_sequence_send(const uint8_t *data,
                                        size_t len,
                                        TickType_t timeout);
void ble_peripheral_notify_sequence_abort(void);
void ble_peripheral_notify_sequence_end(void);

/* ------------------------------------------------------------------ *
 * Status queries
 * ------------------------------------------------------------------ */

bool ble_peripheral_is_connected(void);
bool ble_peripheral_is_ready(void);
uint16_t ble_peripheral_get_mtu(void);
ble_peripheral_state_t ble_peripheral_get_state(void);

/*
 * Delete all bond records from NVS. Called during factory reset.
 * Must NOT be called from a NimBLE callback context.
 */
int ble_peripheral_clear_bonds(void);

/* ------------------------------------------------------------------ *
 * Diagnostics (spec §24)
 * ------------------------------------------------------------------ */

typedef struct {
    uint32_t rx_queued;
    uint32_t rx_dropped;
    uint32_t notify_queued;
    uint32_t notify_dropped;
    uint32_t notify_batch_rejected;
    uint32_t repeat_pairing_count;
    uint32_t notify_sequence_started;
    uint32_t notify_sequence_completed;
    uint32_t notify_sequence_aborted;
    uint32_t notify_sequence_timeout;
    uint32_t notify_oversize;
} ble_peripheral_diag_t;

/* Get current diagnostic counters. */
void ble_peripheral_get_diag(ble_peripheral_diag_t *out_diag);

#ifdef __cplusplus
}
#endif

#endif /* BLE_PERIPHERAL_H */
