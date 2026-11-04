#pragma once

#include "domain/slave_types.h"
#include "esp_err.h"

esp_err_t config_store_init(void);
esp_err_t config_store_load(slave_config_t *config);
esp_err_t config_store_save(const slave_config_t *config);
esp_err_t config_store_erase(void);
