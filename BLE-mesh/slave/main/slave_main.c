#include <inttypes.h>
#include <stdbool.h>
#include <string.h>

#include "ble_mesh_example_init.h"
#include "driver/gpio.h"
#include "esp_ble_mesh_common_api.h"
#include "esp_ble_mesh_config_model_api.h"
#include "esp_ble_mesh_networking_api.h"
#include "esp_ble_mesh_provisioning_api.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "smart_ir_mesh.h"

#define TAG "mesh_slave"
#define CONFIG_NAMESPACE "mesh_slave"
#define CONFIG_KEY "state"
#define CONFIG_VERSION 2
#define UNCONFIGURED_ID 0xff
#define MASTER_ADDR 0x0001
#define APP_IDX 0x0000
#define MSG_TTL 3
#define DEFAULT_SLEEP_MS 10000U
#define DEFAULT_AWAKE_MS 5000U
#define WAKE_HANDSHAKE_TIMEOUT_MS 3000U
#define WAKE_RETRY_INTERVAL_MS 500U
#define SLEEP_TX_GRACE_MS 300U

typedef struct {
    uint32_t version;
    uint8_t id;
    bool running;
    bool ac_power;
    uint8_t ac_mode;
    uint8_t temperature;
    uint8_t fan;
    uint8_t swing;
    uint32_t sleep_ms;
    uint32_t awake_ms;
} slave_state_t;

typedef struct {
    uint32_t version;
    uint8_t id;
    bool running;
    bool ac_power;
    uint8_t ac_mode;
    uint8_t temperature;
    uint8_t fan;
    uint8_t swing;
} slave_state_v1_t;

static uint8_t s_uuid[16] = { SMART_IR_UUID_0, SMART_IR_UUID_1 };
static int64_t s_boot_us;
static TaskHandle_t s_sleep_task;
static volatile bool s_wake_sync_received;
static slave_state_t s_state = {
    .version = CONFIG_VERSION, .id = UNCONFIGURED_ID, .running = false,
    .temperature = 24, .sleep_ms = DEFAULT_SLEEP_MS, .awake_ms = DEFAULT_AWAKE_MS,
};

static esp_ble_mesh_cfg_srv_t s_cfg_srv = {
    .net_transmit = ESP_BLE_MESH_TRANSMIT(2, 20),
    .relay = ESP_BLE_MESH_RELAY_ENABLED,
    .relay_retransmit = ESP_BLE_MESH_TRANSMIT(2, 20),
    .beacon = ESP_BLE_MESH_BEACON_ENABLED,
#if defined(CONFIG_BLE_MESH_GATT_PROXY_SERVER)
    .gatt_proxy = ESP_BLE_MESH_GATT_PROXY_ENABLED,
#else
    .gatt_proxy = ESP_BLE_MESH_GATT_PROXY_NOT_SUPPORTED,
#endif
    .friend_state = ESP_BLE_MESH_FRIEND_NOT_SUPPORTED,
    .default_ttl = 7,
};
static esp_ble_mesh_model_t s_root_models[] = {
    ESP_BLE_MESH_MODEL_CFG_SRV(&s_cfg_srv),
};
static esp_ble_mesh_model_op_t s_vendor_ops[] = {
    ESP_BLE_MESH_MODEL_OP(SMART_IR_OP_COMMAND, 3),
    ESP_BLE_MESH_MODEL_OP_END,
};
static esp_ble_mesh_model_t s_vendor_models[] = {
    ESP_BLE_MESH_VENDOR_MODEL(SMART_IR_CID, SMART_IR_SERVER_MODEL_ID,
                              s_vendor_ops, NULL, NULL),
};
static esp_ble_mesh_elem_t s_elements[] = {
    ESP_BLE_MESH_ELEMENT(0, s_root_models, s_vendor_models),
};
static esp_ble_mesh_comp_t s_comp = {
    .cid = SMART_IR_CID, .element_count = ARRAY_SIZE(s_elements), .elements = s_elements,
};
static esp_ble_mesh_prov_t s_prov = { .uuid = s_uuid };

static void state_save(void)
{
    nvs_handle_t h;
    if (nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, CONFIG_KEY, &s_state, sizeof(s_state));
        nvs_commit(h);
        nvs_close(h);
    }
}

static void state_load(void)
{
    nvs_handle_t h;
    bool migrated = false;
    if (nvs_open(CONFIG_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        size_t length = 0;
        if (nvs_get_blob(h, CONFIG_KEY, NULL, &length) == ESP_OK) {
            if (length == sizeof(slave_state_t)) {
                slave_state_t loaded = {0};
                if (nvs_get_blob(h, CONFIG_KEY, &loaded, &length) == ESP_OK &&
                    loaded.version == CONFIG_VERSION) {
                    s_state = loaded;
                }
            } else if (length == sizeof(slave_state_v1_t)) {
                slave_state_v1_t old = {0};
                if (nvs_get_blob(h, CONFIG_KEY, &old, &length) == ESP_OK && old.version == 1) {
                    s_state.id = old.id;
                    /* Protocol v1 used running as a logical gate. Do not turn
                     * that old value into an automatic deep-sleep cycle. */
                    s_state.running = false;
                    s_state.ac_power = old.ac_power;
                    s_state.ac_mode = old.ac_mode;
                    s_state.temperature = old.temperature;
                    s_state.fan = old.fan;
                    s_state.swing = old.swing;
                    migrated = true;
                    ESP_LOGI(TAG, "Migrated slave state to schema %u", CONFIG_VERSION);
                }
            }
        }
        nvs_close(h);
    }
    if (migrated) state_save();
}

static uint32_t read_u32_le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static esp_err_t send_response(esp_ble_mesh_msg_ctx_t *ctx, uint8_t sequence,
                               uint8_t event, const uint8_t *payload, size_t payload_len)
{
    uint8_t message[16] = { sequence, s_state.id, event };
    if (payload_len > sizeof(message) - 3) return ESP_ERR_INVALID_SIZE;
    if (payload_len) memcpy(message + 3, payload, payload_len);
    return esp_ble_mesh_server_model_send_msg(&s_vendor_models[0], ctx,
                                               SMART_IR_OP_STATUS,
                                               3 + payload_len, message);
}

static esp_err_t send_lifecycle_event(uint8_t event)
{
    uint8_t message[3] = { 0, s_state.id, event };
    esp_ble_mesh_msg_ctx_t ctx = {
        .net_idx = ESP_BLE_MESH_KEY_PRIMARY,
        .app_idx = APP_IDX,
        .addr = MASTER_ADDR,
        .send_ttl = MSG_TTL,
    };
    return esp_ble_mesh_server_model_send_msg(&s_vendor_models[0], &ctx,
                                               SMART_IR_OP_LIFECYCLE,
                                               sizeof(message), message);
}

static void cancel_scheduled_sleep(void)
{
    if (s_sleep_task) {
        vTaskDelete(s_sleep_task);
        s_sleep_task = NULL;
    }
}

static void deep_sleep_task(void *arg)
{
    uint32_t delay_ms = (uint32_t)(uintptr_t)arg;
    if (delay_ms) vTaskDelay(pdMS_TO_TICKS(delay_ms));
    s_sleep_task = NULL;

    if (!s_state.running) {
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Entering deep sleep for %" PRIu32 " ms", s_state.sleep_ms);
    ESP_ERROR_CHECK_WITHOUT_ABORT(send_lifecycle_event(SMART_IR_EVT_SLEEP));
    vTaskDelay(pdMS_TO_TICKS(SLEEP_TX_GRACE_MS));

    /* STOP/WAKE_SYNC may arrive while the final SLEEP event is being sent. */
    if (!s_state.running) {
        ESP_LOGI(TAG, "Deep sleep cancelled; slave remains awake in idle mode");
        vTaskDelete(NULL);
        return;
    }

    ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup((uint64_t)s_state.sleep_ms * 1000ULL));
    esp_deep_sleep_start();
}

static void schedule_sleep(uint32_t delay_ms)
{
    cancel_scheduled_sleep();
    if (!s_state.running) return;
    BaseType_t created = xTaskCreate(deep_sleep_task, "mesh_sleep", 3072,
                                     (void *)(uintptr_t)delay_ms, 3, &s_sleep_task);
    if (created != pdPASS) {
        s_sleep_task = NULL;
        ESP_LOGE(TAG, "Unable to create deep-sleep task");
    }
}

static void wake_announce_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(750));
    if (s_state.running && s_state.id != UNCONFIGURED_ID &&
        esp_ble_mesh_node_is_provisioned()) {
        uint32_t wake_window_ms = s_state.awake_ms == 0 ?
                                  WAKE_HANDSHAKE_TIMEOUT_MS : s_state.awake_ms;
        ESP_LOGI(TAG, "Wake window opened (awake=%" PRIu32 " ms)", s_state.awake_ms);
        s_wake_sync_received = false;
        schedule_sleep(wake_window_ms);

        /* WAKE is an unacknowledged vendor event. Repeat it until the master
         * answers with WAKE_SYNC so one lost/replay-filtered packet cannot
         * strand commands queued while this node was sleeping. */
        while (s_state.running && !s_wake_sync_received) {
            esp_err_t err = send_lifecycle_event(SMART_IR_EVT_WAKE);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Wake announcement failed: %s", esp_err_to_name(err));
            }
            vTaskDelay(pdMS_TO_TICKS(WAKE_RETRY_INTERVAL_MS));
        }
    }
    vTaskDelete(NULL);
}

static void send_nack(esp_ble_mesh_msg_ctx_t *ctx, uint8_t sequence, uint8_t error)
{
    ESP_LOGW(TAG, "NACK seq=%u error=0x%02x", sequence, error);
    ESP_ERROR_CHECK_WITHOUT_ABORT(send_response(ctx, sequence, SMART_IR_EVT_NACK, &error, 1));
}

static bool valid_ac(const uint8_t *p)
{
    return p[0] <= 1 && p[1] <= 4 && p[2] >= 16 && p[2] <= 30 &&
           p[3] <= 5 && p[4] <= 3;
}

static void handle_command(esp_ble_mesh_msg_ctx_t *ctx, const uint8_t *message, uint16_t length)
{
    uint8_t sequence = message[0];
    uint8_t target_id = message[1];
    uint8_t command = message[2];
    const uint8_t *payload = message + 3;
    uint16_t payload_len = length - 3;
    ESP_LOGI(TAG, "RX cmd=0x%02x seq=%u target=%u len=%u", command, sequence, target_id, payload_len);

    if (command != SMART_IR_CMD_SET_CONFIG && target_id != s_state.id) {
        send_nack(ctx, sequence, SMART_IR_NACK_INVALID_ID);
        return;
    }
    switch (command) {
    case SMART_IR_CMD_SET_CONFIG:
        if (payload_len != 1 || payload[0] < 1 || target_id != payload[0]) {
            send_nack(ctx, sequence, SMART_IR_NACK_INVALID_VALUE);
            return;
        }
        s_state.id = payload[0];
        state_save();
        ESP_LOGI(TAG, "Configured as slave %u", s_state.id);
        ESP_ERROR_CHECK_WITHOUT_ABORT(send_response(ctx, sequence, SMART_IR_EVT_ACK, NULL, 0));
        break;
    case SMART_IR_CMD_SET_TIMING:
        if (payload_len != 8) { send_nack(ctx, sequence, SMART_IR_NACK_INVALID_LENGTH); return; }
        if (s_state.running) { send_nack(ctx, sequence, SMART_IR_NACK_BUSY); return; }
        s_state.sleep_ms = read_u32_le(payload);
        s_state.awake_ms = read_u32_le(payload + 4);
        if (s_state.sleep_ms == 0) { send_nack(ctx, sequence, SMART_IR_NACK_INVALID_VALUE); return; }
        state_save();
        ESP_LOGI(TAG, "Timing configured: sleep=%" PRIu32 " ms awake=%" PRIu32 " ms",
                 s_state.sleep_ms, s_state.awake_ms);
        ESP_ERROR_CHECK_WITHOUT_ABORT(send_response(ctx, sequence, SMART_IR_EVT_ACK, NULL, 0));
        break;
    case SMART_IR_CMD_START:
        if (payload_len != 0) { send_nack(ctx, sequence, SMART_IR_NACK_INVALID_LENGTH); return; }
        s_state.running = true;
        state_save();
        ESP_ERROR_CHECK_WITHOUT_ABORT(send_response(ctx, sequence, SMART_IR_EVT_ACK, NULL, 0));
        schedule_sleep(SLEEP_TX_GRACE_MS * 2);
        break;
    case SMART_IR_CMD_STOP:
        if (payload_len != 0) { send_nack(ctx, sequence, SMART_IR_NACK_INVALID_LENGTH); return; }
        s_state.running = false;
        state_save();
        cancel_scheduled_sleep();
        ESP_LOGI(TAG, "Cycle stopped; slave remains awake in idle mode");
        ESP_ERROR_CHECK_WITHOUT_ABORT(send_response(ctx, sequence, SMART_IR_EVT_ACK, NULL, 0));
        break;
    case SMART_IR_CMD_WAKE_SYNC: {
        if (payload_len < 1) { send_nack(ctx, sequence, SMART_IR_NACK_INVALID_LENGTH); return; }
        uint8_t flags = payload[0];
        if (flags & ~(SMART_IR_WAKE_FLAG_STOP | SMART_IR_WAKE_FLAG_AC)) {
            send_nack(ctx, sequence, SMART_IR_NACK_INVALID_VALUE);
            return;
        }
        if (flags & SMART_IR_WAKE_FLAG_STOP) {
            if (payload_len != 1) { send_nack(ctx, sequence, SMART_IR_NACK_INVALID_LENGTH); return; }
            s_wake_sync_received = true;
            s_state.running = false;
            state_save();
            cancel_scheduled_sleep();
            ESP_LOGI(TAG, "Queued stop applied; slave remains awake in idle mode");
            ESP_ERROR_CHECK_WITHOUT_ABORT(send_response(ctx, sequence, SMART_IR_EVT_ACK, NULL, 0));
        } else if (flags & SMART_IR_WAKE_FLAG_AC) {
            if (payload_len != 6) { send_nack(ctx, sequence, SMART_IR_NACK_INVALID_LENGTH); return; }
            if (!valid_ac(payload + 1)) { send_nack(ctx, sequence, SMART_IR_NACK_INVALID_VALUE); return; }
            s_wake_sync_received = true;
            s_state.ac_power = payload[1];
            s_state.ac_mode = payload[2];
            s_state.temperature = payload[3];
            s_state.fan = payload[4];
            s_state.swing = payload[5];
            state_save();
            send_nack(ctx, sequence, SMART_IR_NACK_UNSUPPORTED);
            schedule_sleep(SLEEP_TX_GRACE_MS * 2);
        } else {
            if (payload_len != 1) { send_nack(ctx, sequence, SMART_IR_NACK_INVALID_LENGTH); return; }
            s_wake_sync_received = true;
            ESP_ERROR_CHECK_WITHOUT_ABORT(send_response(ctx, sequence, SMART_IR_EVT_ACK, NULL, 0));
            if (s_state.awake_ms == 0) schedule_sleep(SLEEP_TX_GRACE_MS * 2);
        }
        break;
    }
    case SMART_IR_CMD_GET_STATUS: {
        if (payload_len != 0) { send_nack(ctx, sequence, SMART_IR_NACK_INVALID_LENGTH); return; }
        uint32_t uptime = (uint32_t)((esp_timer_get_time() - s_boot_us) / 1000);
        uint8_t status[7] = {
            s_state.running, s_state.ac_power,
            (uint8_t)uptime, (uint8_t)(uptime >> 8),
            (uint8_t)(uptime >> 16), (uint8_t)(uptime >> 24),
            s_state.temperature,
        };
        ESP_ERROR_CHECK_WITHOUT_ABORT(send_response(ctx, sequence, SMART_IR_EVT_STATUS, status, sizeof(status)));
        break;
    }
    case SMART_IR_CMD_SET_AC_STATE:
        if (payload_len != 5) { send_nack(ctx, sequence, SMART_IR_NACK_INVALID_LENGTH); return; }
        if (!valid_ac(payload)) { send_nack(ctx, sequence, SMART_IR_NACK_INVALID_VALUE); return; }
        /* The exact Daikin family is intentionally not guessed. Keep the
         * requested state for integration tests and report UNSUPPORTED, just
         * like the P2P sample's current IR port. */
        s_state.ac_power = payload[0];
        s_state.ac_mode = payload[1];
        s_state.temperature = payload[2];
        s_state.fan = payload[3];
        s_state.swing = payload[4];
        state_save();
        send_nack(ctx, sequence, SMART_IR_NACK_UNSUPPORTED);
        if (s_state.running) schedule_sleep(SLEEP_TX_GRACE_MS * 2);
        break;
    default:
        send_nack(ctx, sequence, SMART_IR_NACK_INVALID_COMMAND);
        break;
    }
}

static void provisioning_cb(esp_ble_mesh_prov_cb_event_t event,
                            esp_ble_mesh_prov_cb_param_t *param)
{
    switch (event) {
    case ESP_BLE_MESH_NODE_PROV_COMPLETE_EVT:
        ESP_LOGI(TAG, "Provisioned: net=0x%03x addr=0x%04x",
                 param->node_prov_complete.net_idx, param->node_prov_complete.addr);
        break;
    case ESP_BLE_MESH_NODE_PROV_RESET_EVT:
        ESP_LOGW(TAG, "Mesh provisioning reset; advertising again");
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_ble_mesh_node_prov_enable(
            (esp_ble_mesh_prov_bearer_t)(ESP_BLE_MESH_PROV_ADV | ESP_BLE_MESH_PROV_GATT)));
        break;
    default:
        break;
    }
}

static void config_server_cb(esp_ble_mesh_cfg_server_cb_event_t event,
                             esp_ble_mesh_cfg_server_cb_param_t *param)
{
    if (event == ESP_BLE_MESH_CFG_SERVER_STATE_CHANGE_EVT) {
        ESP_LOGI(TAG, "Configuration opcode 0x%04" PRIx32 " applied", param->ctx.recv_op);
    }
}

static void custom_model_cb(esp_ble_mesh_model_cb_event_t event,
                            esp_ble_mesh_model_cb_param_t *param)
{
    if (event == ESP_BLE_MESH_MODEL_OPERATION_EVT &&
        param->model_operation.opcode == SMART_IR_OP_COMMAND &&
        param->model_operation.length >= 3) {
        handle_command(param->model_operation.ctx, param->model_operation.msg,
                       param->model_operation.length);
    } else if (event == ESP_BLE_MESH_MODEL_SEND_COMP_EVT &&
               param->model_send_comp.err_code != 0) {
        ESP_LOGE(TAG, "Response send failed: %d", param->model_send_comp.err_code);
    }
}

static esp_err_t mesh_init(void)
{
    esp_ble_mesh_register_prov_callback(provisioning_cb);
    esp_ble_mesh_register_config_server_callback(config_server_cb);
    esp_ble_mesh_register_custom_model_callback(custom_model_cb);
    ESP_RETURN_ON_ERROR(esp_ble_mesh_init(&s_prov, &s_comp), TAG, "mesh init");
    ESP_RETURN_ON_ERROR(esp_ble_mesh_node_prov_enable(
        (esp_ble_mesh_prov_bearer_t)(ESP_BLE_MESH_PROV_ADV | ESP_BLE_MESH_PROV_GATT)), TAG, "node enable");
    return ESP_OK;
}

void app_main(void)
{
    ESP_LOGI(TAG, "Smart IR Slave %s, protocol v%u, environment %s",
             esp_app_get_description()->version, SMART_IR_PROTOCOL_VERSION,
             SMART_IR_ENVIRONMENT);
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    state_load();
    gpio_config_t gpio = {
        .pin_bit_mask = 1ULL << CONFIG_SMART_IR_IR_TX_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&gpio));
    gpio_set_level(CONFIG_SMART_IR_IR_TX_GPIO, 0);
    s_boot_us = esp_timer_get_time();
    ESP_ERROR_CHECK(bluetooth_init());
    ble_mesh_get_dev_uuid(s_uuid);
    ESP_ERROR_CHECK(mesh_init());
    ESP_LOGI(TAG, "BLE Mesh slave ready (id=%u, running=%s, IR GPIO=%d)",
             s_state.id, s_state.running ? "yes" : "no", CONFIG_SMART_IR_IR_TX_GPIO);
    if (s_state.running) {
        xTaskCreate(wake_announce_task, "mesh_wake", 3072, NULL, 3, NULL);
    }
}
