#pragma once

#include "domain/master_types.h"
#include "esp_err.h"

#define MASTER_CONFIG_SCHEMA_VERSION 1

esp_err_t config_store_init(void);
esp_err_t config_store_load(master_persistent_config_t *config);
esp_err_t config_store_save(const master_persistent_config_t *config);
esp_err_t config_store_factory_reset(void);

