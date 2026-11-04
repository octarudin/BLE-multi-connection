#pragma once

#include "domain/ble_protocol.h"
#include "esp_err.h"

esp_err_t ir_transmitter_init(void);
esp_err_t ir_transmitter_send_daikin(const smart_ir_ac_state_t *state);
