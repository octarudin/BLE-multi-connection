#include "infrastructure/config_store.h"
#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "config_store";
static const char *NAMESPACE = "smart_ir";
static const char *CONFIG_KEY = "config";

esp_err_t config_store_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "erase incompatible NVS");
        err = nvs_flash_init();
    }
    return err;
}

esp_err_t config_store_load(master_persistent_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(config, 0, sizeof(*config));
    config->schema_version = MASTER_CONFIG_SCHEMA_VERSION;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "open NVS");

    size_t size = sizeof(*config);
    err = nvs_get_blob(handle, CONFIG_KEY, config, &size);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        memset(config, 0, sizeof(*config));
        config->schema_version = MASTER_CONFIG_SCHEMA_VERSION;
        return ESP_OK;
    }
    if (err != ESP_OK || size != sizeof(*config) ||
        config->schema_version != MASTER_CONFIG_SCHEMA_VERSION) {
        ESP_LOGW(TAG, "Invalid configuration; factory defaults loaded");
        memset(config, 0, sizeof(*config));
        config->schema_version = MASTER_CONFIG_SCHEMA_VERSION;
        return ESP_OK;
    }
    return ESP_OK;
}

esp_err_t config_store_save(const master_persistent_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open(NAMESPACE, NVS_READWRITE, &handle), TAG, "open NVS");
    esp_err_t err = nvs_set_blob(handle, CONFIG_KEY, config, sizeof(*config));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t config_store_factory_reset(void)
{
    /* This erases application config, Wi-Fi driver state, NimBLE bonds and
       application-managed certificates. It never touches eFuse keys. */
    ESP_RETURN_ON_ERROR(nvs_flash_deinit(), TAG, "deinit NVS");
    return nvs_flash_erase();
}

