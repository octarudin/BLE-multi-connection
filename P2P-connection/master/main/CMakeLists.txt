idf_component_register(
    SRCS
        "app_main.c"
        "domain/ble_protocol.c"
        "domain/master_types.c"
        "application/master_service.c"
        "infrastructure/config_store.c"
        "infrastructure/ble_central.c"
        "infrastructure/ble_config_server.c"
        "infrastructure/status_led.c"
        "infrastructure/factory_reset.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        bt nvs_flash json
        esp_timer driver esp_app_format espcoredump
)
