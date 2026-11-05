idf_component_register(SRCS "slave_main.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_app_format esp_driver_gpio nvs_flash)
