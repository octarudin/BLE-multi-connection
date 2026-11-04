#pragma once

#include <stdbool.h>
#include "esp_err.h"

/* GATT UUIDs are part of protocol version 1 and must be shared by slave FW. */
#define SMART_IR_SERVICE_UUID_TEXT "7f510001-6c6f-4b8d-9a65-534d41525449"
#define SMART_IR_COMMAND_UUID_TEXT "7f510002-6c6f-4b8d-9a65-534d41525449"
#define SMART_IR_STATUS_UUID_TEXT  "7f510003-6c6f-4b8d-9a65-534d41525449"

esp_err_t ble_central_init(void);
void ble_central_set_scan_enabled(bool enabled);
void ble_central_set_configuration_active(bool active);
esp_err_t ble_central_start_configuration_scan(void);
bool ble_central_scan_active(void);
void ble_central_kick(void);

