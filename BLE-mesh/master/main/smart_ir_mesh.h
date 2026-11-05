#pragma once

#include <stdint.h>
#include "esp_ble_mesh_defs.h"
#include "sdkconfig.h"

#define SMART_IR_CID                 0x02E5
#define SMART_IR_CLIENT_MODEL_ID     0x1000
#define SMART_IR_SERVER_MODEL_ID     0x1001
#define SMART_IR_OP_COMMAND          ESP_BLE_MESH_MODEL_OP_3(0x10, SMART_IR_CID)
#define SMART_IR_OP_STATUS           ESP_BLE_MESH_MODEL_OP_3(0x11, SMART_IR_CID)
#define SMART_IR_OP_LIFECYCLE        ESP_BLE_MESH_MODEL_OP_3(0x12, SMART_IR_CID)

#define SMART_IR_UUID_0              0x53
#define SMART_IR_UUID_1              0x49
#define SMART_IR_PROTOCOL_VERSION    3
#define SMART_IR_ENVIRONMENT         "dev"
/* Follow the provisioner capacity selected in ESP-IDF menuconfig. This is not
 * tied to how many physical boards are currently available. */
#define SMART_IR_MAX_SLAVES          CONFIG_BLE_MESH_MAX_PROV_NODES

typedef enum {
    SMART_IR_CMD_SET_CONFIG = 0x10,
    SMART_IR_CMD_SET_TIMING = 0x11,
    SMART_IR_CMD_WAKE_SYNC = 0x12,
    SMART_IR_CMD_START = 0x20,
    SMART_IR_CMD_STOP = 0x21,
    SMART_IR_CMD_SET_AC_STATE = 0x30,
    SMART_IR_CMD_GET_STATUS = 0x40,
} smart_ir_command_t;

typedef enum {
    SMART_IR_EVT_ACK = 0x80,
    SMART_IR_EVT_NACK = 0x81,
    SMART_IR_EVT_STATUS = 0x82,
    SMART_IR_EVT_WAKE = 0x83,
    SMART_IR_EVT_AC_RESULT = 0x84,
    SMART_IR_EVT_SLEEP = 0x85,
} smart_ir_event_t;

#define SMART_IR_WAKE_FLAG_STOP      (1U << 0)
#define SMART_IR_WAKE_FLAG_AC        (1U << 1)

typedef enum {
    SMART_IR_NACK_INVALID_COMMAND = 0x01,
    SMART_IR_NACK_INVALID_LENGTH = 0x02,
    SMART_IR_NACK_INVALID_ID = 0x03,
    SMART_IR_NACK_INVALID_VALUE = 0x04,
    SMART_IR_NACK_PROGRAM_STOPPED = 0x05,
    SMART_IR_NACK_IR_FAILURE = 0x06,
    SMART_IR_NACK_UNSUPPORTED = 0x07,
    SMART_IR_NACK_BUSY = 0x08,
} smart_ir_nack_t;
