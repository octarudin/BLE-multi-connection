#include "application/master_service.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "infrastructure/ble_central.h"
#include "infrastructure/ble_config_server.h"
#include "infrastructure/config_store.h"
#include "infrastructure/factory_reset.h"
#include "infrastructure/status_led.h"

static const char *TAG = "smart_ir_master";

static void status_task(void *arg)
{
    (void)arg;
    while (true) {
        master_slave_view_t slaves[MASTER_MAX_SLAVES];
        size_t count = master_service_slaves_snapshot(slaves, MASTER_MAX_SLAVES);
        bool ble_connected = false;
        for (size_t i = 0; i < count; ++i) ble_connected |= slaves[i].connected;
        status_led_set(ble_config_server_connected() ? STATUS_LED_CONFIG_CONNECTED :
                       ble_connected ? STATUS_LED_BLE_CONNECTED : STATUS_LED_BLE_READY);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Smart IR master 0.2.9-dev (environment=dev, protocol=1, transport=BLE)");
    ESP_ERROR_CHECK(config_store_init());
    ESP_ERROR_CHECK(master_service_init());
    esp_err_t led_err = status_led_init();
    if (led_err != ESP_OK) ESP_LOGW(TAG, "RGB LED unavailable: %s", esp_err_to_name(led_err));
    ESP_ERROR_CHECK(factory_reset_button_init());
    ESP_ERROR_CHECK(ble_central_init());
    ble_central_set_scan_enabled(true);
    xTaskCreate(status_task, "connectivity", 6144, NULL, 3, NULL);
    ESP_LOGI(TAG, "Startup complete; laptop configuration advertising and slave scan enabled");
}
