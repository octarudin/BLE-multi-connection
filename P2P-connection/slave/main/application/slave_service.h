#pragma once

#include <stdbool.h>
#include "domain/ble_protocol.h"
#include "domain/slave_types.h"
#include "esp_err.h"

typedef struct {
    esp_err_t (*send_frame)(const smart_ir_frame_t *frame);
    void (*refresh_advertising)(void);
    void (*enter_deep_sleep)(uint16_t sleep_ms);
} slave_service_ports_t;

esp_err_t slave_service_init(const slave_service_ports_t *ports);
void slave_service_on_subscribed(void);
void slave_service_on_disconnected(void);
void slave_service_handle_frame(const smart_ir_frame_t *frame);
void slave_service_snapshot(slave_config_t *config);
uint8_t slave_service_advertising_flags(void);
