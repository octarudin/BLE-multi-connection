#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "domain/ble_protocol.h"

#define SLAVE_CONFIG_SCHEMA_VERSION 1U
#define SLAVE_UNPROVISIONED_ID      0xFFU

typedef struct {
    uint32_t schema_version;
    bool provisioned;
    bool running;
    uint8_t id;
    uint16_t sleep_ms;
    uint32_t awake_timeout_ms;
    smart_ir_ac_state_t last_ac_state;
} slave_config_t;
