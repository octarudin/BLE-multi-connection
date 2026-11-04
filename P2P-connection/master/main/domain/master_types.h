#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "domain/ble_protocol.h"

#define MASTER_MAX_SLAVES       3
#define MASTER_MAX_DISCOVERED   12
#define MASTER_QUEUE_DEPTH      8
#define MASTER_MAC_STRING_SIZE  18
#define MASTER_STATUS_TEXT_SIZE 32

typedef struct {
    uint8_t type;
    uint8_t value[6];
} master_ble_address_t;

typedef struct {
    bool used;
    smart_ir_frame_t frame;
    int64_t queued_at_ms;
} master_pending_command_t;

typedef struct {
    bool used;
    bool connected;
    bool running;
    bool awake;
    bool bonded;
    uint8_t id;
    uint16_t sleep_ms;
    uint32_t awake_timeout_ms;
    int8_t rssi;
    master_ble_address_t address;
    smart_ir_ac_state_t ac_state;
    master_pending_command_t queue[MASTER_QUEUE_DEPTH];
    uint8_t queue_count;
    uint8_t last_sequence;
    uint8_t last_event;
    uint8_t last_error;
    uint32_t uptime_ms;
    uint16_t battery_mv;
    char firmware_version[16];
} master_slave_t;

/* Read-only DTO used outside the application service.  It deliberately omits
   the command queue so callers cannot copy several kilobytes onto a task
   stack. */
typedef struct {
    bool connected;
    bool running;
    bool awake;
    bool bonded;
    bool config_pending;
    bool configuration_confirmed;
    bool advertised_provisioned;
    uint8_t id;
    uint8_t advertised_id;
    uint16_t sleep_ms;
    uint32_t awake_timeout_ms;
    int8_t rssi;
    master_ble_address_t address;
    smart_ir_ac_state_t ac_state;
    uint8_t pending_count;
    uint8_t last_event;
    uint8_t last_error;
    uint32_t uptime_ms;
    uint16_t battery_mv;
    int64_t last_seen_ms;
    char firmware_version[16];
} master_slave_view_t;

typedef struct {
    bool configured;
    char ssid[33];
    char password[65];
} master_wifi_credentials_t;

typedef struct {
    bool used;
    bool provisioned_flag;
    bool running_flag;
    uint8_t advertised_id;
    int8_t rssi;
    int64_t last_seen_ms;
    master_ble_address_t address;
} master_discovered_t;

typedef struct {
    uint32_t schema_version;
    bool wifi_configured;
    char wifi_ssid[33];
    char wifi_password[65];
    master_slave_t slaves[MASTER_MAX_SLAVES];
} master_persistent_config_t;

void master_ble_address_format(const master_ble_address_t *address,
                               char output[MASTER_MAC_STRING_SIZE]);
bool master_ble_address_parse(const char *text, master_ble_address_t *address);
bool master_ble_address_equal(const master_ble_address_t *a,
                              const master_ble_address_t *b);

