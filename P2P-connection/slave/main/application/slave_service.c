#include "application/slave_service.h"

#include <string.h>
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "infrastructure/config_store.h"
#include "infrastructure/ir_transmitter.h"
#include "nvs.h"
#include "sdkconfig.h"

#define SLEEP_NOTIFY_IDLE   (1U << 0)
#define SLEEP_NOTIFY_SAFETY (1U << 1)

static const char *TAG = "slave_service";
static slave_config_t s_config;
static slave_service_ports_t s_ports;
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_sleep_task;
static esp_timer_handle_t s_idle_timer;
static esp_timer_handle_t s_safety_timer;
static int64_t s_boot_time_us;
static bool s_subscribed;

static slave_config_t default_config(void)
{
    return (slave_config_t) {
        .schema_version = SLAVE_CONFIG_SCHEMA_VERSION,
        .provisioned = false,
        .running = true,
        .id = SLAVE_UNPROVISIONED_ID,
        .sleep_ms = 1000,
        .awake_timeout_ms = 10000,
        .last_ac_state = {
            .power = false,
            .mode = SMART_IR_AC_MODE_AUTO,
            .temperature_c = 24,
            .fan = 0,
            .swing = SMART_IR_SWING_OFF,
        },
    };
}

static void timer_callback(void *arg)
{
    if (s_sleep_task) xTaskNotify(s_sleep_task, (uint32_t)(uintptr_t)arg,
                                  eSetBits);
}

static void stop_sleep_timers(void)
{
    esp_timer_stop(s_idle_timer);
    esp_timer_stop(s_safety_timer);
}

static void arm_safety_timer_locked(void)
{
    esp_timer_stop(s_safety_timer);
    if (s_config.provisioned && s_config.running && s_config.awake_timeout_ms > 0) {
        esp_timer_start_once(s_safety_timer,
                             (uint64_t)s_config.awake_timeout_ms * 1000ULL);
    }
}

static void arm_idle_timer_locked(void)
{
    esp_timer_stop(s_idle_timer);
    if (s_config.provisioned && s_config.running && s_subscribed) {
        esp_timer_start_once(s_idle_timer,
                             (uint64_t)CONFIG_SMART_IR_TRANSACTION_IDLE_MS * 1000ULL);
    }
}

static esp_err_t send_event(uint8_t event, uint8_t sequence,
                            const uint8_t *payload, uint16_t payload_length)
{
    smart_ir_frame_t response = {
        .sequence = sequence,
        .slave_id = s_config.provisioned ? s_config.id : SLAVE_UNPROVISIONED_ID,
        .command = event,
        .payload_length = payload_length,
    };
    if (payload_length && payload) memcpy(response.payload, payload, payload_length);
    return s_ports.send_frame ? s_ports.send_frame(&response) : ESP_ERR_INVALID_STATE;
}

static void send_nack(uint8_t sequence, smart_ir_nack_t error)
{
    const uint8_t payload[] = { (uint8_t)error };
    ESP_LOGW(TAG, "NACK seq=%u error=0x%02X", sequence, error);
    (void)send_event(SMART_IR_EVT_NACK, sequence, payload, sizeof(payload));
}

static void fill_status(uint8_t payload[8])
{
    uint32_t uptime_ms = (uint32_t)((esp_timer_get_time() - s_boot_time_us) / 1000);
    payload[0] = s_config.running ? 1 : 0;
    payload[1] = (uint8_t)esp_sleep_get_wakeup_cause();
    payload[2] = (uint8_t)uptime_ms;
    payload[3] = (uint8_t)(uptime_ms >> 8);
    payload[4] = (uint8_t)(uptime_ms >> 16);
    payload[5] = (uint8_t)(uptime_ms >> 24);
    payload[6] = 0; /* Battery ADC is board-specific and not configured yet. */
    payload[7] = 0;
}

static void sleep_task(void *arg)
{
    (void)arg;
    while (true) {
        uint32_t reason = 0;
        xTaskNotifyWait(0, UINT32_MAX, &reason, portMAX_DELAY);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool should_sleep = s_config.provisioned && s_config.running;
        uint16_t sleep_ms = s_config.sleep_ms;
        if (should_sleep) stop_sleep_timers();
        xSemaphoreGive(s_lock);
        if (!should_sleep) continue;

        uint8_t payload[2] = { (reason & SLEEP_NOTIFY_SAFETY) ? 1 : 0,
                               (uint8_t)(sleep_ms > 0) };
        ESP_LOGI(TAG, "PRE_SLEEP_STATUS reason=%s, deep_sleep=%u ms",
                 (reason & SLEEP_NOTIFY_SAFETY) ? "awake-timeout" : "transaction-complete",
                 sleep_ms);
        if (s_subscribed) {
            (void)send_event(SMART_IR_EVT_PRE_SLEEP_STATUS, 0, payload, sizeof(payload));
            vTaskDelay(pdMS_TO_TICKS(120));
        }
        if (s_ports.enter_deep_sleep) s_ports.enter_deep_sleep(sleep_ms);
    }
}

esp_err_t slave_service_init(const slave_service_ports_t *ports)
{
    if (!ports || !ports->send_frame || !ports->enter_deep_sleep) {
        return ESP_ERR_INVALID_ARG;
    }
    s_ports = *ports;
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    s_boot_time_us = esp_timer_get_time();
    s_config = default_config();
    esp_err_t err = config_store_load(&s_config);
    if (err == ESP_ERR_NVS_NOT_FOUND || err == ESP_ERR_NVS_NOT_INITIALIZED ||
        err == ESP_ERR_INVALID_VERSION) {
        s_config = default_config();
        err = ESP_OK;
    }
    if (err != ESP_OK) return err;

    const esp_timer_create_args_t idle_args = {
        .callback = timer_callback, .arg = (void *)SLEEP_NOTIFY_IDLE,
        .name = "slave_idle",
    };
    const esp_timer_create_args_t safety_args = {
        .callback = timer_callback, .arg = (void *)SLEEP_NOTIFY_SAFETY,
        .name = "slave_safety",
    };
    ESP_ERROR_CHECK(esp_timer_create(&idle_args, &s_idle_timer));
    ESP_ERROR_CHECK(esp_timer_create(&safety_args, &s_safety_timer));
    if (xTaskCreate(sleep_task, "slave_sleep", 3072, NULL, 5, &s_sleep_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    arm_safety_timer_locked();
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "Configuration: provisioned=%s id=%u running=%s sleep=%u ms awake=%lu ms",
             s_config.provisioned ? "yes" : "no", s_config.id,
             s_config.running ? "yes" : "no", s_config.sleep_ms,
             (unsigned long)s_config.awake_timeout_ms);
    return ESP_OK;
}

void slave_service_on_subscribed(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_subscribed = true;
    uint8_t status[8];
    fill_status(status);
    ESP_LOGI(TAG, "WAKE_STATUS id=%u running=%s", s_config.id,
             s_config.running ? "yes" : "no");
    (void)send_event(SMART_IR_EVT_WAKE_STATUS, 0, status, sizeof(status));
    /* Give the central the full safety-awake window to finish encryption and
       GATT discovery before the first command.  The short idle timer is only
       appropriate after a command response, when it is used to drain the
       remainder of the master's queue. */
    arm_safety_timer_locked();
    xSemaphoreGive(s_lock);
}

void slave_service_on_disconnected(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_subscribed = false;
    esp_timer_stop(s_idle_timer);
    arm_safety_timer_locked();
    xSemaphoreGive(s_lock);
}

void slave_service_handle_frame(const smart_ir_frame_t *frame)
{
    if (!frame) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_timer_stop(s_idle_timer);
    ESP_LOGI(TAG, "RX cmd=0x%02X seq=%u id=%u len=%u", frame->command,
             frame->sequence, frame->slave_id, frame->payload_length);

    if (s_config.provisioned && frame->slave_id != s_config.id) {
        send_nack(frame->sequence, SMART_IR_NACK_INVALID_ID);
        arm_idle_timer_locked();
        xSemaphoreGive(s_lock);
        return;
    }

    switch (frame->command) {
        case SMART_IR_CMD_SET_CONFIG: {
            if (frame->payload_length != 7) {
                send_nack(frame->sequence, SMART_IR_NACK_INVALID_LENGTH);
                break;
            }
            uint16_t sleep_ms = (uint16_t)frame->payload[1] |
                                ((uint16_t)frame->payload[2] << 8);
            uint32_t awake_ms = (uint32_t)frame->payload[3] |
                ((uint32_t)frame->payload[4] << 8) |
                ((uint32_t)frame->payload[5] << 16) |
                ((uint32_t)frame->payload[6] << 24);
            if (sleep_ms < 1 || sleep_ms > 1999 ||
                (awake_ms != 0 && (awake_ms < 2000 || awake_ms > 30000))) {
                send_nack(frame->sequence, SMART_IR_NACK_INVALID_VALUE);
                break;
            }
            slave_config_t previous = s_config;
            s_config.provisioned = true;
            s_config.id = frame->payload[0];
            s_config.sleep_ms = sleep_ms;
            s_config.awake_timeout_ms = awake_ms;
            esp_err_t err = config_store_save(&s_config);
            if (err != ESP_OK) {
                s_config = previous;
                send_nack(frame->sequence, SMART_IR_NACK_BUSY);
                break;
            }
            ESP_LOGI(TAG, "Configuration persisted: id=%u sleep=%u ms awake=%lu ms",
                     s_config.id, s_config.sleep_ms,
                     (unsigned long)s_config.awake_timeout_ms);
            (void)send_event(SMART_IR_EVT_ACK, frame->sequence, NULL, 0);
            if (s_ports.refresh_advertising) s_ports.refresh_advertising();
            arm_safety_timer_locked();
            break;
        }
        case SMART_IR_CMD_START:
        case SMART_IR_CMD_STOP: {
            if (frame->payload_length != 0) {
                send_nack(frame->sequence, SMART_IR_NACK_INVALID_LENGTH);
                break;
            }
            bool previous = s_config.running;
            s_config.running = frame->command == SMART_IR_CMD_START;
            if (config_store_save(&s_config) != ESP_OK) {
                s_config.running = previous;
                send_nack(frame->sequence, SMART_IR_NACK_BUSY);
                break;
            }
            ESP_LOGI(TAG, "Program state=%s", s_config.running ? "STARTED" : "STOPPED");
            (void)send_event(SMART_IR_EVT_ACK, frame->sequence, NULL, 0);
            if (s_ports.refresh_advertising) s_ports.refresh_advertising();
            if (s_config.running) arm_safety_timer_locked();
            else stop_sleep_timers();
            break;
        }
        case SMART_IR_CMD_GET_STATUS: {
            if (frame->payload_length != 0) {
                send_nack(frame->sequence, SMART_IR_NACK_INVALID_LENGTH);
                break;
            }
            uint8_t status[8];
            fill_status(status);
            (void)send_event(SMART_IR_EVT_ACK, frame->sequence, status, sizeof(status));
            break;
        }
        case SMART_IR_CMD_SET_AC_STATE: {
            if (frame->payload_length != 5) {
                send_nack(frame->sequence, SMART_IR_NACK_INVALID_LENGTH);
                break;
            }
            if (!s_config.running) {
                send_nack(frame->sequence, SMART_IR_NACK_PROGRAM_STOPPED);
                break;
            }
            smart_ir_ac_state_t state = {
                .power = frame->payload[0] != 0,
                .mode = frame->payload[1],
                .temperature_c = frame->payload[2],
                .fan = frame->payload[3],
                .swing = frame->payload[4],
            };
            if (!smart_ir_ac_state_valid(&state)) {
                send_nack(frame->sequence, SMART_IR_NACK_INVALID_VALUE);
                break;
            }
            esp_err_t err = ir_transmitter_send_daikin(&state);
            if (err == ESP_ERR_NOT_SUPPORTED) {
                send_nack(frame->sequence, SMART_IR_NACK_UNSUPPORTED);
            } else if (err != ESP_OK) {
                send_nack(frame->sequence, SMART_IR_NACK_IR_FAILURE);
            } else {
                s_config.last_ac_state = state;
                (void)config_store_save(&s_config);
                const uint8_t result[] = { 1 };
                (void)send_event(SMART_IR_EVT_AC_RESULT, frame->sequence,
                                 result, sizeof(result));
            }
            break;
        }
        default:
            send_nack(frame->sequence, SMART_IR_NACK_INVALID_COMMAND);
            break;
    }
    arm_idle_timer_locked();
    xSemaphoreGive(s_lock);
}

void slave_service_snapshot(slave_config_t *config)
{
    if (!config || !s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *config = s_config;
    xSemaphoreGive(s_lock);
}

uint8_t slave_service_advertising_flags(void)
{
    if (!s_lock) return 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint8_t flags = (s_config.provisioned ? SMART_IR_ADV_FLAG_PROVISIONED : 0) |
                    (s_config.running ? SMART_IR_ADV_FLAG_RUNNING : 0);
    xSemaphoreGive(s_lock);
    return flags;
}
