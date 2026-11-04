#include "infrastructure/config_store.h"

#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#define CONFIG_NAMESPACE "slave_cfg"
#define CONFIG_KEY       "settings"

static const char *TAG = "config_store";

esp_err_t config_store_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

esp_err_t config_store_load(slave_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CONFIG_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;
    size_t size = sizeof(*config);
    err = nvs_get_blob(handle, CONFIG_KEY, config, &size);
    nvs_close(handle);
    if (err == ESP_OK &&
        (size != sizeof(*config) || config->schema_version != SLAVE_CONFIG_SCHEMA_VERSION)) {
        ESP_LOGW(TAG, "Stored configuration schema is incompatible");
        return ESP_ERR_INVALID_VERSION;
    }
    return err;
}

esp_err_t config_store_save(const slave_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(handle, CONFIG_KEY, config, sizeof(*config));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t config_store_erase(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_erase_all(handle);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}
