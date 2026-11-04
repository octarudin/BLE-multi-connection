#pragma once

#include "esp_err.h"

typedef enum {
    STATUS_LED_BOOTING,
    STATUS_LED_BLE_READY,
    STATUS_LED_CONFIG_CONNECTED,
    STATUS_LED_BLE_CONNECTED,
    STATUS_LED_ERROR,
    STATUS_LED_FACTORY_RESET,
} status_led_state_t;

esp_err_t status_led_init(void);
void status_led_set(status_led_state_t state);

