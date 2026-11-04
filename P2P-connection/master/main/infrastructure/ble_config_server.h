#pragma once

#include <stdbool.h>
#include "esp_err.h"

#define SMART_IR_CONFIG_SERVICE_UUID_TEXT "7f520001-6c6f-4b8d-9a65-534d41525449"
#define SMART_IR_CONFIG_RX_UUID_TEXT      "7f520002-6c6f-4b8d-9a65-534d41525449"
#define SMART_IR_CONFIG_TX_UUID_TEXT      "7f520003-6c6f-4b8d-9a65-534d41525449"

/* Called after nimble_port_init(), before the NimBLE host task starts. */
esp_err_t ble_config_server_init(void);

/* Called by the shared NimBLE sync callback. */
void ble_config_server_on_sync(void);
void ble_config_server_start_window(void);

bool ble_config_server_connected(void);
