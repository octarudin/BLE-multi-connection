#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

typedef struct {
    bool sta_configured;
    bool sta_connecting;
    bool sta_connected;
    bool ap_active;
    uint8_t ap_client_count;
    char sta_ssid[33];
    char sta_ip[16];
    char ap_ssid[33];
    char ap_ip[16];
} wifi_manager_status_t;

esp_err_t wifi_manager_init(void);
esp_err_t wifi_manager_apply_credentials(const char *ssid, const char *password);
void wifi_manager_get_status(wifi_manager_status_t *status);

