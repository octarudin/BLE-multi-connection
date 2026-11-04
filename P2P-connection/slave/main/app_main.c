#include "application/slave_service.h"
#include "esp_check.h"
#include "esp_log.h"
#include "infrastructure/ble_peripheral.h"
#include "infrastructure/config_store.h"
#include "infrastructure/ir_transmitter.h"

static const char *TAG = "app_main";

void app_main(void)
{
    ESP_LOGI(TAG, "Smart IR Slave 0.1.1-dev, protocol v1, environment dev");
    ESP_ERROR_CHECK(config_store_init());
    ESP_ERROR_CHECK(ir_transmitter_init());

    const slave_service_ports_t ports = {
        .send_frame = ble_peripheral_send_frame,
        .refresh_advertising = ble_peripheral_refresh_advertising,
        .enter_deep_sleep = ble_peripheral_enter_deep_sleep,
    };
    ESP_ERROR_CHECK(slave_service_init(&ports));
    ESP_ERROR_CHECK(ble_peripheral_init());
    ESP_LOGI(TAG, "Slave initialized; app_main can return safely");
}
