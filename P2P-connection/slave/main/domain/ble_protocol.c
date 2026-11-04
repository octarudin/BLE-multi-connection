#include "domain/ble_protocol.h"
#include <string.h>

static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static void write_le16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

uint16_t smart_ir_crc16_ccitt_false(const uint8_t *data, size_t length)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < length; ++i) {
        crc ^= (uint16_t)data[i] << 8;
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000U) ? (uint16_t)((crc << 1) ^ 0x1021U)
                                  : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

esp_err_t smart_ir_frame_encode(const smart_ir_frame_t *frame, uint8_t *output,
                                size_t capacity, size_t *written)
{
    if (!frame || !output || !written || frame->payload_length > SMART_IR_MAX_PAYLOAD) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t total = SMART_IR_FRAME_OVERHEAD + frame->payload_length;
    if (capacity < total) return ESP_ERR_INVALID_SIZE;
    output[0] = SMART_IR_SOF;
    output[1] = SMART_IR_PROTOCOL_VERSION;
    output[2] = frame->sequence;
    output[3] = frame->slave_id;
    output[4] = frame->command;
    write_le16(&output[5], frame->payload_length);
    memcpy(&output[7], frame->payload, frame->payload_length);
    write_le16(&output[7 + frame->payload_length],
               smart_ir_crc16_ccitt_false(&output[1], 6 + frame->payload_length));
    *written = total;
    return ESP_OK;
}

esp_err_t smart_ir_frame_decode(const uint8_t *data, size_t length,
                                smart_ir_frame_t *frame)
{
    if (!data || !frame || length < SMART_IR_FRAME_OVERHEAD) return ESP_ERR_INVALID_ARG;
    if (data[0] != SMART_IR_SOF || data[1] != SMART_IR_PROTOCOL_VERSION) {
        return ESP_ERR_INVALID_VERSION;
    }
    const uint16_t payload_length = read_le16(&data[5]);
    if (payload_length > SMART_IR_MAX_PAYLOAD ||
        length != SMART_IR_FRAME_OVERHEAD + payload_length) return ESP_ERR_INVALID_SIZE;
    const uint16_t expected = read_le16(&data[7 + payload_length]);
    const uint16_t actual = smart_ir_crc16_ccitt_false(&data[1], 6 + payload_length);
    if (expected != actual) return ESP_ERR_INVALID_CRC;
    frame->sequence = data[2];
    frame->slave_id = data[3];
    frame->command = data[4];
    frame->payload_length = payload_length;
    memcpy(frame->payload, &data[7], payload_length);
    return ESP_OK;
}

bool smart_ir_ac_state_valid(const smart_ir_ac_state_t *state)
{
    return state && state->mode <= SMART_IR_AC_MODE_HEAT &&
           state->temperature_c >= 16 && state->temperature_c <= 30 &&
           state->fan <= 5 && state->swing <= SMART_IR_SWING_BOTH;
}
