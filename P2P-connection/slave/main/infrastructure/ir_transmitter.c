#include "infrastructure/ir_transmitter.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "ir_transmitter";

esp_err_t ir_transmitter_init(void)
{
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << CONFIG_SMART_IR_IR_TX_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&config);
    if (err == ESP_OK) gpio_set_level(CONFIG_SMART_IR_IR_TX_GPIO, 0);
    ESP_LOGI(TAG, "IR output GPIO=%d; Daikin encoder not selected",
             CONFIG_SMART_IR_IR_TX_GPIO);
    return err;
}

esp_err_t ir_transmitter_send_daikin(const smart_ir_ac_state_t *state)
{
    if (!smart_ir_ac_state_valid(state)) return ESP_ERR_INVALID_ARG;
    /* Daikin has several incompatible frame families (for example 152, 176,
     * 216 and 280 bit). Emitting an assumed family can operate the AC with a
     * wrong state, so this port deliberately reports unsupported until the
     * exact indoor-unit protocol is configured. */
    ESP_LOGW(TAG, "AC command rejected: Daikin protocol variant is not configured");
    return ESP_ERR_NOT_SUPPORTED;
}
