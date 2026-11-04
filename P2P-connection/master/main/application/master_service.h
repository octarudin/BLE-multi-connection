#pragma once

#include <stddef.h>
#include "domain/master_types.h"
#include "esp_err.h"

esp_err_t master_service_init(void);

void master_service_discovered(const master_ble_address_t *address, int8_t rssi,
                               const smart_ir_advertisement_t *advertisement);
size_t master_service_discovered_snapshot(master_discovered_t *output, size_t capacity);
void master_service_clear_discovered(void);
size_t master_service_slaves_snapshot(master_slave_view_t *output, size_t capacity);

esp_err_t master_service_provision(const master_ble_address_t *address, uint8_t id,
                                   uint16_t sleep_ms, uint32_t awake_timeout_ms);
esp_err_t master_service_forget(uint8_t id);
bool master_service_is_registered(const master_ble_address_t *address);
bool master_service_find_address_by_id(uint8_t id, master_ble_address_t *address);

esp_err_t master_service_set_config(uint8_t current_id, uint8_t new_id,
                                    uint16_t sleep_ms, uint32_t awake_timeout_ms);
esp_err_t master_service_start(uint8_t id);
esp_err_t master_service_stop(uint8_t id);
esp_err_t master_service_set_ac(uint8_t id, const smart_ir_ac_state_t *state);
esp_err_t master_service_request_status(uint8_t id);

bool master_service_peek_command(const master_ble_address_t *address,
                                 smart_ir_frame_t *frame);
bool master_service_has_pending_commands(void);
bool master_service_address_has_pending(const master_ble_address_t *address);
void master_service_handle_frame(const master_ble_address_t *address,
                                 const smart_ir_frame_t *frame);
void master_service_connection_changed(const master_ble_address_t *address,
                                       bool connected, bool bonded);

esp_err_t master_service_get_wifi_credentials(master_wifi_credentials_t *credentials);
esp_err_t master_service_update_wifi(const char *ssid, const char *password);

