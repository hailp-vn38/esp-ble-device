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
 *   - Bounded RX callback (copy + enqueue)
 *   - Notify TX via queue + worker task
 *   - Bond clearing for factory reset
 */
#include "ble_peripheral.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/util/util.h"
#include "os/os_mbuf.h"


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
    bool adv_active;
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
        if (s_periph.conn_handle == BLE_HS_CONN_HANDLE_NONE) continue;

        struct os_mbuf *om = os_mbuf_get_pkthdr(NULL, 0);
        if (om == NULL) continue;
        os_mbuf_append(om, msg.data, msg.len);
        int rc = ble_gattc_notify_custom(s_periph.conn_handle,
                                         s_periph.status_val_handle, om);
        os_mbuf_free_chain(om);
        if (rc != 0 && rc != BLE_HS_ENOTCONN) {
            ESP_LOGW(TAG, "notify failed: %d", rc);
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
 * ------------------------------------------------------------------ */

static int cmd_chr_write_cb(uint16_t conn_handle, uint16_t attr_handle,
                            struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;

    const struct os_mbuf *om = ctxt->om;
    size_t len = OS_MBUF_PKTLEN(om);
    if (len == 0 || len > GW_MSG_MAX_LEN) return 0;

    uint8_t *buf = malloc(len);
    if (buf == NULL) return 0;
    os_mbuf_copydata(om, 0, len, buf);

    rx_msg_t rx = { .len = (uint16_t)len };
    memcpy(rx.data, buf, len);

    if (xQueueSend(s_periph.rx_queue, &rx, 0) != pdTRUE) {
        ESP_LOGW(TAG, "RX queue full, dropping %zu bytes", len);
    }
    free(buf);
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

        ble_gap_security_initiate(s_periph.conn_handle);
        return 0;
    }

    case BLE_GAP_EVENT_DISCONNECT: {
        ESP_LOGI(TAG, "disconnected (reason %d)", event->disconnect.reason);
        s_periph.conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_periph.mtu = 0;
        s_periph.security_ok = false;
        s_periph.cccd_enabled = false;
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
            ESP_LOGI(TAG, "security established");
            s_periph.security_ok = true;
            xEventGroupSetBits(s_periph.ready_event, READY_BIT_SECURITY);
            if (s_periph.state == BLE_PERIPH_CONNECTED) {
                set_state(BLE_PERIPH_SECURING);
            }
            ready_gate_check();
        } else {
            ESP_LOGW(TAG, "security failed: %d", event->enc_change.status);
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
            }
        }
        return 0;
    }

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        ESP_LOGW(TAG, "repeat pairing — retrying");
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
    ble_hs_cfg.sm_bonding = s_periph.cfg.require_bonding ? 1 : 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;

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
    if (s_periph.state < BLE_PERIPH_READY) return -1;
    if (data == NULL || len == 0 || len > GW_MSG_MAX_LEN) return -1;

    uint16_t max_payload = gw_ble_max_tx_payload(s_periph.mtu);
    if (len > max_payload) return -1;

    notify_msg_t msg = { .len = (uint16_t)len };
    memcpy(msg.data, data, len);

    if (xQueueSend(s_periph.notify_queue, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "notify queue full, dropping %zu bytes", len);
        return -1;
    }
    return 0;
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
