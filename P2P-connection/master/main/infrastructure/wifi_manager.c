#include "infrastructure/wifi_manager.h"
#include <string.h>
#include "application/master_service.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

static const char *TAG = "wifi_manager";
static wifi_manager_status_t s_status;
static char s_password[65];
static SemaphoreHandle_t s_lock;
static esp_timer_handle_t s_reconnect_timer;
static esp_timer_handle_t s_ap_disable_timer;

static void configure_ap(void)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    wifi_config_t ap = {0};
    snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "smart-ir-%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ap.ap.ssid_len = strlen((char *)ap.ap.ssid);
    ap.ap.channel = 6;
    ap.ap.ssid_hidden = 0;
#if CONFIG_SMART_IR_DIAGNOSTIC_OPEN_AP
    ap.ap.max_connection = 1;
#else
    ap.ap.max_connection = 4;
#endif
    ap.ap.beacon_interval = 100;
#if CONFIG_SMART_IR_DIAGNOSTIC_OPEN_AP
    ap.ap.authmode = WIFI_AUTH_OPEN;
#else
    strlcpy((char *)ap.ap.password, CONFIG_SMART_IR_AP_PASSWORD, sizeof(ap.ap.password));
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
#endif
    /* Match the ESP-IDF SoftAP example verified on the target board. */
    ap.ap.pmf_cfg.required = true;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
#if CONFIG_SMART_IR_DIAGNOSTIC_OPEN_AP
    ESP_LOGW(TAG, "DIAGNOSTIC OPEN SoftAP: SSID=%s channel=%u NO PASSWORD",
             (char *)ap.ap.ssid, ap.ap.channel);
#else
    ESP_LOGI(TAG, "SoftAP configured: SSID=%s channel=%u security=WPA2-CCMP",
             (char *)ap.ap.ssid, ap.ap.channel);
#endif
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_status.ap_ssid, (char *)ap.ap.ssid, sizeof(s_status.ap_ssid));
    strlcpy(s_status.ap_ip, "192.168.4.1", sizeof(s_status.ap_ip));
    s_status.ap_active = true;
    xSemaphoreGive(s_lock);
}

static void enable_ap_and_connect(void)
{
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    configure_ap();
    if (s_status.sta_configured) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.sta_connecting = true;
        xSemaphoreGive(s_lock);
        esp_wifi_connect();
    }
}

static void enable_ap_only(void)
{
    esp_wifi_set_mode(WIFI_MODE_AP);
    configure_ap();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.sta_connecting = false;
    xSemaphoreGive(s_lock);
}

static void reconnect_timer_cb(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Background STA reconnect");
    enable_ap_and_connect();
}

static void ap_disable_timer_cb(void *arg)
{
    (void)arg;
    if (s_status.sta_connected) {
        ESP_LOGI(TAG, "STA stable; disabling provisioning AP");
        esp_wifi_set_mode(WIFI_MODE_STA);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.ap_active = false;
        xSemaphoreGive(s_lock);
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_AP_START) {
        ESP_LOGI(TAG, "SoftAP started and broadcasting SSID=%s", s_status.ap_ssid);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.ap_active = true;
        xSemaphoreGive(s_lock);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STOP) {
        ESP_LOGW(TAG, "SoftAP stopped");
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.ap_active = false;
        s_status.ap_client_count = 0;
        xSemaphoreGive(s_lock);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        const wifi_event_ap_staconnected_t *event = data;
        ESP_LOGI(TAG, "AP client " MACSTR " connected (AID=%u)",
                 MAC2STR(event->mac), event->aid);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_status.ap_client_count < 4) ++s_status.ap_client_count;
        xSemaphoreGive(s_lock);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        const wifi_event_ap_stadisconnected_t *event = data;
        ESP_LOGW(TAG, "AP client " MACSTR " disconnected (AID=%u reason=%u)",
                 MAC2STR(event->mac), event->aid, event->reason);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_status.ap_client_count > 0) --s_status.ap_client_count;
        xSemaphoreGive(s_lock);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event = data;
        ESP_LOGW(TAG, "STA disconnected (reason=%u); returning to provisioning AP",
                 event->reason);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.sta_connected = false;
        s_status.sta_connecting = false;
        s_status.sta_ip[0] = '\0';
        xSemaphoreGive(s_lock);
        enable_ap_only();
        esp_timer_stop(s_reconnect_timer);
        esp_timer_start_once(s_reconnect_timer, 60ULL * 1000 * 1000);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = data;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.sta_connected = true;
        s_status.sta_connecting = false;
        snprintf(s_status.sta_ip, sizeof(s_status.sta_ip), IPSTR, IP2STR(&event->ip_info.ip));
        xSemaphoreGive(s_lock);
        esp_timer_stop(s_reconnect_timer);
        esp_timer_stop(s_ap_disable_timer);
        esp_timer_start_once(s_ap_disable_timer, 5ULL * 1000 * 1000);
    }
}

esp_err_t wifi_manager_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
    esp_err_t loop_err = esp_event_loop_create_default();
    if (loop_err != ESP_OK && loop_err != ESP_ERR_INVALID_STATE) return loop_err;
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), TAG, "Wi-Fi init");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_FLASH), TAG, "Wi-Fi flash storage");
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL);

    const esp_timer_create_args_t reconnect_args = { .callback = reconnect_timer_cb,
        .name = "wifi_reconnect" };
    const esp_timer_create_args_t ap_args = { .callback = ap_disable_timer_cb,
        .name = "ap_disable" };
    ESP_RETURN_ON_ERROR(esp_timer_create(&reconnect_args, &s_reconnect_timer), TAG, "timer");
    ESP_RETURN_ON_ERROR(esp_timer_create(&ap_args, &s_ap_disable_timer), TAG, "timer");

    master_wifi_credentials_t config;
    ESP_RETURN_ON_ERROR(master_service_get_wifi_credentials(&config), TAG, "load Wi-Fi config");
    s_status.sta_configured = config.configured;
    strlcpy(s_status.sta_ssid, config.ssid, sizeof(s_status.sta_ssid));
    strlcpy(s_password, config.password, sizeof(s_password));
    wifi_mode_t initial_mode = config.configured ? WIFI_MODE_APSTA : WIFI_MODE_AP;
    if (config.configured) {
        ESP_LOGI(TAG, "STA credentials found; starting AP+STA with BLE held off during authentication");
    } else {
        ESP_LOGI(TAG, "No STA credentials; starting AP-only (STA and BLE are off)");
    }
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(initial_mode), TAG, "set mode");
    configure_ap();
    if (config.configured) {
        wifi_config_t sta = {0};
        strlcpy((char *)sta.sta.ssid, config.ssid, sizeof(sta.sta.ssid));
        strlcpy((char *)sta.sta.password, config.password, sizeof(sta.sta.password));
        sta.sta.threshold.authmode = config.password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
        ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &sta), TAG, "set STA");
    }
    /* Keep the driver's default b/g/n protocol, bandwidth, AMPDU and power
       management settings. This is the profile proven by the isolated
       ESP-IDF WPA2 smoke test on this board. */
    ESP_LOGI(TAG, "SoftAP radio profile: ESP-IDF defaults, channel 6");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start Wi-Fi");
    if (config.configured) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.sta_connecting = true;
        xSemaphoreGive(s_lock);
        esp_wifi_connect();
    }
    return ESP_OK;
}

esp_err_t wifi_manager_apply_credentials(const char *ssid, const char *password)
{
    if (!ssid || !password || strlen(ssid) > 32 || strlen(password) > 64) return ESP_ERR_INVALID_ARG;
    ESP_RETURN_ON_ERROR(master_service_update_wifi(ssid, password), TAG, "persist Wi-Fi");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.sta_configured = ssid[0] != '\0';
    strlcpy(s_status.sta_ssid, ssid, sizeof(s_status.sta_ssid));
    strlcpy(s_password, password, sizeof(s_password));
    xSemaphoreGive(s_lock);
    if (ssid[0] == '\0') {
        enable_ap_only();
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "enable STA");
    configure_ap();
    wifi_config_t sta = {0};
    strlcpy((char *)sta.sta.ssid, ssid, sizeof(sta.sta.ssid));
    strlcpy((char *)sta.sta.password, password, sizeof(sta.sta.password));
    sta.sta.threshold.authmode = password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &sta), TAG, "apply Wi-Fi");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.sta_connecting = true;
    xSemaphoreGive(s_lock);
    esp_wifi_disconnect();
    ESP_RETURN_ON_ERROR(esp_wifi_connect(), TAG, "connect STA");
    return ESP_OK;
}

void wifi_manager_get_status(wifi_manager_status_t *status)
{
    if (!status || !s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *status = s_status;
    xSemaphoreGive(s_lock);
}

