/*
 * ble_peripheral — NimBLE Peripheral / GATT Server transport.
 *
 * Implements:
 *   - NimBLE host init + FreeRTOS host task
 *   - GATT service ABF0: ABF1 (WriteNoRsp) + ABF2 (Notify + CCCD)
 *   - Advertising with ABF0 UUID in primary packet
 *   - Security: bonding, LE Secure Connections, NO_IO
 *   - Connection state: MTU, CCCD, security tracking
 *   - State machine: STOPPED -> INITIALIZED -> ADVERTISING -> CONNECTED
 *     -> SECURING -> WAIT_CCCD -> READY -> ADVERTISING
 *   - Bounded RX callback (copy + enqueue, NO heap allocation)
 *   - Notify TX via queue + worker task
 *   - Bond clearing for factory reset
 *   - Repeat pairing recovery with stale bond cleanup
 *
 * Spec references:
 *   - §9 (GATT write callback target): no malloc/calloc/free in callback
 *   - §19 (Repeat pairing recovery): remove stale bond before retry
 *   - §10 (Command RX pipeline): bounded raw queue
 */
#include "ble_peripheral.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/util/util.h"
#include "os/os_mbuf.h"
#include "host/ble_store.h"
#include "host/ble_sm.h"

/* Provided by NimBLE store/config component. */
void ble_store_config_init(void);

#include "gateway_protocol.h"

static const char *TAG = "ble_peripheral";

/* ------------------------------------------------------------------ *
 * Internal types
 * ------------------------------------------------------------------ */

typedef struct {
    uint8_t data[GW_MSG_MAX_LEN];
    uint16_t len;
} rx_msg_t;

typedef struct {
    uint8_t data[GW_MSG_MAX_LEN];
    uint16_t len;
} notify_msg_t;

/* ------------------------------------------------------------------ *
 * Constants
 * ------------------------------------------------------------------ */

#define BLE_PERIPH_TASK_STACK       4096
#define BLE_PERIPH_TASK_PRIORITY    5
#define BLE_RX_QUEUE_DEPTH          8
#define BLE_NOTIFY_QUEUE_DEPTH      16
#define BLE_NOTIFY_TASK_STACK       4096
#define BLE_NOTIFY_TASK_PRIORITY    4
#define BLE_DEFAULT_MTU             256
#define BLE_ADV_INTERVAL_DEFAULT_MS 100
#define BLE_DEVICE_NAME_MAX_LEN     31

#define BLE_ADV_ITVL(ms) ((uint16_t)((ms) * 1000u / 625u))

#define READY_BIT_SECURITY  (1u << 0)
#define READY_BIT_CCCD      (1u << 1)
#define READY_BIT_ALL       (READY_BIT_SECURITY | READY_BIT_CCCD)

/* ------------------------------------------------------------------ *
 * Internal state
 * ------------------------------------------------------------------ */

static struct {
    ble_peripheral_config_t cfg;
    char device_name[BLE_DEVICE_NAME_MAX_LEN + 1];
    ble_peripheral_rx_cb_t rx_cb;
    ble_peripheral_state_cb_t state_cb;

    uint16_t conn_handle;
    uint16_t mtu;
    uint8_t own_addr_type;
    bool security_ok;
    bool cccd_enabled;
    ble_peripheral_state_t state;

    QueueHandle_t rx_queue;
    QueueHandle_t notify_queue;
    TaskHandle_t notify_task;

    uint16_t status_val_handle;
    bool gatt_registered;

    EventGroupHandle_t ready_event;
    SemaphoreHandle_t notify_submit_mutex;
    bool adv_active;

    /* Diagnostics (spec §24: queue/drop diagnostics). */
    struct {
        uint32_t rx_queued;
        uint32_t rx_dropped;
        uint32_t notify_queued;
        uint32_t notify_dropped;
        uint32_t notify_batch_rejected;
        uint32_t repeat_pairing_count;
    } diag;
} s_periph;

/* ------------------------------------------------------------------ *
 * Forward declarations
 * ------------------------------------------------------------------ */

static int gap_event_handler(struct ble_gap_event *event, void *arg);
static int cmd_chr_write_cb(uint16_t conn_handle, uint16_t attr_handle,
                            struct ble_gatt_access_ctxt *ctxt, void *arg);
static int status_chr_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                struct ble_gatt_access_ctxt *ctxt, void *arg);

/* ------------------------------------------------------------------ *
 * State machine (docs §16)
 * ------------------------------------------------------------------ */

static void set_state(ble_peripheral_state_t new_state)
{
    if (s_periph.state == new_state) return;
    ESP_LOGI(TAG, "state %d -> %d", (int)s_periph.state, (int)new_state);
    s_periph.state = new_state;
    if (s_periph.state_cb) s_periph.state_cb(new_state);
}

static void ready_gate_check(void)
{
    if (s_periph.state < BLE_PERIPH_CONNECTED) return;
    EventBits_t bits = xEventGroupGetBits(s_periph.ready_event);
    if ((bits & READY_BIT_ALL) == READY_BIT_ALL &&
        s_periph.state == BLE_PERIPH_WAIT_CCCD) {
        set_state(BLE_PERIPH_READY);
    }
}

/* ------------------------------------------------------------------ *
 * Advertising (docs §17)
 * ------------------------------------------------------------------ */

static void start_advertising(void)
{
    if (s_periph.adv_active) return;

    struct ble_gap_adv_params adv = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
        .itvl_min  = BLE_ADV_ITVL(s_periph.cfg.adv_interval_ms),
        .itvl_max  = BLE_ADV_ITVL(s_periph.cfg.adv_interval_ms),
        .channel_map = 0,
    };

    struct ble_hs_adv_fields fields = {
        .flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP,
    };
    fields.uuids16 = (const ble_uuid16_t[]){
        BLE_UUID16_INIT(GW_BLE_SERVICE_UUID)
    };
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv set fields failed: %d", rc);
        return;
    }

    struct ble_hs_adv_fields sr_fields = { 0 };
    const char *name = s_periph.cfg.device_name;
    if (name == NULL || name[0] == '\0') name = "GW-Device";
    sr_fields.name = (const uint8_t *)name;
    sr_fields.name_len = strlen(name);
    sr_fields.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&sr_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "scan rsp failed: %d", rc);
        return;
    }

    rc = ble_gap_adv_start(s_periph.own_addr_type, NULL,
                           BLE_HS_FOREVER, &adv,
                           gap_event_handler, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv start failed: %d", rc);
        return;
    }

    s_periph.adv_active = true;
    if (s_periph.state < BLE_PERIPH_CONNECTED) {
        set_state(BLE_PERIPH_ADVERTISING);
    }
    ESP_LOGI(TAG, "advertising (service 0x%04X)",
             (unsigned)GW_BLE_SERVICE_UUID);
}

/* ------------------------------------------------------------------ *
 * RX consumer task: dequeue from BLE RX queue, call rx_cb
 * ------------------------------------------------------------------ */

static void rx_consumer(void *arg)
{
    rx_msg_t rx;
    while (1) {
        if (xQueueReceive(s_periph.rx_queue, &rx, portMAX_DELAY) != pdTRUE)
            continue;
        if (s_periph.rx_cb != NULL) {
            s_periph.rx_cb(rx.data, rx.len);
        }
    }
}

/* ------------------------------------------------------------------ *
 * Notify TX (queue + worker task, docs §44..#46)
 * ------------------------------------------------------------------ */

static void notify_worker(void *arg)
{
    notify_msg_t msg;
    while (1) {
        if (xQueueReceive(s_periph.notify_queue, &msg, portMAX_DELAY) != pdTRUE)
            continue;
        if (s_periph.conn_handle == BLE_HS_CONN_HANDLE_NONE) {
            s_periph.diag.notify_dropped++;
            continue;
        }

        struct os_mbuf *om = os_msys_get_pkthdr(0, 0);
        if (om == NULL) {
            s_periph.diag.notify_dropped++;
            continue;
        }
        int rc = os_mbuf_append(om, msg.data, msg.len);
        if (rc != 0) {
            os_mbuf_free_chain(om);
            s_periph.diag.notify_dropped++;
            continue;
        }
        /* ble_gatts_notify_custom consumes om regardless of outcome. */
        rc = ble_gatts_notify_custom(s_periph.conn_handle,
                                     s_periph.status_val_handle, om);
        if (rc != 0 && rc != BLE_HS_ENOTCONN) {
            ESP_LOGW(TAG, "notify failed: %d", rc);
            s_periph.diag.notify_dropped++;
        } else {
            s_periph.diag.notify_queued++;
        }
    }
}

/* ------------------------------------------------------------------ *
 * GATT UUIDs — must be static (compound literals are not valid
 * in static initializers per C11 §6.7.9).
 * ------------------------------------------------------------------ */

static const ble_uuid16_t svc_uuid    = BLE_UUID16_INIT(GW_BLE_SERVICE_UUID);
static const ble_uuid16_t cmd_uuid    = BLE_UUID16_INIT(GW_BLE_COMMAND_UUID);
static const ble_uuid16_t status_uuid = BLE_UUID16_INIT(GW_BLE_STATUS_UUID);

/* ------------------------------------------------------------------ *
 * GATT service definition (docs §18)
 * ------------------------------------------------------------------ */

static struct ble_gatt_chr_def gatt_chars[] = {
    {
        .uuid      = (const ble_uuid_t *)&cmd_uuid,
        .access_cb = cmd_chr_write_cb,
        .flags     = BLE_GATT_CHR_F_WRITE_NO_RSP,
    },
    {
        .uuid       = (const ble_uuid_t *)&status_uuid,
        .access_cb  = status_chr_access_cb,
        .flags      = BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_periph.status_val_handle,
    },
    { 0 },
};

static const struct ble_gatt_svc_def gatt_services[] = {
    {
        .type           = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid           = (const ble_uuid_t *)&svc_uuid,
        .characteristics = gatt_chars,
    },
    { 0 },
};

/* ------------------------------------------------------------------ *
 * GATT access callbacks
 *
 * Spec §9: GATT command-write callback must NOT heap allocate.
 * Direct os_mbuf_copydata into queue object (bounded copy).
 * No malloc/calloc/free/CBOR decode/command execution in callback.
 * ------------------------------------------------------------------ */

static int cmd_chr_write_cb(uint16_t conn_handle, uint16_t attr_handle,
                            struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;

    const struct os_mbuf *om = ctxt->om;
    size_t len = OS_MBUF_PKTLEN(om);
    if (len == 0 || len > GW_MSG_MAX_LEN) return 0;

    /* Bounded copy directly from os_mbuf into queue object.
     * No heap allocation (spec §9, §10). */
    rx_msg_t rx = {
        .len = (uint16_t)len,
    };

    if (os_mbuf_copydata(om, 0, len, rx.data) != 0) {
        ESP_LOGW(TAG, "RX copy failed");
        return 0;
    }

    if (xQueueSend(s_periph.rx_queue, &rx, 0) != pdTRUE) {
        s_periph.diag.rx_dropped++;
        ESP_LOGW(TAG, "RX queue full, dropping %zu bytes (total dropped: %lu)",
                 len, (unsigned long)s_periph.diag.rx_dropped);
    } else {
        s_periph.diag.rx_queued++;
    }
    return 0;
}

static int status_chr_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    return BLE_ATT_ERR_UNLIKELY;
}

/* ------------------------------------------------------------------ *
 * NimBLE host callbacks
 * ------------------------------------------------------------------ */

static void on_ble_host_sync(void)
{
    int rc = ble_hs_id_infer_auto(0, &s_periph.own_addr_type);
    assert(rc == 0);
    ESP_LOGI(TAG, "BLE host synced, addr type %d", s_periph.own_addr_type);
    start_advertising();
}

static void on_ble_host_reset(int reason)
{
    ESP_LOGW(TAG, "BLE host reset (reason %d)", reason);
    s_periph.conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s_periph.mtu = 0;
    s_periph.security_ok = false;
    s_periph.cccd_enabled = false;
    s_periph.adv_active = false;
    xEventGroupClearBits(s_periph.ready_event, READY_BIT_ALL);
}

static void nimble_host_task(void *arg)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* ------------------------------------------------------------------ *
 * GAP event handler
 * ------------------------------------------------------------------ */

static int gap_event_handler(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {

    case BLE_GAP_EVENT_CONNECT: {
        /* Connect completion ends the advertising procedure. */
        s_periph.adv_active = false;
        if (event->connect.status != 0) {
            ESP_LOGW(TAG, "connect failed: %d", event->connect.status);
            set_state(BLE_PERIPH_ADVERTISING);
            start_advertising();
            return 0;
        }
        s_periph.conn_handle = event->connect.conn_handle;
        s_periph.mtu = 0;
        s_periph.security_ok = false;
        s_periph.cccd_enabled = false;
        xEventGroupClearBits(s_periph.ready_event, READY_BIT_ALL);
        set_state(BLE_PERIPH_CONNECTED);

        /*
         * Gateway is the BLE Central and owns security initiation.
         * Peripheral waits for the Central's pairing/encryption procedure.
         */
        return 0;
    }

    case BLE_GAP_EVENT_DISCONNECT: {
        ESP_LOGI(TAG, "disconnected (reason %d)", event->disconnect.reason);
        s_periph.conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_periph.mtu = 0;
        s_periph.security_ok = false;
        s_periph.cccd_enabled = false;
        s_periph.adv_active = false;
        xEventGroupClearBits(s_periph.ready_event, READY_BIT_ALL);
        set_state(BLE_PERIPH_ADVERTISING);
        start_advertising();
        return 0;
    }

    case BLE_GAP_EVENT_MTU: {
        s_periph.mtu = event->mtu.value;
        ESP_LOGI(TAG, "MTU: %u", (unsigned)s_periph.mtu);
        return 0;
    }

    case BLE_GAP_EVENT_ENC_CHANGE: {
        if (event->enc_change.status == 0) {
            struct ble_gap_conn_desc desc;
            bool bonded = false;
            bool encrypted = false;

            if (ble_gap_conn_find(event->enc_change.conn_handle, &desc) == 0) {
                bonded = desc.sec_state.bonded;
                encrypted = desc.sec_state.encrypted;
            }

            ESP_LOGI(TAG,
                     "security established: handle=%u encrypted=%d bonded=%d",
                     event->enc_change.conn_handle,
                     encrypted,
                     bonded);

            s_periph.security_ok = true;
            xEventGroupSetBits(s_periph.ready_event, READY_BIT_SECURITY);

            if (s_periph.state == BLE_PERIPH_CONNECTED) {
                set_state(BLE_PERIPH_SECURING);
            }

            ready_gate_check();
        } else {
            ESP_LOGW(TAG,
                     "security failed: handle=%u status=%d (0x%04X)",
                     event->enc_change.conn_handle,
                     event->enc_change.status,
                     event->enc_change.status);
        }

        return 0;
    }

    case BLE_GAP_EVENT_SUBSCRIBE: {
        if (event->subscribe.attr_handle == s_periph.status_val_handle) {
            s_periph.cccd_enabled = event->subscribe.cur_notify;
            if (s_periph.cccd_enabled) {
                ESP_LOGI(TAG, "STATUS notifications enabled");
                xEventGroupSetBits(s_periph.ready_event, READY_BIT_CCCD);
                if (s_periph.state < BLE_PERIPH_WAIT_CCCD) {
                    set_state(BLE_PERIPH_WAIT_CCCD);
                }
                ready_gate_check();
            } else {
                s_periph.cccd_enabled = false;
                xEventGroupClearBits(s_periph.ready_event, READY_BIT_CCCD);
                if (s_periph.security_ok) {
                    set_state(BLE_PERIPH_WAIT_CCCD);
                }
            }
        }
        return 0;
    }

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        /* Spec §19: Repeat pairing recovery.
         * When peer requests repeat pairing because one side lost bond,
         * we must remove stale local bond for that peer before retrying.
         * This prevents tight loop and ensures clean re-pairing. */
        s_periph.diag.repeat_pairing_count++;
        ESP_LOGW(TAG, "repeat pairing — removing stale bond for peer (count: %lu)",
                 (unsigned long)s_periph.diag.repeat_pairing_count);

        /* Delete stale security material for this specific peer.
         * Use conn_handle to find the peer address, then delete its bonds. */
        struct ble_gap_conn_desc desc;
        int rc = ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc);
        if (rc == 0) {
            /* Peer found — delete its security records using peer-specific API. */
            ble_store_util_delete_peer(&desc.peer_id_addr);
            ESP_LOGI(TAG, "stale bond removed for peer, retrying pairing");
        } else {
            ESP_LOGW(TAG, "could not identify peer for bond cleanup (rc=%d)", rc);
        }

        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

    default:
        break;
    }
    return 0;
}

/* ------------------------------------------------------------------ *
 * Public API
 * ------------------------------------------------------------------ */

int ble_peripheral_init(const ble_peripheral_config_t *config,
                        ble_peripheral_rx_cb_t rx_cb,
                        ble_peripheral_state_cb_t state_cb)
{
    if (config == NULL) return -1;

    memset(&s_periph, 0, sizeof(s_periph));
    /* Keep an owned copy: callers are allowed to pass a stack config. */
    s_periph.cfg = *config;
    if (config->device_name != NULL) {
        size_t name_len = strnlen(config->device_name, BLE_DEVICE_NAME_MAX_LEN);
        memcpy(s_periph.device_name, config->device_name, name_len);
        s_periph.device_name[name_len] = '\0';
        s_periph.cfg.device_name = s_periph.device_name;
    }
    if (s_periph.cfg.adv_interval_ms == 0) {
        s_periph.cfg.adv_interval_ms = BLE_ADV_INTERVAL_DEFAULT_MS;
    }
    s_periph.rx_cb = rx_cb;
    s_periph.state_cb = state_cb;
    s_periph.conn_handle = BLE_HS_CONN_HANDLE_NONE;

    s_periph.rx_queue = xQueueCreate(BLE_RX_QUEUE_DEPTH, sizeof(rx_msg_t));
    if (s_periph.rx_queue == NULL) return -1;

    s_periph.notify_queue = xQueueCreate(BLE_NOTIFY_QUEUE_DEPTH,
                                         sizeof(notify_msg_t));
    if (s_periph.notify_queue == NULL) return -1;

    s_periph.ready_event = xEventGroupCreate();
    if (s_periph.ready_event == NULL) return -1;

    s_periph.notify_submit_mutex = xSemaphoreCreateMutex();
    if (s_periph.notify_submit_mutex == NULL) return -1;

    set_state(BLE_PERIPH_INITIALIZED);
    ESP_LOGI(TAG, "initialized (name=%s, bond=%d)",
             config->device_name ? config->device_name : "default",
             config->require_bonding);
    return 0;
}

int ble_peripheral_start(void)
{
    if (s_periph.state < BLE_PERIPH_INITIALIZED) return -1;

    esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %d", ret);
        return -1;
    }

    ble_hs_cfg.reset_cb = on_ble_host_reset;
    ble_hs_cfg.sync_cb = on_ble_host_sync;

    /*
     * Security model:
     * - Bonding enabled for gateway devices.
     * - LE Secure Connections.
     * - Just Works / no MITM because both sides are NO_IO.
     */
    ble_hs_cfg.sm_bonding = s_periph.cfg.require_bonding ? 1 : 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;

    /*
     * Persist LTK/security material.
     *
     * Without a configured store, bonding can become inconsistent across
     * reconnects/reboots and lead to SMP failures.
     */
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    if (s_periph.cfg.require_bonding) {
        ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC;
        ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC;
    }

    ble_store_config_init();

    int rc = ble_gatts_count_cfg(gatt_services);
    if (rc != 0) {
        ESP_LOGE(TAG, "gatts count cfg: %d", rc);
        return rc;
    }
    rc = ble_gatts_add_svcs(gatt_services);
    if (rc != 0) {
        ESP_LOGE(TAG, "gatts add svcs: %d", rc);
        return rc;
    }
    s_periph.gatt_registered = true;
    ESP_LOGI(TAG, "GATT: ABF0 {ABF1 WR_NO_RSP, ABF2 NOTIFY+CCCD}");

    nimble_port_freertos_init(nimble_host_task);

    xTaskCreate(notify_worker, "ble_notify", BLE_NOTIFY_TASK_STACK,
                NULL, BLE_NOTIFY_TASK_PRIORITY, &s_periph.notify_task);

    /* RX consumer: dequeue from BLE RX queue, call rx_cb if set. */
    if (s_periph.rx_cb != NULL) {
        xTaskCreate(rx_consumer, "ble_rx", BLE_PERIPH_TASK_STACK,
                    NULL, BLE_PERIPH_TASK_PRIORITY, NULL);
    }

    ESP_LOGI(TAG, "started");
    return 0;
}

int ble_peripheral_stop(void)
{
    ble_gap_adv_stop();
    s_periph.adv_active = false;
    if (s_periph.conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_periph.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    if (s_periph.notify_task) {
        vTaskDelete(s_periph.notify_task);
        s_periph.notify_task = NULL;
    }
    set_state(BLE_PERIPH_STOPPED);
    return 0;
}

int ble_peripheral_notify(const uint8_t *data, size_t len)
{
    ble_peripheral_notify_item_t item = {.data = data, .len = len};
    return ble_peripheral_notify_batch(&item, 1);
}

int ble_peripheral_notify_batch(const ble_peripheral_notify_item_t *items,
                                size_t count)
{
    if (items == NULL || count == 0 || count > BLE_NOTIFY_QUEUE_DEPTH ||
        s_periph.notify_submit_mutex == NULL ||
        xSemaphoreTake(s_periph.notify_submit_mutex,
                       pdMS_TO_TICKS(1000)) != pdTRUE) {
        return -1;
    }

    int result = -1;
    if (s_periph.state != BLE_PERIPH_READY || !s_periph.cccd_enabled) goto done;

    uint16_t max_payload = gw_ble_max_tx_payload(s_periph.mtu);
    for (size_t i = 0; i < count; i++) {
        if (items[i].data == NULL || items[i].len == 0 ||
            items[i].len > max_payload || items[i].len > GW_MSG_MAX_LEN) {
            goto done;
        }
    }

    /* Wait for enough room before the first enqueue, so a batch is never
     * partially submitted. The worker can continue draining while producers
     * are serialized by notify_submit_mutex. */
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(1000);
    while (uxQueueSpacesAvailable(s_periph.notify_queue) < count) {
        if (s_periph.state != BLE_PERIPH_READY ||
            (int32_t)(deadline - xTaskGetTickCount()) <= 0) {
            goto done;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    for (size_t i = 0; i < count; i++) {
        notify_msg_t msg = {.len = (uint16_t)items[i].len};
        memcpy(msg.data, items[i].data, items[i].len);
        if (xQueueSend(s_periph.notify_queue, &msg, 0) != pdTRUE) goto done;
    }
    result = 0;

done:
    xSemaphoreGive(s_periph.notify_submit_mutex);
    if (result != 0) {
        s_periph.diag.notify_batch_rejected++;
        ESP_LOGW(TAG, "notify batch rejected (count=%u, total rejected: %lu)",
                 (unsigned)count,
                 (unsigned long)s_periph.diag.notify_batch_rejected);
    }
    return result;
}

bool ble_peripheral_is_connected(void)
{
    return s_periph.conn_handle != BLE_HS_CONN_HANDLE_NONE;
}

bool ble_peripheral_is_ready(void)
{
    return s_periph.state == BLE_PERIPH_READY;
}

uint16_t ble_peripheral_get_mtu(void)
{
    return s_periph.mtu;
}

ble_peripheral_state_t ble_peripheral_get_state(void)
{
    return s_periph.state;
}

int ble_peripheral_clear_bonds(void)
{
    int rc1 = ble_store_util_delete_all(BLE_STORE_OBJ_TYPE_OUR_SEC, NULL);
    int rc2 = ble_store_util_delete_all(BLE_STORE_OBJ_TYPE_PEER_SEC, NULL);
    int rc3 = ble_store_util_delete_all(BLE_STORE_OBJ_TYPE_CCCD, NULL);
    int rc = (rc1 != 0) ? rc1 : (rc2 != 0) ? rc2 : rc3;
    if (rc != 0) {
        ESP_LOGE(TAG, "clear bonds failed: %d", rc);
    } else {
        ESP_LOGI(TAG, "all bonds cleared");
    }
    return rc;
}

/* ------------------------------------------------------------------ *
 * Diagnostics (spec §24)
 * ------------------------------------------------------------------ */

void ble_peripheral_get_diag(ble_peripheral_diag_t *out_diag)
{
    if (out_diag == NULL) return;
    out_diag->rx_queued = s_periph.diag.rx_queued;
    out_diag->rx_dropped = s_periph.diag.rx_dropped;
    out_diag->notify_queued = s_periph.diag.notify_queued;
    out_diag->notify_dropped = s_periph.diag.notify_dropped;
    out_diag->notify_batch_rejected = s_periph.diag.notify_batch_rejected;
    out_diag->repeat_pairing_count = s_periph.diag.repeat_pairing_count;
}
