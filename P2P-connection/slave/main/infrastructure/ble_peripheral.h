#pragma once

#include "domain/ble_protocol.h"
#include "esp_err.h"

esp_err_t ble_peripheral_init(void);
esp_err_t ble_peripheral_send_frame(const smart_ir_frame_t *frame);
void ble_peripheral_refresh_advertising(void);
void ble_peripheral_enter_deep_sleep(uint16_t sleep_ms);
