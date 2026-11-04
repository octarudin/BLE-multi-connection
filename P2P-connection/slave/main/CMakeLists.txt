idf_component_register(
    SRCS
        "app_main.c"
        "domain/ble_protocol.c"
        "application/slave_service.c"
        "infrastructure/config_store.c"
        "infrastructure/ble_peripheral.c"
        "infrastructure/ir_transmitter.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES bt nvs_flash esp_timer esp_driver_gpio esp_system
)
