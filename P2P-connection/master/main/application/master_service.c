#include "application/master_service.h"
#include <stdlib.h>
#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "infrastructure/config_store.h"

static const char *TAG = "master_service";
static master_persistent_config_t s_config;
static master_discovered_t s_discovered[MASTER_MAX_DISCOVERED];
static bool s_configuration_confirmed[MASTER_MAX_SLAVES];
static SemaphoreHandle_t s_lock;

static esp_err_t enqueue_locked(master_slave_t *slave, uint8_t command,
                                const uint8_t *payload, uint16_t length);

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static master_slave_t *find_by_id(uint8_t id)
{
    for (size_t i = 0; i < MASTER_MAX_SLAVES; ++i) {
        if (s_config.slaves[i].used && s_config.slaves[i].id == id) {
            return &s_config.slaves[i];
        }
    }
    return NULL;
}

static master_slave_t *find_by_address(const master_ble_address_t *address)
{
    for (size_t i = 0; i < MASTER_MAX_SLAVES; ++i) {
        if (s_config.slaves[i].used &&
            master_ble_address_equal(&s_config.slaves[i].address, address)) {
            return &s_config.slaves[i];
        }
    }
    return NULL;
}

static bool id_reserved_by_other(uint8_t id, const master_slave_t *owner)
{
    for (size_t i = 0; i < MASTER_MAX_SLAVES; ++i) {
        const master_slave_t *slave = &s_config.slaves[i];
        if (!slave->used || slave == owner) continue;
        if (slave->id == id) return true;
        for (uint8_t q = 0; q < slave->queue_count && q < MASTER_QUEUE_DEPTH; ++q) {
            const smart_ir_frame_t *pending = &slave->queue[q].frame;
            if (pending->command == SMART_IR_CMD_SET_CONFIG &&
                pending->payload_length == 7 && pending->payload[0] == id) {
                return true;
            }
        }
    }
    return false;
}

static esp_err_t save_config_locked(void)
{
    esp_err_t err = config_store_save(&s_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not save configuration: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t master_service_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }
    ESP_RETURN_ON_ERROR(config_store_load(&s_config), TAG, "load config");
    for (size_t i = 0; i < MASTER_MAX_SLAVES; ++i) {
        master_slave_t *slave = &s_config.slaves[i];
        slave->connected = false;
        slave->awake = false;
        if (slave->queue_count > MASTER_QUEUE_DEPTH) {
            ESP_LOGW(TAG, "Discarding invalid persisted queue for slave slot %u",
                     (unsigned)i);
            slave->queue_count = 0;
            memset(slave->queue, 0, sizeof(slave->queue));
        }
        for (uint8_t q = 0; q < slave->queue_count; ++q) {
            slave->queue[q].used = true;
            slave->queue[q].queued_at_ms = now_ms();
        }
        if (slave->used && slave->awake_timeout_ms == 0) {
            slave->awake_timeout_ms = 10000;
        }
        /* Preserve pending commands. Re-assert the current configuration only
           when no configuration command is already waiting, so an
           unacknowledged ID change is not overwritten after a reboot. */
        if (slave->used) {
            bool config_pending = false;
            for (uint8_t q = 0; q < slave->queue_count; ++q) {
                config_pending |= slave->queue[q].frame.command == SMART_IR_CMD_SET_CONFIG;
            }
            if (!config_pending) {
                const uint32_t awake = slave->awake_timeout_ms;
                const uint16_t sleep = slave->sleep_ms;
                uint8_t payload[7] = { slave->id, (uint8_t)sleep, (uint8_t)(sleep >> 8),
                    (uint8_t)awake, (uint8_t)(awake >> 8), (uint8_t)(awake >> 16),
                    (uint8_t)(awake >> 24) };
                enqueue_locked(slave, SMART_IR_CMD_SET_CONFIG, payload, sizeof(payload));
            }
        }
    }
    save_config_locked();
    return ESP_OK;
}

void master_service_discovered(const master_ble_address_t *address, int8_t rssi,
                               const smart_ir_advertisement_t *advertisement)
{
    if (!address || !advertisement || !s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    master_discovered_t *slot = NULL;
    master_discovered_t *oldest = &s_discovered[0];
    for (size_t i = 0; i < MASTER_MAX_DISCOVERED; ++i) {
        if (s_discovered[i].used && master_ble_address_equal(&s_discovered[i].address, address)) {
            slot = &s_discovered[i];
            break;
        }
        if (!s_discovered[i].used && !slot) slot = &s_discovered[i];
        if (s_discovered[i].last_seen_ms < oldest->last_seen_ms) oldest = &s_discovered[i];
    }
    if (!slot) slot = oldest;
    slot->used = true;
    slot->address = *address;
    slot->rssi = rssi;
    slot->advertised_id = advertisement->slave_id;
    slot->provisioned_flag = (advertisement->flags & SMART_IR_ADV_FLAG_PROVISIONED) != 0;
    slot->running_flag = (advertisement->flags & SMART_IR_ADV_FLAG_RUNNING) != 0;
    slot->last_seen_ms = now_ms();
    master_slave_t *registered = find_by_address(address);
    if (registered) {
        registered->rssi = rssi;
        if (advertisement->slave_id == registered->id &&
            (advertisement->flags & SMART_IR_ADV_FLAG_PROVISIONED) != 0) {
            s_configuration_confirmed[registered - s_config.slaves] = true;
        }
    }
    xSemaphoreGive(s_lock);
}

void master_service_clear_discovered(void)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(s_discovered, 0, sizeof(s_discovered));
    xSemaphoreGive(s_lock);
}

size_t master_service_discovered_snapshot(master_discovered_t *output, size_t capacity)
{
    if (!output || !s_lock) return 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t count = 0;
    for (size_t i = 0; i < MASTER_MAX_DISCOVERED && count < capacity; ++i) {
        if (s_discovered[i].used) output[count++] = s_discovered[i];
    }
    xSemaphoreGive(s_lock);
    return count;
}

size_t master_service_slaves_snapshot(master_slave_view_t *output, size_t capacity)
{
    if (!output || !s_lock) return 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t count = 0;
    for (size_t i = 0; i < MASTER_MAX_SLAVES && count < capacity; ++i) {
        if (s_config.slaves[i].used) {
            const master_slave_t *source = &s_config.slaves[i];
            master_slave_view_t *view = &output[count++];
            bool config_pending = false;
            for (uint8_t q = 0; q < source->queue_count; ++q) {
                config_pending |= source->queue[q].frame.command == SMART_IR_CMD_SET_CONFIG;
            }
            const master_discovered_t *advertised = NULL;
            for (size_t d = 0; d < MASTER_MAX_DISCOVERED; ++d) {
                if (s_discovered[d].used &&
                    master_ble_address_equal(&s_discovered[d].address, &source->address)) {
                    advertised = &s_discovered[d];
                    break;
                }
            }
            *view = (master_slave_view_t) {
                .connected = source->connected,
                .running = source->running,
                .awake = source->awake,
                .bonded = source->bonded,
                .config_pending = config_pending,
                .configuration_confirmed = s_configuration_confirmed[i],
                .advertised_provisioned = advertised && advertised->provisioned_flag,
                .id = source->id,
                .advertised_id = advertised ? advertised->advertised_id : UINT8_MAX,
                .sleep_ms = source->sleep_ms,
                .awake_timeout_ms = source->awake_timeout_ms,
                .rssi = source->rssi,
                .address = source->address,
                .ac_state = source->ac_state,
                .pending_count = source->queue_count,
                .last_event = source->last_event,
                .last_error = source->last_error,
                .uptime_ms = source->uptime_ms,
                .battery_mv = source->battery_mv,
                .last_seen_ms = advertised ? advertised->last_seen_ms : 0,
            };
            strlcpy(view->firmware_version, source->firmware_version,
                    sizeof(view->firmware_version));
        }
    }
    xSemaphoreGive(s_lock);
    return count;
}

static esp_err_t enqueue_locked(master_slave_t *slave, uint8_t command,
                                const uint8_t *payload, uint16_t length)
{
    if (!slave || length > SMART_IR_MAX_PAYLOAD) return ESP_ERR_INVALID_ARG;
    for (uint8_t i = 0; i < slave->queue_count; ++i) {
        uint8_t existing = slave->queue[i].frame.command;
        bool replace = existing == command ||
            ((command == SMART_IR_CMD_START || command == SMART_IR_CMD_STOP) &&
             (existing == SMART_IR_CMD_START || existing == SMART_IR_CMD_STOP));
        if (replace) {
            slave->queue[i].frame.command = command;
            slave->queue[i].frame.payload_length = length;
            memcpy(slave->queue[i].frame.payload, payload, length);
            slave->queue[i].queued_at_ms = now_ms();
            return ESP_OK;
        }
    }
    if (slave->queue_count >= MASTER_QUEUE_DEPTH) return ESP_ERR_NO_MEM;
    master_pending_command_t *pending = &slave->queue[slave->queue_count++];
    memset(pending, 0, sizeof(*pending));
    pending->used = true;
    pending->frame.sequence = ++slave->last_sequence;
    pending->frame.slave_id = slave->id;
    pending->frame.command = command;
    pending->frame.payload_length = length;
    memcpy(pending->frame.payload, payload, length);
    pending->queued_at_ms = now_ms();
    return ESP_OK;
}

esp_err_t master_service_provision(const master_ble_address_t *address, uint8_t id,
                                   uint16_t sleep_ms, uint32_t awake_timeout_ms)
{
    if (!address || sleep_ms < 1 || sleep_ms > 1999 || awake_timeout_ms < 2000 ||
        awake_timeout_ms > 30000) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (find_by_id(id) || id_reserved_by_other(id, NULL) || find_by_address(address)) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    master_slave_t *slot = NULL;
    for (size_t i = 0; i < MASTER_MAX_SLAVES; ++i) {
        if (!s_config.slaves[i].used) { slot = &s_config.slaves[i]; break; }
    }
    if (!slot) { xSemaphoreGive(s_lock); return ESP_ERR_NO_MEM; }
    memset(slot, 0, sizeof(*slot));
    slot->used = true;
    slot->id = id;
    slot->sleep_ms = sleep_ms;
    slot->awake_timeout_ms = awake_timeout_ms;
    slot->address = *address;
    slot->running = true;
    slot->ac_state = (smart_ir_ac_state_t){ .power = false, .mode = SMART_IR_AC_MODE_AUTO,
        .temperature_c = 24, .fan = 0, .swing = SMART_IR_SWING_OFF };
    uint8_t payload[7] = { id, (uint8_t)sleep_ms, (uint8_t)(sleep_ms >> 8),
        (uint8_t)awake_timeout_ms, (uint8_t)(awake_timeout_ms >> 8),
        (uint8_t)(awake_timeout_ms >> 16), (uint8_t)(awake_timeout_ms >> 24) };
    esp_err_t err = enqueue_locked(slot, SMART_IR_CMD_SET_CONFIG, payload, sizeof(payload));
    char mac[MASTER_MAC_STRING_SIZE];
    master_ble_address_format(address, mac);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Provision queued: MAC=%s target_id=%u sleep=%u ms awake=%lu ms",
                 mac, id, sleep_ms, (unsigned long)awake_timeout_ms);
    }
    save_config_locked();
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t master_service_forget(uint8_t id)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    master_slave_t *slave = find_by_id(id);
    if (!slave) { xSemaphoreGive(s_lock); return ESP_ERR_NOT_FOUND; }
    memset(slave, 0, sizeof(*slave));
    save_config_locked();
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

bool master_service_is_registered(const master_ble_address_t *address)
{
    if (!s_lock || !address) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool found = find_by_address(address) != NULL;
    xSemaphoreGive(s_lock);
    return found;
}

bool master_service_find_address_by_id(uint8_t id, master_ble_address_t *address)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    master_slave_t *slave = find_by_id(id);
    if (slave && address) *address = slave->address;
    xSemaphoreGive(s_lock);
    return slave != NULL;
}

esp_err_t master_service_set_config(uint8_t current_id, uint8_t new_id,
                                    uint16_t sleep_ms, uint32_t awake_timeout_ms)
{
    if (sleep_ms < 1 || sleep_ms > 1999 || awake_timeout_ms < 2000 || awake_timeout_ms > 30000)
        return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    master_slave_t *slave = find_by_id(current_id);
    if (!slave || id_reserved_by_other(new_id, slave)) {
        xSemaphoreGive(s_lock);
        return slave ? ESP_ERR_INVALID_STATE : ESP_ERR_NOT_FOUND;
    }
    uint8_t payload[7] = { new_id, (uint8_t)sleep_ms, (uint8_t)(sleep_ms >> 8),
        (uint8_t)awake_timeout_ms, (uint8_t)(awake_timeout_ms >> 8),
        (uint8_t)(awake_timeout_ms >> 16), (uint8_t)(awake_timeout_ms >> 24) };
    esp_err_t err = enqueue_locked(slave, SMART_IR_CMD_SET_CONFIG, payload, sizeof(payload));
    if (err == ESP_OK) {
        /* Commit the requested values only after the slave acknowledges them. */
        save_config_locked();
    }
    xSemaphoreGive(s_lock);
    return err;
}

static esp_err_t enqueue_simple(uint8_t id, uint8_t command)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    master_slave_t *slave = find_by_id(id);
    esp_err_t err = slave ? enqueue_locked(slave, command, NULL, 0) : ESP_ERR_NOT_FOUND;
    if (err == ESP_OK) save_config_locked();
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t master_service_start(uint8_t id) { return enqueue_simple(id, SMART_IR_CMD_START); }
esp_err_t master_service_stop(uint8_t id) { return enqueue_simple(id, SMART_IR_CMD_STOP); }
esp_err_t master_service_request_status(uint8_t id) { return enqueue_simple(id, SMART_IR_CMD_GET_STATUS); }

esp_err_t master_service_set_ac(uint8_t id, const smart_ir_ac_state_t *state)
{
    if (!smart_ir_ac_state_valid(state)) return ESP_ERR_INVALID_ARG;
    uint8_t payload[5] = { state->power, state->mode, state->temperature_c,
                           state->fan, state->swing };
    xSemaphoreTake(s_lock, portMAX_DELAY);
    master_slave_t *slave = find_by_id(id);
    esp_err_t err = slave ? enqueue_locked(slave, SMART_IR_CMD_SET_AC_STATE,
                                            payload, sizeof(payload)) : ESP_ERR_NOT_FOUND;
    if (err == ESP_OK) {
        slave->ac_state = *state;
        save_config_locked();
    }
    xSemaphoreGive(s_lock);
    return err;
}

bool master_service_peek_command(const master_ble_address_t *address, smart_ir_frame_t *frame)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    master_slave_t *slave = find_by_address(address);
    bool found = slave && slave->queue_count > 0;
    if (found && frame) *frame = slave->queue[0].frame;
    xSemaphoreGive(s_lock);
    return found;
}

bool master_service_has_pending_commands(void)
{
    if (!s_lock) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool found = false;
    for (size_t i = 0; i < MASTER_MAX_SLAVES; ++i) {
        if (s_config.slaves[i].used && s_config.slaves[i].queue_count > 0) {
            found = true;
            break;
        }
    }
    xSemaphoreGive(s_lock);
    return found;
}

bool master_service_address_has_pending(const master_ble_address_t *address)
{
    if (!s_lock || !address) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    master_slave_t *slave = find_by_address(address);
    bool found = slave && slave->queue_count > 0;
    xSemaphoreGive(s_lock);
    return found;
}

void master_service_handle_frame(const master_ble_address_t *address,
                                 const smart_ir_frame_t *frame)
{
    if (!address || !frame) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    master_slave_t *slave = find_by_address(address);
    if (!slave) { xSemaphoreGive(s_lock); return; }
    slave->last_event = frame->command;
    if (frame->command == SMART_IR_EVT_WAKE_STATUS) {
        slave->awake = true;
        if (frame->payload_length >= 8) {
            slave->running = frame->payload[0] != 0;
            slave->uptime_ms = (uint32_t)frame->payload[2] |
                ((uint32_t)frame->payload[3] << 8) | ((uint32_t)frame->payload[4] << 16) |
                ((uint32_t)frame->payload[5] << 24);
            slave->battery_mv = (uint16_t)frame->payload[6] | ((uint16_t)frame->payload[7] << 8);
        }
    } else if (frame->command == SMART_IR_EVT_PRE_SLEEP_STATUS) {
        slave->awake = false;
    } else if (frame->command == SMART_IR_EVT_NACK) {
        slave->last_error = frame->payload_length ? frame->payload[0] : SMART_IR_NACK_INVALID_LENGTH;
        char mac[MASTER_MAC_STRING_SIZE];
        master_ble_address_format(address, mac);
        ESP_LOGW(TAG, "Slave NACK: MAC=%s id=%u seq=%u error=%u", mac, slave->id,
                 frame->sequence, slave->last_error);
    } else if ((frame->command == SMART_IR_EVT_ACK || frame->command == SMART_IR_EVT_AC_RESULT) &&
               slave->queue_count && slave->queue[0].frame.sequence == frame->sequence) {
        const smart_ir_frame_t completed_frame = slave->queue[0].frame;
        uint8_t completed = completed_frame.command;
        if (completed == SMART_IR_CMD_START) slave->running = true;
        if (completed == SMART_IR_CMD_STOP) slave->running = false;
        if (completed == SMART_IR_CMD_GET_STATUS && frame->payload_length >= 8) {
            slave->running = frame->payload[0] != 0;
            slave->uptime_ms = (uint32_t)frame->payload[2] |
                ((uint32_t)frame->payload[3] << 8) |
                ((uint32_t)frame->payload[4] << 16) |
                ((uint32_t)frame->payload[5] << 24);
            slave->battery_mv = (uint16_t)frame->payload[6] |
                                ((uint16_t)frame->payload[7] << 8);
        }
        memmove(&slave->queue[0], &slave->queue[1],
                sizeof(slave->queue[0]) * (MASTER_QUEUE_DEPTH - 1));
        memset(&slave->queue[MASTER_QUEUE_DEPTH - 1], 0, sizeof(slave->queue[0]));
        --slave->queue_count;
        if (completed == SMART_IR_CMD_SET_CONFIG && completed_frame.payload_length == 7) {
            const uint8_t previous_id = slave->id;
            slave->id = completed_frame.payload[0];
            slave->sleep_ms = (uint16_t)completed_frame.payload[1] |
                              ((uint16_t)completed_frame.payload[2] << 8);
            slave->awake_timeout_ms = (uint32_t)completed_frame.payload[3] |
                ((uint32_t)completed_frame.payload[4] << 8) |
                ((uint32_t)completed_frame.payload[5] << 16) |
                ((uint32_t)completed_frame.payload[6] << 24);
            /* Commands queued behind SET_CONFIG must use the newly ACKed ID. */
            for (uint8_t q = 0; q < slave->queue_count; ++q) {
                slave->queue[q].frame.slave_id = slave->id;
            }
            s_configuration_confirmed[slave - s_config.slaves] = true;
            char mac[MASTER_MAC_STRING_SIZE];
            master_ble_address_format(address, mac);
            ESP_LOGI(TAG, "Configuration ACK: MAC=%s ID %u -> %u, sleep=%u ms, awake=%lu ms",
                     mac, previous_id, slave->id, slave->sleep_ms,
                     (unsigned long)slave->awake_timeout_ms);
        }
        save_config_locked();
    }
    xSemaphoreGive(s_lock);
}

void master_service_connection_changed(const master_ble_address_t *address,
                                       bool connected, bool bonded)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    master_slave_t *slave = find_by_address(address);
    if (slave) {
        slave->connected = connected;
        slave->bonded = slave->bonded || bonded;
        if (!connected) slave->awake = false;
    }
    xSemaphoreGive(s_lock);
}

esp_err_t master_service_get_wifi_credentials(master_wifi_credentials_t *credentials)
{
    if (!credentials) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    credentials->configured = s_config.wifi_configured;
    strlcpy(credentials->ssid, s_config.wifi_ssid, sizeof(credentials->ssid));
    strlcpy(credentials->password, s_config.wifi_password, sizeof(credentials->password));
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t master_service_update_wifi(const char *ssid, const char *password)
{
    if (!ssid || !password || strlen(ssid) > 32 || strlen(password) > 64) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    char previous_ssid[sizeof(s_config.wifi_ssid)];
    char previous_password[sizeof(s_config.wifi_password)];
    bool previous_configured = s_config.wifi_configured;
    strlcpy(previous_ssid, s_config.wifi_ssid, sizeof(previous_ssid));
    strlcpy(previous_password, s_config.wifi_password, sizeof(previous_password));
    bool ssid_changed = strcmp(previous_ssid, ssid) != 0;
    bool password_changed = strcmp(previous_password, password) != 0;
    strlcpy(s_config.wifi_ssid, ssid, sizeof(s_config.wifi_ssid));
    strlcpy(s_config.wifi_password, password, sizeof(s_config.wifi_password));
    s_config.wifi_configured = s_config.wifi_ssid[0] != '\0';
    esp_err_t error = save_config_locked();
    if (error == ESP_OK) {
        /* This structure contains all slave command queues and is too large
         * for the NimBLE host task stack. Use heap memory for NVS read-back. */
        master_persistent_config_t *persisted = calloc(1, sizeof(*persisted));
        if (!persisted) {
            error = ESP_ERR_NO_MEM;
        } else {
            error = config_store_load(persisted);
            if (error == ESP_OK &&
                (persisted->wifi_configured != s_config.wifi_configured ||
                 strcmp(persisted->wifi_ssid, s_config.wifi_ssid) != 0 ||
                 strcmp(persisted->wifi_password, s_config.wifi_password) != 0)) {
                error = ESP_ERR_INVALID_STATE;
                ESP_LOGE(TAG, "Wi-Fi credential read-back verification failed");
            }
            free(persisted);
        }
    }
    if (error != ESP_OK) {
        strlcpy(s_config.wifi_ssid, previous_ssid, sizeof(s_config.wifi_ssid));
        strlcpy(s_config.wifi_password, previous_password, sizeof(s_config.wifi_password));
        s_config.wifi_configured = previous_configured;
        (void)save_config_locked();
    } else {
        ESP_LOGI(TAG,
                 "Wi-Fi credentials persisted: SSID=\"%s\", ssid_changed=%s, "
                 "password_changed=%s, password=%s, password_length=%u",
                 s_config.wifi_ssid, ssid_changed ? "yes" : "no",
                 password_changed ? "yes" : "no",
                 s_config.wifi_password[0] ? "********" : "empty",
                 (unsigned)strlen(s_config.wifi_password));
    }
    xSemaphoreGive(s_lock);
    return error;
}
