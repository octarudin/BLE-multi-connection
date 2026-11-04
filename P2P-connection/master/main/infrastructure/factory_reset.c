#include "infrastructure/factory_reset.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "infrastructure/config_store.h"
#include "infrastructure/status_led.h"
#include "sdkconfig.h"

static const char *TAG = "factory_reset";

static void reset_task(void *arg)
{
    (void)arg;
    status_led_set(STATUS_LED_FACTORY_RESET);
    vTaskDelay(pdMS_TO_TICKS(750));
    ESP_LOGW(TAG, "Erasing application configuration and BLE bonds");
    ESP_ERROR_CHECK(config_store_factory_reset());
    vTaskDelay(pdMS_TO_TICKS(250));
    esp_restart();
}

void factory_reset_schedule(void)
{
    xTaskCreate(reset_task, "factory_reset", 4096, NULL, 10, NULL);
}

static void button_task(void *arg)
{
    (void)arg;
    TickType_t pressed_at = 0;
    bool announced = false;
    while (true) {
        if (gpio_get_level(CONFIG_SMART_IR_FACTORY_RESET_GPIO) == 0) {
            if (!pressed_at) pressed_at = xTaskGetTickCount();
            TickType_t elapsed = xTaskGetTickCount() - pressed_at;
            if (elapsed >= pdMS_TO_TICKS(4000) && !announced) {
                status_led_set(STATUS_LED_FACTORY_RESET);
                announced = true;
            }
            if (elapsed >= pdMS_TO_TICKS(5000)) factory_reset_schedule();
        } else {
            pressed_at = 0;
            announced = false;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

esp_err_t factory_reset_button_init(void)
{
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << CONFIG_SMART_IR_FACTORY_RESET_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&config), TAG, "configure button");
    return xTaskCreate(button_task, "reset_button", 2048, NULL, 5, NULL) == pdPASS
        ? ESP_OK : ESP_ERR_NO_MEM;
}

