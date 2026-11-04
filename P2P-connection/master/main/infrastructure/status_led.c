#include "infrastructure/status_led.h"
#include "led_strip.h"
#include "sdkconfig.h"

static led_strip_handle_t s_strip;

esp_err_t status_led_init(void)
{
    const led_strip_config_t strip = {
        .strip_gpio_num = CONFIG_SMART_IR_RGB_LED_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags.invert_out = false,
    };
    const led_strip_rmt_config_t rmt = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 0,
        .flags.with_dma = false,
    };
    esp_err_t err = led_strip_new_rmt_device(&strip, &rmt, &s_strip);
    if (err == ESP_OK) status_led_set(STATUS_LED_BOOTING);
    return err;
}

void status_led_set(status_led_state_t state)
{
    if (!s_strip) return;
    uint8_t r = 0, g = 0, b = 0;
    switch (state) {
        case STATUS_LED_BOOTING:       b = 16; break;
        case STATUS_LED_BLE_READY:     b = 12; break;
        case STATUS_LED_CONFIG_CONNECTED: r = 10; g = 8; b = 16; break;
        case STATUS_LED_BLE_CONNECTED: g = 12; b = 12; break;
        case STATUS_LED_ERROR:         r = 20; break;
        case STATUS_LED_FACTORY_RESET: r = 20; g = 8; break;
    }
    led_strip_set_pixel(s_strip, 0, r, g, b);
    led_strip_refresh(s_strip);
}

