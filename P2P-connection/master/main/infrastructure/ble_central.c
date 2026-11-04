#include "infrastructure/ble_central.h"
#include "infrastructure/ble_config_server.h"
#include <assert.h>
#include <string.h>
#include "application/master_service.h"
#include "domain/ble_protocol.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "store/config/ble_store_config.h"

/* NimBLE's store/config component exports this initialization entry point but
   does not publish it in a public header in ESP-IDF 5.5.x. */
void ble_store_config_init(void);

static const char *TAG = "ble_central";
static const ble_uuid128_t SERVICE_UUID = BLE_UUID128_INIT(
    0x49,0x54,0x52,0x41,0x4d,0x53,0x65,0x9a,0x8d,0x4b,0x6f,0x6c,0x01,0x00,0x51,0x7f);
static const ble_uuid128_t COMMAND_UUID = BLE_UUID128_INIT(
    0x49,0x54,0x52,0x41,0x4d,0x53,0x65,0x9a,0x8d,0x4b,0x6f,0x6c,0x02,0x00,0x51,0x7f);
static const ble_uuid128_t STATUS_UUID = BLE_UUID128_INIT(
    0x49,0x54,0x52,0x41,0x4d,0x53,0x65,0x9a,0x8d,0x4b,0x6f,0x6c,0x03,0x00,0x51,0x7f);

typedef struct {
    bool active;
    bool secured;
    bool wake_received;
    bool command_in_flight;
    bool stop_acknowledged;
    uint16_t conn_handle;
    uint16_t service_start;
    uint16_t service_end;
    uint16_t command_handle;
    uint16_t status_handle;
    uint16_t cccd_handle;
    master_ble_address_t address;
} central_session_t;

static central_session_t s_session;
static esp_timer_handle_t s_wake_timer;
static esp_timer_handle_t s_transaction_timer;
static volatile bool s_host_synced;
static volatile bool s_scan_enabled;
static volatile bool s_configuration_active;
static volatile bool s_configuration_scan_active;
static uint8_t s_configuration_scan_windows_remaining;
static bool s_demand_scan_active;
static int64_t s_demand_scan_deadline_ms;
static int gap_event(struct ble_gap_event *event, void *arg);
static void start_scan(void);
static void schedule_next_activity(void);

#define DEMAND_SCAN_TIMEOUT_MS 15000

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void start_demand_scan_window(void)
{
    if (!master_service_has_pending_commands()) return;
    s_demand_scan_active = true;
    s_demand_scan_deadline_ms = now_ms() + DEMAND_SCAN_TIMEOUT_MS;
}

static void session_timeout(void *arg)
{
    const char *kind = arg;
    if (s_session.active) {
        ESP_LOGW(TAG, "%s timeout; disconnecting", kind);
        ble_gap_terminate(s_session.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
}

static void timers_stop(void)
{
    esp_timer_stop(s_wake_timer);
    esp_timer_stop(s_transaction_timer);
}

static void start_transaction_timeout(void)
{
    esp_timer_stop(s_transaction_timer);
    esp_timer_start_once(s_transaction_timer, 3ULL * 1000 * 1000);
}

static int write_complete(uint16_t conn_handle, const struct ble_gatt_error *error,
                          struct ble_gatt_attr *attr, void *arg)
{
    (void)conn_handle; (void)attr; (void)arg;
    if (error->status != 0) {
        ESP_LOGW(TAG, "GATT write failed: %d", error->status);
        s_session.command_in_flight = false;
    }
    return 0;
}

static void send_next_command(void)
{
    smart_ir_frame_t frame;
    if (!s_session.active || !s_session.wake_received || s_session.command_in_flight ||
        !s_session.command_handle ||
        !master_service_peek_command(&s_session.address, &frame)) return;
    uint8_t bytes[SMART_IR_MAX_FRAME_SIZE];
    size_t length = 0;
    if (smart_ir_frame_encode(&frame, bytes, sizeof(bytes), &length) != ESP_OK) return;
    ESP_LOGI(TAG, "TX cmd=0x%02X seq=%u id=%u len=%u", frame.command,
             frame.sequence, frame.slave_id, (unsigned)length);
    int rc = ble_gattc_write_flat(s_session.conn_handle, s_session.command_handle,
                                  bytes, length, write_complete, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "Could not start GATT write: %d", rc);
        return;
    }
    s_session.command_in_flight = true;
    start_transaction_timeout();
}

static int subscribe_complete(uint16_t conn_handle, const struct ble_gatt_error *error,
                              struct ble_gatt_attr *attr, void *arg)
{
    (void)conn_handle; (void)attr; (void)arg;
    if (error->status != 0) {
        ESP_LOGE(TAG, "Subscribe failed: %d", error->status);
        ble_gap_terminate(s_session.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return 0;
    }
    ESP_LOGI(TAG, "Subscribed; waiting for WAKE_STATUS");
    esp_timer_stop(s_wake_timer);
    esp_timer_start_once(s_wake_timer, 2ULL * 1000 * 1000);
    /* The slave can send WAKE_STATUS from its CCCD callback before this write
       procedure completes.  The first command write then returns EBUSY. Retry
       now that the subscription procedure has definitely finished. */
    send_next_command();
    return 0;
}

static int descriptor_discovered(uint16_t conn_handle, const struct ble_gatt_error *error,
                                 uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc,
                                 void *arg)
{
    (void)chr_val_handle; (void)arg;
    if (error->status == 0 && ble_uuid_u16(&dsc->uuid.u) == BLE_GATT_DSC_CLT_CFG_UUID16) {
        s_session.cccd_handle = dsc->handle;
        uint8_t enable[2] = {1, 0};
        ble_gattc_write_flat(conn_handle, dsc->handle, enable, sizeof(enable),
                             subscribe_complete, NULL);
    } else if (error->status == BLE_HS_EDONE && !s_session.cccd_handle) {
        ESP_LOGE(TAG, "Status CCCD not found");
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    return 0;
}

static int characteristic_discovered(uint16_t conn_handle,
                                     const struct ble_gatt_error *error,
                                     const struct ble_gatt_chr *chr, void *arg)
{
    (void)arg;
    if (error->status == 0) {
        if (ble_uuid_cmp(&chr->uuid.u, &COMMAND_UUID.u) == 0) s_session.command_handle = chr->val_handle;
        if (ble_uuid_cmp(&chr->uuid.u, &STATUS_UUID.u) == 0) s_session.status_handle = chr->val_handle;
    } else if (error->status == BLE_HS_EDONE) {
        if (!s_session.command_handle || !s_session.status_handle) {
            ESP_LOGE(TAG, "Required characteristics not found");
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        } else {
            int rc = ble_gattc_disc_all_dscs(conn_handle, s_session.status_handle,
                                              s_session.service_end,
                                              descriptor_discovered, NULL);
            if (rc != 0) ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
    }
    return 0;
}

static int service_discovered(uint16_t conn_handle, const struct ble_gatt_error *error,
                              const struct ble_gatt_svc *service, void *arg)
{
    (void)arg;
    if (error->status == 0) {
        s_session.service_start = service->start_handle;
        s_session.service_end = service->end_handle;
    } else if (error->status == BLE_HS_EDONE) {
        if (!s_session.service_start) {
            ESP_LOGE(TAG, "Smart IR service not found");
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        } else {
            int rc = ble_gattc_disc_all_chrs(conn_handle, s_session.service_start,
                                              s_session.service_end,
                                              characteristic_discovered, NULL);
            if (rc != 0) ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
    }
    return 0;
}

static void discover_service(uint16_t conn_handle)
{
    int rc = ble_gattc_disc_svc_by_uuid(conn_handle, &SERVICE_UUID.u,
                                         service_discovered, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "Could not start service discovery: %d", rc);
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
}

static master_ble_address_t convert_address(const ble_addr_t *address)
{
    master_ble_address_t result = { .type = address->type };
    memcpy(result.value, address->val, sizeof(result.value));
    return result;
}

static void handle_advertisement(const struct ble_gap_disc_desc *disc)
{
    struct ble_hs_adv_fields fields;
    if (ble_hs_adv_parse_fields(&fields, disc->data, disc->length_data) != 0 ||
        !fields.mfg_data || fields.mfg_data_len < 7) return;
    smart_ir_advertisement_t advertisement;
    if (smart_ir_parse_manufacturer_data(fields.mfg_data, fields.mfg_data_len,
                                         &advertisement) != ESP_OK) return;
    master_ble_address_t address = convert_address(&disc->addr);
    master_service_discovered(&address, disc->rssi, &advertisement);
    if (s_configuration_scan_active) return;
    if (!s_scan_enabled || s_session.active ||
        !master_service_is_registered(&address) ||
        (disc->event_type != BLE_HCI_ADV_RPT_EVTYPE_ADV_IND &&
         disc->event_type != BLE_HCI_ADV_RPT_EVTYPE_DIR_IND)) return;
    if (master_service_has_pending_commands() &&
        !master_service_address_has_pending(&address)) return;

    if (ble_gap_disc_cancel() != 0) return;
    uint8_t own_addr_type;
    if (ble_hs_id_infer_auto(0, &own_addr_type) != 0) return;
    memset(&s_session, 0, sizeof(s_session));
    s_session.active = true;
    s_session.address = address;
    int rc = ble_gap_connect(own_addr_type, &disc->addr, 3000, NULL, gap_event, NULL);
    if (rc != 0) {
        memset(&s_session, 0, sizeof(s_session));
        start_scan();
    }
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    struct ble_gap_conn_desc desc;
    switch (event->type) {
        case BLE_GAP_EVENT_DISC:
            handle_advertisement(&event->disc);
            return 0;
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status != 0) {
                ESP_LOGW(TAG, "Connection failed: %d", event->connect.status);
                memset(&s_session, 0, sizeof(s_session));
                schedule_next_activity();
                return 0;
            }
            s_session.conn_handle = event->connect.conn_handle;
            master_service_connection_changed(&s_session.address, true, false);
            if (ble_gap_security_initiate(event->connect.conn_handle) != 0) {
                ble_gap_terminate(event->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            }
            return 0;
        case BLE_GAP_EVENT_ENC_CHANGE:
            if (event->enc_change.status == 0) {
                s_session.secured = true;
                master_service_connection_changed(&s_session.address, true, true);
                discover_service(event->enc_change.conn_handle);
            } else {
                ble_gap_terminate(event->enc_change.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            }
            return 0;
        case BLE_GAP_EVENT_NOTIFY_RX: {
            uint16_t length = OS_MBUF_PKTLEN(event->notify_rx.om);
            if (length > SMART_IR_MAX_FRAME_SIZE) return 0;
            uint8_t bytes[SMART_IR_MAX_FRAME_SIZE];
            if (os_mbuf_copydata(event->notify_rx.om, 0, length, bytes) != 0) return 0;
            smart_ir_frame_t frame;
            esp_err_t err = smart_ir_frame_decode(bytes, length, &frame);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Invalid RX frame: %s", esp_err_to_name(err));
                return 0;
            }
            ESP_LOGI(TAG, "RX evt=0x%02X seq=%u id=%u", frame.command,
                     frame.sequence, frame.slave_id);
            smart_ir_frame_t pending_frame = {0};
            bool completes_pending =
                (frame.command == SMART_IR_EVT_ACK ||
                 frame.command == SMART_IR_EVT_AC_RESULT) &&
                master_service_peek_command(&s_session.address, &pending_frame) &&
                pending_frame.sequence == frame.sequence;
            master_service_handle_frame(&s_session.address, &frame);
            if (frame.command == SMART_IR_EVT_WAKE_STATUS) {
                s_session.wake_received = true;
                esp_timer_stop(s_wake_timer);
                send_next_command();
            } else if (frame.command == SMART_IR_EVT_ACK ||
                       frame.command == SMART_IR_EVT_AC_RESULT) {
                esp_timer_stop(s_transaction_timer);
                s_session.command_in_flight = false;
                if (completes_pending && pending_frame.command == SMART_IR_CMD_STOP) {
                    s_session.stop_acknowledged = true;
                } else if (completes_pending && pending_frame.command == SMART_IR_CMD_START) {
                    s_session.stop_acknowledged = false;
                }
                send_next_command();
                /* STOP disables both slave sleep timers. It therefore cannot
                   send PRE_SLEEP_STATUS to close the link. Close it from the
                   master after all commands in this session are acknowledged. */
                if (s_session.stop_acknowledged &&
                    !master_service_address_has_pending(&s_session.address)) {
                    ble_gap_terminate(s_session.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
                }
            } else if (frame.command == SMART_IR_EVT_NACK) {
                esp_timer_stop(s_transaction_timer);
                s_session.command_in_flight = false;
            } else if (frame.command == SMART_IR_EVT_PRE_SLEEP_STATUS) {
                timers_stop();
                ble_gap_terminate(s_session.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            }
            return 0;
        }
        case BLE_GAP_EVENT_DISCONNECT:
            timers_stop();
            master_service_connection_changed(&s_session.address, false, s_session.secured);
            memset(&s_session, 0, sizeof(s_session));
            schedule_next_activity();
            return 0;
        case BLE_GAP_EVENT_DISC_COMPLETE:
            if (s_session.active) return 0;
            if (s_configuration_scan_active) {
                if (s_configuration_scan_windows_remaining > 0) {
                    --s_configuration_scan_windows_remaining;
                    start_scan();
                    return 0;
                }
                s_configuration_scan_active = false;
                ESP_LOGI(TAG, "Configuration slave scan complete");
                if (master_service_has_pending_commands()) start_demand_scan_window();
            }
            schedule_next_activity();
            return 0;
        case BLE_GAP_EVENT_REPEAT_PAIRING:
            if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
                ble_store_util_delete_peer(&desc.peer_id_addr);
            }
            return BLE_GAP_REPEAT_PAIRING_RETRY;
        default:
            return 0;
    }
}

static void start_scan(void)
{
    if (!s_host_synced || !s_scan_enabled || s_session.active ||
        ble_gap_adv_active()) return;
    uint8_t own_addr_type;
    if (ble_hs_id_infer_auto(0, &own_addr_type) != 0) return;
    const struct ble_gap_disc_params params = {
        .itvl = 0, .window = 0, .filter_policy = 0,
        .limited = 0, .passive = 1, .filter_duplicates = 0,
    };
    int rc = ble_gap_disc(own_addr_type, 3000, &params, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) ESP_LOGE(TAG, "Scan start failed: %d", rc);
}

static void schedule_next_activity(void)
{
    if (!s_host_synced || !s_scan_enabled || s_session.active) return;
    if (s_demand_scan_active) {
        if (!master_service_has_pending_commands()) {
            s_demand_scan_active = false;
            ESP_LOGI(TAG, "Demand scan complete; all pending commands acknowledged");
        } else if (now_ms() < s_demand_scan_deadline_ms) {
            start_scan();
            return;
        } else {
            s_demand_scan_active = false;
            ESP_LOGW(TAG, "Demand scan timeout; commands remain queued for the next trigger");
        }
    }
    if (s_configuration_scan_active) {
        start_scan();
        return;
    }
    if (s_configuration_active) {
        ESP_LOGD(TAG, "Demand-driven transport idle while laptop remains connected");
        return;
    }
    if (!ble_gap_disc_active()) {
        ESP_LOGI(TAG, "Demand-driven transport idle; opening laptop configuration window");
        ble_config_server_start_window();
    }
}

static void on_reset(int reason)
{
    ESP_LOGE(TAG, "NimBLE reset: %d", reason);
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    assert(rc == 0);
    s_host_synced = true;
    ble_config_server_on_sync();
    ESP_LOGI(TAG, "BLE scheduler uses concurrent demand-driven slave transport");
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t ble_central_init(void)
{
    const esp_timer_create_args_t wake_args = { .callback = session_timeout,
        .arg = "WAKE_STATUS", .name = "ble_wake" };
    const esp_timer_create_args_t tx_args = { .callback = session_timeout,
        .arg = "transaction", .name = "ble_tx" };
    ESP_ERROR_CHECK(esp_timer_create(&wake_args, &s_wake_timer));
    ESP_ERROR_CHECK(esp_timer_create(&tx_args, &s_transaction_timer));

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) return err;
    err = ble_config_server_init();
    if (err != ESP_OK) return err;
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

void ble_central_set_scan_enabled(bool enabled)
{
    if (s_scan_enabled == enabled) return;
    s_scan_enabled = enabled;
    if (!s_host_synced) return;
    if (enabled) {
        ESP_LOGI(TAG, "BLE slave scan windows enabled");
        if (!ble_gap_adv_active()) ble_config_server_start_window();
    } else {
        ESP_LOGI(TAG, "BLE slave scan windows disabled");
        if (!s_session.active) {
            int rc = ble_gap_disc_cancel();
            if (rc != 0 && rc != BLE_HS_EALREADY) {
                ESP_LOGW(TAG, "Could not pause BLE scan: %d", rc);
            }
        }
    }
}

void ble_central_set_configuration_active(bool active)
{
    if (s_configuration_active == active) return;
    s_configuration_active = active;
    if (!s_host_synced) return;
    if (active) {
        ESP_LOGI(TAG, "Configuration session active; slave transport waits for a demand");
        schedule_next_activity();
    } else {
        ESP_LOGI(TAG, "Configuration session closed; processing pending slave work");
        if (!master_service_has_pending_commands() && ble_gap_disc_active()) {
            int rc = ble_gap_disc_cancel();
            if (rc != 0 && rc != BLE_HS_EALREADY) {
                ESP_LOGW(TAG, "Could not close idle slave scan: %d", rc);
            }
        } else {
            schedule_next_activity();
        }
    }
}

esp_err_t ble_central_start_configuration_scan(void)
{
    if (!s_host_synced || !s_configuration_active) return ESP_ERR_INVALID_STATE;
    master_service_clear_discovered();
    s_configuration_scan_active = true;
    if (ble_gap_disc_active()) {
        /* Finish the current partial window, then run one complete 3 s window. */
        s_configuration_scan_windows_remaining = 1;
        ESP_LOGI(TAG, "Configuration scan reset; one full scan window will follow");
        return ESP_OK;
    }
    if (s_session.active) {
        /* Two full discovery-only windows start after this transaction. */
        s_configuration_scan_windows_remaining = 1;
        ESP_LOGI(TAG, "Configuration scan queued after current slave transaction");
        return ESP_OK;
    }
    /* Run this window plus one more, for about six seconds total. */
    s_configuration_scan_windows_remaining = 1;
    uint8_t own_addr_type;
    if (ble_hs_id_infer_auto(0, &own_addr_type) != 0) return ESP_FAIL;
    const struct ble_gap_disc_params params = {
        .itvl = 0, .window = 0, .filter_policy = 0,
        .limited = 0, .passive = 1, .filter_duplicates = 0,
    };
    int rc = ble_gap_disc(own_addr_type, 3000, &params, gap_event, NULL);
    if (rc != 0) {
        s_configuration_scan_active = false;
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Configuration slave discovery-only scan started");
    return ESP_OK;
}

bool ble_central_scan_active(void)
{
    return ble_gap_disc_active();
}

void ble_central_kick(void)
{
    start_demand_scan_window();
    if (s_session.active) {
        send_next_command();
    } else {
        schedule_next_activity();
    }
}
