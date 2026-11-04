#include "infrastructure/ble_peripheral.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "application/slave_service.h"
#include "domain/slave_types.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "store/config/ble_store_config.h"

void ble_store_config_init(void);

#define CONN_NONE BLE_HS_CONN_HANDLE_NONE

static const char *TAG = "ble_peripheral";
static const ble_uuid128_t SERVICE_UUID = BLE_UUID128_INIT(
    0x49,0x54,0x52,0x41,0x4d,0x53,0x65,0x9a,0x8d,0x4b,0x6f,0x6c,0x01,0x00,0x51,0x7f);
static const ble_uuid128_t COMMAND_UUID = BLE_UUID128_INIT(
    0x49,0x54,0x52,0x41,0x4d,0x53,0x65,0x9a,0x8d,0x4b,0x6f,0x6c,0x02,0x00,0x51,0x7f);
static const ble_uuid128_t STATUS_UUID = BLE_UUID128_INIT(
    0x49,0x54,0x52,0x41,0x4d,0x53,0x65,0x9a,0x8d,0x4b,0x6f,0x6c,0x03,0x00,0x51,0x7f);

static uint16_t s_conn_handle = CONN_NONE;
static uint16_t s_status_handle;
static uint8_t s_own_addr_type;
static bool s_host_synced;
static bool s_notify_enabled;
static char s_device_name[32];

static int gap_event(struct ble_gap_event *event, void *arg);
static void start_advertising(void);

static int gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
    const uint16_t length = OS_MBUF_PKTLEN(ctxt->om);
    if (length > SMART_IR_MAX_FRAME_SIZE || length < SMART_IR_FRAME_OVERHEAD) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    uint8_t bytes[SMART_IR_MAX_FRAME_SIZE];
    if (os_mbuf_copydata(ctxt->om, 0, length, bytes) != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    smart_ir_frame_t frame;
    esp_err_t err = smart_ir_frame_decode(bytes, length, &frame);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Invalid command frame: %s", esp_err_to_name(err));
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    slave_service_handle_frame(&frame);
    return 0;
}

static int status_access(uint16_t conn_handle, uint16_t attr_handle,
                         struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)ctxt;
    (void)arg;
    return BLE_ATT_ERR_READ_NOT_PERMITTED;
}

static const struct ble_gatt_svc_def SERVICES[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &SERVICE_UUID.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &COMMAND_UUID.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC,
            },
            {
                .uuid = &STATUS_UUID.u,
                .access_cb = status_access,
                .val_handle = &s_status_handle,
                .flags = BLE_GATT_CHR_F_NOTIFY,
            },
            { 0 }
        },
    },
    { 0 }
};

esp_err_t ble_peripheral_send_frame(const smart_ir_frame_t *frame)
{
    if (!frame || s_conn_handle == CONN_NONE || !s_notify_enabled) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t bytes[SMART_IR_MAX_FRAME_SIZE];
    size_t length = 0;
    esp_err_t err = smart_ir_frame_encode(frame, bytes, sizeof(bytes), &length);
    if (err != ESP_OK) return err;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(bytes, length);
    if (!om) return ESP_ERR_NO_MEM;
    int rc = ble_gatts_notify_custom(s_conn_handle, s_status_handle, om);
    if (rc != 0) {
        ESP_LOGW(TAG, "Notify failed: %d", rc);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "TX evt=0x%02X seq=%u id=%u len=%u", frame->command,
             frame->sequence, frame->slave_id, (unsigned)length);
    return ESP_OK;
}

static void start_advertising(void)
{
    if (!s_host_synced || s_conn_handle != CONN_NONE || ble_gap_adv_active()) return;
    slave_config_t config;
    slave_service_snapshot(&config);
    uint8_t manufacturer[7] = {
        (uint8_t)SMART_IR_COMPANY_ID,
        (uint8_t)(SMART_IR_COMPANY_ID >> 8),
        SMART_IR_ADV_MAGIC_0,
        SMART_IR_ADV_MAGIC_1,
        SMART_IR_PROTOCOL_VERSION,
        slave_service_advertising_flags(),
        config.provisioned ? config.id : SLAVE_UNPROVISIONED_ID,
    };
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = (ble_uuid128_t *)&SERVICE_UUID;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    fields.mfg_data = manufacturer;
    fields.mfg_data_len = sizeof(manufacturer);
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "Could not set advertising fields: %d", rc);
        return;
    }
    struct ble_hs_adv_fields scan_response = {0};
    scan_response.name = (uint8_t *)s_device_name;
    scan_response.name_len = strlen(s_device_name);
    scan_response.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&scan_response);
    if (rc != 0) {
        ESP_LOGE(TAG, "Could not set scan response: %d", rc);
        return;
    }
    const struct ble_gap_adv_params params = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
        .itvl_min = 0,
        .itvl_max = 0,
        .channel_map = 0,
        .filter_policy = 0,
        .high_duty_cycle = 0,
    };
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                           &params, gap_event, NULL);
    if (rc != 0) ESP_LOGE(TAG, "Advertising start failed: %d", rc);
    else ESP_LOGI(TAG, "Advertising %s id=%u flags=0x%02X", s_device_name,
                  manufacturer[6], manufacturer[5]);
}

void ble_peripheral_refresh_advertising(void)
{
    if (!s_host_synced || s_conn_handle != CONN_NONE) return;
    if (ble_gap_adv_active()) (void)ble_gap_adv_stop();
    start_advertising();
}

void ble_peripheral_enter_deep_sleep(uint16_t sleep_ms)
{
    if (s_conn_handle != CONN_NONE) {
        (void)ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    ESP_LOGI(TAG, "Entering deep sleep for %u ms", sleep_ms);
    ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup((uint64_t)sleep_ms * 1000ULL));
    esp_deep_sleep_start();
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    struct ble_gap_conn_desc desc;
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status != 0) {
                ESP_LOGW(TAG, "Connection failed: %d", event->connect.status);
                start_advertising();
                return 0;
            }
            s_conn_handle = event->connect.conn_handle;
            s_notify_enabled = false;
            ESP_LOGI(TAG, "Master connected, handle=%u", s_conn_handle);
            return 0;
        case BLE_GAP_EVENT_SUBSCRIBE:
            if (event->subscribe.attr_handle == s_status_handle) {
                s_notify_enabled = event->subscribe.cur_notify != 0;
                ESP_LOGI(TAG, "Status notifications %s",
                         s_notify_enabled ? "enabled" : "disabled");
                if (s_notify_enabled) slave_service_on_subscribed();
            }
            return 0;
        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "Master disconnected, reason=%d", event->disconnect.reason);
            s_conn_handle = CONN_NONE;
            s_notify_enabled = false;
            slave_service_on_disconnected();
            start_advertising();
            return 0;
        case BLE_GAP_EVENT_ADV_COMPLETE:
            start_advertising();
            return 0;
        case BLE_GAP_EVENT_REPEAT_PAIRING:
            if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
                ble_store_util_delete_peer(&desc.peer_id_addr);
            }
            return BLE_GAP_REPEAT_PAIRING_RETRY;
        case BLE_GAP_EVENT_ENC_CHANGE:
            ESP_LOGI(TAG, "Link encryption status=%d", event->enc_change.status);
            return 0;
        case BLE_GAP_EVENT_MTU:
            ESP_LOGI(TAG, "ATT MTU=%u", event->mtu.value);
            return 0;
        default:
            return 0;
    }
}

static void on_reset(int reason)
{
    s_host_synced = false;
    ESP_LOGE(TAG, "NimBLE reset: %d", reason);
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    assert(rc == 0);
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    assert(rc == 0);
    s_host_synced = true;
    start_advertising();
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t ble_peripheral_init(void)
{
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_BT));
    snprintf(s_device_name, sizeof(s_device_name), "Smart-IR-Slave-%02X%02X%02X",
             mac[3], mac[4], mac[5]);

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) return err;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_svc_gap_device_name_set(s_device_name);
    if (rc == 0) rc = ble_gatts_count_cfg(SERVICES);
    if (rc == 0) rc = ble_gatts_add_svcs(SERVICES);
    if (rc != 0) return ESP_FAIL;

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_store_config_init();
    nimble_port_freertos_init(host_task);
    return ESP_OK;
}
