#include "infrastructure/ble_config_server.h"

#include <stdio.h>
#include <string.h>
#include "application/master_service.h"
#include "cJSON.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "host/ble_att.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "infrastructure/ble_central.h"
#include "infrastructure/factory_reset.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#define CONFIG_MESSAGE_MAX 1024
#define CONFIG_CHUNK_DATA_MAX 160
#define CONFIG_CONN_NONE BLE_HS_CONN_HANDLE_NONE

static const char *TAG = "ble_config";
static const ble_uuid128_t CONFIG_SERVICE_UUID = BLE_UUID128_INIT(
    0x49,0x54,0x52,0x41,0x4d,0x53,0x65,0x9a,0x8d,0x4b,0x6f,0x6c,0x01,0x00,0x52,0x7f);
static const ble_uuid128_t CONFIG_RX_UUID = BLE_UUID128_INIT(
    0x49,0x54,0x52,0x41,0x4d,0x53,0x65,0x9a,0x8d,0x4b,0x6f,0x6c,0x02,0x00,0x52,0x7f);
static const ble_uuid128_t CONFIG_TX_UUID = BLE_UUID128_INIT(
    0x49,0x54,0x52,0x41,0x4d,0x53,0x65,0x9a,0x8d,0x4b,0x6f,0x6c,0x03,0x00,0x52,0x7f);

static uint16_t s_conn_handle = CONFIG_CONN_NONE;
static uint16_t s_tx_handle;
static uint8_t s_own_addr_type;
static char s_device_name[32];
static char s_last_response[CONFIG_MESSAGE_MAX];
static uint8_t s_rx_message_id;
static uint8_t s_rx_next_chunk;
static uint8_t s_rx_chunk_count;
static size_t s_rx_length;
static char s_rx_buffer[CONFIG_MESSAGE_MAX];

static int config_gap_event(struct ble_gap_event *event, void *arg);

static cJSON *response_base(bool ok)
{
    cJSON *root = cJSON_CreateObject();
    if (root) cJSON_AddBoolToObject(root, "ok", ok);
    return root;
}

static cJSON *response_error(esp_err_t error)
{
    cJSON *root = response_base(false);
    if (root) cJSON_AddStringToObject(root, "error", esp_err_to_name(error));
    return root;
}

static bool json_number(const cJSON *root, const char *name, int *value)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!cJSON_IsNumber(item)) return false;
    *value = item->valueint;
    return true;
}

static cJSON *dispatch_request(const cJSON *request)
{
    const cJSON *op = cJSON_GetObjectItemCaseSensitive(request, "op");
    if (!cJSON_IsString(op) || !op->valuestring) return response_error(ESP_ERR_INVALID_ARG);

    if (strcmp(op->valuestring, "info") == 0) {
        cJSON *root = response_base(true);
        cJSON_AddStringToObject(root, "firmware", "0.2.9-dev");
        cJSON_AddStringToObject(root, "environment", "dev");
        cJSON_AddNumberToObject(root, "protocol", 1);
        cJSON_AddStringToObject(root, "transport", "ble");
        return root;
    }

    if (strcmp(op->valuestring, "get_wifi") == 0) {
        master_wifi_credentials_t credentials;
        esp_err_t error = master_service_get_wifi_credentials(&credentials);
        if (error != ESP_OK) return response_error(error);
        cJSON *root = response_base(true);
        cJSON_AddBoolToObject(root, "configured", credentials.configured);
        cJSON_AddStringToObject(root, "ssid", credentials.ssid);
        cJSON_AddBoolToObject(root, "password_saved", credentials.password[0] != '\0');
        return root;
    }

    if (strcmp(op->valuestring, "set_wifi") == 0) {
        const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(request, "ssid");
        const cJSON *password = cJSON_GetObjectItemCaseSensitive(request, "password");
        if (!cJSON_IsString(ssid) || !cJSON_IsString(password)) {
            return response_error(ESP_ERR_INVALID_ARG);
        }
        master_wifi_credentials_t current;
        master_service_get_wifi_credentials(&current);
        const char *effective_password = password->valuestring[0]
            ? password->valuestring : current.password;
        ESP_LOGI(TAG,
                 "Wi-Fi credential update received: SSID=\"%s\", password_action=%s",
                 ssid->valuestring,
                 password->valuestring[0] ? "replace" : "keep-existing");
        esp_err_t error = master_service_update_wifi(ssid->valuestring, effective_password);
        if (error != ESP_OK) return response_error(error);
        master_wifi_credentials_t saved;
        error = master_service_get_wifi_credentials(&saved);
        if (error != ESP_OK) return response_error(error);
        cJSON *root = response_base(true);
        cJSON_AddBoolToObject(root, "configured", saved.configured);
        cJSON_AddStringToObject(root, "ssid", saved.ssid);
        cJSON_AddBoolToObject(root, "password_saved", saved.password[0] != '\0');
        return root;
    }

    if (strcmp(op->valuestring, "counts") == 0) {
        master_slave_view_t slaves[MASTER_MAX_SLAVES];
        master_discovered_t discovered[MASTER_MAX_DISCOVERED];
        cJSON *root = response_base(true);
        cJSON_AddNumberToObject(root, "registered",
            master_service_slaves_snapshot(slaves, MASTER_MAX_SLAVES));
        cJSON_AddNumberToObject(root, "discovered",
            master_service_discovered_snapshot(discovered, MASTER_MAX_DISCOVERED));
        cJSON_AddBoolToObject(root, "scanning", ble_central_scan_active());
        cJSON_AddBoolToObject(root, "slave_transport_paused", false);
        cJSON_AddStringToObject(root, "slave_transport_mode", "demand-driven");
        return root;
    }

    if (strcmp(op->valuestring, "scan_slaves") == 0) {
        esp_err_t error = ble_central_start_configuration_scan();
        if (error != ESP_OK) return response_error(error);
        cJSON *root = response_base(true);
        cJSON_AddBoolToObject(root, "scanning", true);
        cJSON_AddNumberToObject(root, "duration_ms", 6000);
        return root;
    }

    if (strcmp(op->valuestring, "get_discovered") == 0) {
        int index;
        if (!json_number(request, "index", &index) || index < 0) {
            return response_error(ESP_ERR_INVALID_ARG);
        }
        master_discovered_t list[MASTER_MAX_DISCOVERED];
        size_t count = master_service_discovered_snapshot(list, MASTER_MAX_DISCOVERED);
        if ((size_t)index >= count) return response_error(ESP_ERR_NOT_FOUND);
        char mac[MASTER_MAC_STRING_SIZE];
        master_ble_address_format(&list[index].address, mac);
        cJSON *root = response_base(true);
        cJSON_AddNumberToObject(root, "index", index);
        cJSON_AddStringToObject(root, "mac", mac);
        cJSON_AddNumberToObject(root, "address_type", list[index].address.type);
        cJSON_AddNumberToObject(root, "rssi", list[index].rssi);
        cJSON_AddNumberToObject(root, "id", list[index].advertised_id);
        cJSON_AddBoolToObject(root, "provisioned", list[index].provisioned_flag);
        cJSON_AddBoolToObject(root, "running", list[index].running_flag);
        cJSON_AddBoolToObject(root, "registered",
            master_service_is_registered(&list[index].address));
        int64_t age = esp_timer_get_time() / 1000 - list[index].last_seen_ms;
        cJSON_AddNumberToObject(root, "last_seen_age_ms", age < 0 ? 0 : age);
        return root;
    }

    if (strcmp(op->valuestring, "get_slave") == 0) {
        int index;
        if (!json_number(request, "index", &index) || index < 0) {
            return response_error(ESP_ERR_INVALID_ARG);
        }
        master_slave_view_t list[MASTER_MAX_SLAVES];
        size_t count = master_service_slaves_snapshot(list, MASTER_MAX_SLAVES);
        if ((size_t)index >= count) return response_error(ESP_ERR_NOT_FOUND);
        const master_slave_view_t *slave = &list[index];
        char mac[MASTER_MAC_STRING_SIZE];
        master_ble_address_format(&slave->address, mac);
        cJSON *root = response_base(true);
        cJSON_AddNumberToObject(root, "index", index);
        cJSON_AddNumberToObject(root, "id", slave->id);
        cJSON_AddStringToObject(root, "mac", mac);
        cJSON_AddBoolToObject(root, "connected", slave->connected);
        cJSON_AddBoolToObject(root, "bonded", slave->bonded);
        cJSON_AddBoolToObject(root, "running", slave->running);
        cJSON_AddBoolToObject(root, "awake", slave->awake);
        cJSON_AddBoolToObject(root, "config_pending", slave->config_pending);
        cJSON_AddBoolToObject(root, "configuration_confirmed",
                              slave->configuration_confirmed);
        cJSON_AddBoolToObject(root, "advertised_provisioned",
                              slave->advertised_provisioned);
        cJSON_AddNumberToObject(root, "advertised_id", slave->advertised_id);
        int64_t age = slave->last_seen_ms
            ? esp_timer_get_time() / 1000 - slave->last_seen_ms : -1;
        cJSON_AddNumberToObject(root, "last_seen_age_ms", age < 0 ? -1 : age);
        cJSON_AddNumberToObject(root, "rssi", slave->rssi);
        cJSON_AddNumberToObject(root, "sleep_ms", slave->sleep_ms);
        cJSON_AddNumberToObject(root, "awake_timeout_ms", slave->awake_timeout_ms);
        cJSON_AddNumberToObject(root, "pending", slave->pending_count);
        cJSON_AddNumberToObject(root, "battery_mv", slave->battery_mv);
        cJSON_AddNumberToObject(root, "uptime_ms", slave->uptime_ms);
        cJSON_AddNumberToObject(root, "last_event", slave->last_event);
        cJSON_AddNumberToObject(root, "last_error", slave->last_error);
        cJSON *ac = cJSON_AddObjectToObject(root, "ac");
        cJSON_AddBoolToObject(ac, "power", slave->ac_state.power);
        cJSON_AddNumberToObject(ac, "mode", slave->ac_state.mode);
        cJSON_AddNumberToObject(ac, "temperature", slave->ac_state.temperature_c);
        cJSON_AddNumberToObject(ac, "fan", slave->ac_state.fan);
        cJSON_AddNumberToObject(ac, "swing", slave->ac_state.swing);
        return root;
    }

    if (strcmp(op->valuestring, "provision") == 0) {
        const cJSON *mac = cJSON_GetObjectItemCaseSensitive(request, "mac");
        int id, sleep_ms, awake_timeout_ms;
        if (!cJSON_IsString(mac) || !json_number(request, "id", &id) ||
            !json_number(request, "sleep_ms", &sleep_ms) ||
            !json_number(request, "awake_timeout_ms", &awake_timeout_ms) ||
            id < 0 || id > 255) return response_error(ESP_ERR_INVALID_ARG);
        master_ble_address_t address;
        if (!master_ble_address_parse(mac->valuestring, &address)) {
            return response_error(ESP_ERR_INVALID_ARG);
        }
        const cJSON *address_type = cJSON_GetObjectItemCaseSensitive(request, "address_type");
        if (cJSON_IsNumber(address_type)) address.type = (uint8_t)address_type->valueint;
        esp_err_t error = master_service_provision(&address, (uint8_t)id,
            (uint16_t)sleep_ms, (uint32_t)awake_timeout_ms);
        if (error != ESP_OK) return response_error(error);
        ble_central_kick();
        cJSON *root = response_base(true);
        cJSON_AddStringToObject(root, "mac", mac->valuestring);
        cJSON_AddNumberToObject(root, "target_id", id);
        cJSON_AddStringToObject(root, "state", "queued");
        return root;
    }

    if (strcmp(op->valuestring, "action") == 0) {
        int id;
        const cJSON *action = cJSON_GetObjectItemCaseSensitive(request, "action");
        if (!json_number(request, "id", &id) || id < 0 || id > 255 ||
            !cJSON_IsString(action)) return response_error(ESP_ERR_INVALID_ARG);
        esp_err_t error = ESP_ERR_INVALID_ARG;
        if (strcmp(action->valuestring, "start") == 0) error = master_service_start(id);
        else if (strcmp(action->valuestring, "stop") == 0) error = master_service_stop(id);
        else if (strcmp(action->valuestring, "status") == 0) error = master_service_request_status(id);
        else if (strcmp(action->valuestring, "forget") == 0) error = master_service_forget(id);
        else if (strcmp(action->valuestring, "config") == 0) {
            int new_id, sleep_ms, awake_timeout_ms;
            if (json_number(request, "new_id", &new_id) && new_id >= 0 && new_id <= 255 &&
                json_number(request, "sleep_ms", &sleep_ms) &&
                json_number(request, "awake_timeout_ms", &awake_timeout_ms)) {
                error = master_service_set_config(id, new_id, sleep_ms, awake_timeout_ms);
            }
        } else if (strcmp(action->valuestring, "ac") == 0) {
            const cJSON *ac = cJSON_GetObjectItemCaseSensitive(request, "ac");
            int mode, temperature, fan, swing;
            const cJSON *power = cJSON_GetObjectItemCaseSensitive(ac, "power");
            if (cJSON_IsObject(ac) && cJSON_IsBool(power) &&
                json_number(ac, "mode", &mode) && json_number(ac, "temperature", &temperature) &&
                json_number(ac, "fan", &fan) && json_number(ac, "swing", &swing)) {
                smart_ir_ac_state_t state = { .power = cJSON_IsTrue(power), .mode = mode,
                    .temperature_c = temperature, .fan = fan, .swing = swing };
                error = master_service_set_ac(id, &state);
            }
        }
        if (error == ESP_OK) ble_central_kick();
        return error == ESP_OK ? response_base(true) : response_error(error);
    }

    if (strcmp(op->valuestring, "factory_reset") == 0) {
        const cJSON *confirm = cJSON_GetObjectItemCaseSensitive(request, "confirm");
        if (!cJSON_IsTrue(confirm)) return response_error(ESP_ERR_INVALID_ARG);
        factory_reset_schedule();
        return response_base(true);
    }

    return response_error(ESP_ERR_NOT_SUPPORTED);
}

static void send_response(uint8_t message_id, cJSON *response)
{
    char *json = response ? cJSON_PrintUnformatted(response) : NULL;
    cJSON_Delete(response);
    if (!json) json = strdup("{\"ok\":false,\"error\":\"ESP_ERR_NO_MEM\"}");
    if (!json) return;
    strlcpy(s_last_response, json, sizeof(s_last_response));
    size_t length = strlen(s_last_response);
    uint16_t mtu = s_conn_handle == CONFIG_CONN_NONE ? 23 : ble_att_mtu(s_conn_handle);
    size_t chunk_data = mtu > 6 ? mtu - 6 : 17; /* ATT opcode+handle and our 3-byte header. */
    if (chunk_data > CONFIG_CHUNK_DATA_MAX) chunk_data = CONFIG_CHUNK_DATA_MAX;
    uint8_t chunk_count = (uint8_t)((length + chunk_data - 1) / chunk_data);
    if (chunk_count == 0) chunk_count = 1;
    for (uint8_t index = 0; index < chunk_count && s_conn_handle != CONFIG_CONN_NONE; ++index) {
        size_t offset = (size_t)index * chunk_data;
        size_t part = length - offset;
        if (part > chunk_data) part = chunk_data;
        uint8_t packet[3 + CONFIG_CHUNK_DATA_MAX] = { message_id, index, chunk_count };
        memcpy(packet + 3, s_last_response + offset, part);
        struct os_mbuf *om = ble_hs_mbuf_from_flat(packet, part + 3);
        if (!om) break;
        int rc = ble_gatts_notify_custom(s_conn_handle, s_tx_handle, om);
        if (rc != 0) {
            ESP_LOGW(TAG, "Response notification failed: %d", rc);
            break;
        }
    }
    cJSON_free(json);
}

static void process_request(uint8_t message_id)
{
    s_rx_buffer[s_rx_length] = '\0';
    cJSON *request = cJSON_ParseWithLength(s_rx_buffer, s_rx_length);
    cJSON *response = request ? dispatch_request(request) : response_error(ESP_ERR_INVALID_ARG);
    cJSON_Delete(request);
    send_response(message_id, response);
}

static int gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)arg;
    if (attr_handle == s_tx_handle && ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        return os_mbuf_append(ctxt->om, s_last_response, strlen(s_last_response)) == 0
            ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
    uint16_t packet_length = OS_MBUF_PKTLEN(ctxt->om);
    if (packet_length < 3 || packet_length > 3 + CONFIG_CHUNK_DATA_MAX) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    uint8_t packet[3 + CONFIG_CHUNK_DATA_MAX];
    if (os_mbuf_copydata(ctxt->om, 0, packet_length, packet) != 0 ||
        packet[2] == 0 || packet[1] >= packet[2]) return BLE_ATT_ERR_UNLIKELY;
    size_t part = packet_length - 3;
    if (packet[1] == 0) {
        s_rx_message_id = packet[0];
        s_rx_next_chunk = 0;
        s_rx_chunk_count = packet[2];
        s_rx_length = 0;
    }
    if (packet[0] != s_rx_message_id || packet[1] != s_rx_next_chunk ||
        packet[2] != s_rx_chunk_count || s_rx_length + part >= sizeof(s_rx_buffer)) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    memcpy(s_rx_buffer + s_rx_length, packet + 3, part);
    s_rx_length += part;
    ++s_rx_next_chunk;
    if (s_rx_next_chunk == s_rx_chunk_count) process_request(s_rx_message_id);
    return 0;
}

static const struct ble_gatt_svc_def CONFIG_SERVICES[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &CONFIG_SERVICE_UUID.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &CONFIG_RX_UUID.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_WRITE,
            },
            {
                .uuid = &CONFIG_TX_UUID.u,
                .access_cb = gatt_access,
                .val_handle = &s_tx_handle,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
            },
            { 0 }
        },
    },
    { 0 }
};

static int config_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    struct ble_gap_conn_desc desc;
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status != 0) {
                ESP_LOGW(TAG, "Laptop connection failed: %d", event->connect.status);
                ble_config_server_start_window();
                return 0;
            }
            s_conn_handle = event->connect.conn_handle;
            ble_central_set_configuration_active(true);
            ESP_LOGI(TAG, "Laptop connected; concurrent slave activity enabled");
            return 0;
        case BLE_GAP_EVENT_ENC_CHANGE:
            if (event->enc_change.status == 0) {
                ESP_LOGI(TAG, "Laptop configuration link optionally encrypted");
            } else {
                /* Configuration transport is intentionally usable without OS
                 * pairing in dev. A stale Windows bond must not tear down the
                 * Web Bluetooth session. Slave links still require security. */
                ESP_LOGW(TAG, "Optional laptop encryption failed: %d; continuing",
                         event->enc_change.status);
            }
            return 0;
        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "Laptop disconnected: reason=%d; pending slave work prioritized",
                     event->disconnect.reason);
            s_conn_handle = CONFIG_CONN_NONE;
            s_rx_length = 0;
            ble_central_set_configuration_active(false);
            return 0;
        case BLE_GAP_EVENT_ADV_COMPLETE:
            if (s_conn_handle == CONFIG_CONN_NONE) {
                ESP_LOGI(TAG, "Laptop advertising window complete; restarting advertising");
                ble_config_server_start_window();
            }
            return 0;
        case BLE_GAP_EVENT_REPEAT_PAIRING:
            if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
                ble_store_util_delete_peer(&desc.peer_id_addr);
            }
            return BLE_GAP_REPEAT_PAIRING_RETRY;
        case BLE_GAP_EVENT_MTU:
            ESP_LOGI(TAG, "Laptop ATT MTU=%u", event->mtu.value);
            return 0;
        default:
            return 0;
    }
}

void ble_config_server_start_window(void)
{
    if (s_conn_handle != CONFIG_CONN_NONE || ble_gap_adv_active() || ble_gap_disc_active()) return;
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = (ble_uuid128_t *)&CONFIG_SERVICE_UUID;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "Advertising fields failed: %d", rc);
        return;
    }
    struct ble_hs_adv_fields scan_response = {0};
    scan_response.name = (const uint8_t *)s_device_name;
    scan_response.name_len = strlen(s_device_name);
    scan_response.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&scan_response);
    if (rc != 0) {
        ESP_LOGE(TAG, "Scan-response fields failed: %d", rc);
        return;
    }
    const struct ble_gap_adv_params parameters = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
    };
    /* Keep the device selectable long enough for the native Web Bluetooth
     * chooser. A short window can expire between discovery and the user's
     * click, which Windows reports as "Connection failed". */
    rc = ble_gap_adv_start(s_own_addr_type, NULL, 60000, &parameters,
                           config_gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) ESP_LOGE(TAG, "Advertising failed: %d", rc);
    else ESP_LOGI(TAG, "Configuration advertising window as %s", s_device_name);
}

esp_err_t ble_config_server_init(void)
{
    uint8_t mac[6];
    ESP_RETURN_ON_ERROR(esp_read_mac(mac, ESP_MAC_BT), TAG, "read BT MAC");
    snprintf(s_device_name, sizeof(s_device_name), "Smart-IR-Master-%02X%02X%02X",
             mac[3], mac[4], mac[5]);
    strlcpy(s_last_response, "{\"ok\":true}", sizeof(s_last_response));
    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_svc_gap_device_name_set(s_device_name);
    if (rc == 0) rc = ble_gatts_count_cfg(CONFIG_SERVICES);
    if (rc == 0) rc = ble_gatts_add_svcs(CONFIG_SERVICES);
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

void ble_config_server_on_sync(void)
{
    int rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "Cannot infer address type: %d", rc);
        return;
    }
    ble_config_server_start_window();
}

bool ble_config_server_connected(void)
{
    return s_conn_handle != CONFIG_CONN_NONE;
}
