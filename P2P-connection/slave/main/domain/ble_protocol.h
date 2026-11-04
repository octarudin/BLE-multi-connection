#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define SMART_IR_SOF                  0xA5
#define SMART_IR_PROTOCOL_VERSION     0x01
#define SMART_IR_MAX_PAYLOAD          128
#define SMART_IR_FRAME_OVERHEAD       9
#define SMART_IR_MAX_FRAME_SIZE       (SMART_IR_MAX_PAYLOAD + SMART_IR_FRAME_OVERHEAD)

#define SMART_IR_COMPANY_ID           0xFFFF
#define SMART_IR_ADV_MAGIC_0          0x53
#define SMART_IR_ADV_MAGIC_1          0x49
#define SMART_IR_ADV_FLAG_PROVISIONED (1U << 0)
#define SMART_IR_ADV_FLAG_RUNNING     (1U << 1)
#define SMART_IR_ADV_FLAG_STATUS      (1U << 2)

typedef enum {
    SMART_IR_CMD_SET_CONFIG       = 0x10,
    SMART_IR_CMD_START            = 0x20,
    SMART_IR_CMD_STOP             = 0x21,
    SMART_IR_CMD_SET_AC_STATE     = 0x30,
    SMART_IR_CMD_GET_STATUS       = 0x40,
    SMART_IR_EVT_ACK              = 0x80,
    SMART_IR_EVT_NACK             = 0x81,
    SMART_IR_EVT_WAKE_STATUS      = 0x82,
    SMART_IR_EVT_PRE_SLEEP_STATUS = 0x83,
    SMART_IR_EVT_AC_RESULT        = 0x84,
} smart_ir_command_t;

typedef enum {
    SMART_IR_NACK_INVALID_COMMAND = 0x01,
    SMART_IR_NACK_INVALID_LENGTH  = 0x02,
    SMART_IR_NACK_INVALID_ID      = 0x03,
    SMART_IR_NACK_INVALID_VALUE   = 0x04,
    SMART_IR_NACK_PROGRAM_STOPPED = 0x05,
    SMART_IR_NACK_IR_FAILURE      = 0x06,
    SMART_IR_NACK_UNSUPPORTED     = 0x07,
    SMART_IR_NACK_BUSY            = 0x08,
} smart_ir_nack_t;

typedef enum {
    SMART_IR_AC_MODE_AUTO = 0,
    SMART_IR_AC_MODE_COOL = 1,
    SMART_IR_AC_MODE_DRY  = 2,
    SMART_IR_AC_MODE_FAN  = 3,
    SMART_IR_AC_MODE_HEAT = 4,
} smart_ir_ac_mode_t;

typedef enum {
    SMART_IR_SWING_OFF        = 0,
    SMART_IR_SWING_VERTICAL   = 1,
    SMART_IR_SWING_HORIZONTAL = 2,
    SMART_IR_SWING_BOTH       = 3,
} smart_ir_swing_t;

typedef struct {
    bool power;
    uint8_t mode;
    uint8_t temperature_c;
    uint8_t fan;
    uint8_t swing;
} smart_ir_ac_state_t;

typedef struct {
    uint8_t sequence;
    uint8_t slave_id;
    uint8_t command;
    uint16_t payload_length;
    uint8_t payload[SMART_IR_MAX_PAYLOAD];
} smart_ir_frame_t;

uint16_t smart_ir_crc16_ccitt_false(const uint8_t *data, size_t length);
esp_err_t smart_ir_frame_encode(const smart_ir_frame_t *frame, uint8_t *output,
                                size_t capacity, size_t *written);
esp_err_t smart_ir_frame_decode(const uint8_t *data, size_t length,
                                smart_ir_frame_t *frame);
bool smart_ir_ac_state_valid(const smart_ir_ac_state_t *state);
