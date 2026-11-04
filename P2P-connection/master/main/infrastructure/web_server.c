#include "infrastructure/web_server.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "application/master_service.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "infrastructure/factory_reset.h"
#include "infrastructure/wifi_manager.h"

static const char *TAG = "web_server";
static httpd_handle_t s_server;
extern const uint8_t web_index_html_start[] asm("_binary_index_html_start");
extern const uint8_t web_index_html_end[] asm("_binary_index_html_end");

static esp_err_t send_json(httpd_req_t *request, cJSON *json, int status)
{
    char *text = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!text) return ESP_ERR_NO_MEM;
    httpd_resp_set_type(request, "application/json");
    if (status >= 400) httpd_resp_set_status(request, status == 400 ? "400 Bad Request" :
        status == 404 ? "404 Not Found" : "409 Conflict");
    esp_err_t err = httpd_resp_sendstr(request, text);
    free(text);
    return err;
}

static esp_err_t send_error(httpd_req_t *request, int status, const char *message)
{
    cJSON *json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "ok", false);
    cJSON_AddStringToObject(json, "error", message);
    return send_json(request, json, status);
}

static cJSON *receive_json(httpd_req_t *request)
{
    if (request->content_len <= 0 || request->content_len > 1024) return NULL;
    char *body = calloc(1, request->content_len + 1);
    if (!body) return NULL;
    size_t received = 0;
    while (received < request->content_len) {
        int count = httpd_req_recv(request, body + received, request->content_len - received);
        if (count <= 0) { free(body); return NULL; }
        received += count;
    }
    cJSON *json = cJSON_Parse(body);
    free(body);
    return json;
}

static esp_err_t index_get(httpd_req_t *request)
{
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, (const char *)web_index_html_start,
                           web_index_html_end - web_index_html_start);
}

static esp_err_t system_get(httpd_req_t *request)
{
    wifi_manager_status_t wifi;
    wifi_manager_get_status(&wifi);
    const esp_app_desc_t *app = esp_app_get_description();
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "firmware_version", app->version);
    cJSON_AddStringToObject(root, "environment", "dev");
    cJSON_AddNumberToObject(root, "protocol_version", SMART_IR_PROTOCOL_VERSION);
    cJSON *network = cJSON_AddObjectToObject(root, "wifi");
    cJSON_AddBoolToObject(network, "configured", wifi.sta_configured);
    cJSON_AddBoolToObject(network, "connected", wifi.sta_connected);
    cJSON_AddStringToObject(network, "ssid", wifi.sta_ssid);
    cJSON_AddStringToObject(network, "sta_ip", wifi.sta_ip);
    cJSON_AddBoolToObject(network, "ap_active", wifi.ap_active);
    cJSON_AddStringToObject(network, "ap_ssid", wifi.ap_ssid);
    cJSON_AddStringToObject(network, "ap_ip", wifi.ap_ip);
    return send_json(request, root, 200);
}

static cJSON *slave_to_json(const master_slave_view_t *slave)
{
    char mac[MASTER_MAC_STRING_SIZE];
    master_ble_address_format(&slave->address, mac);
    cJSON *json = cJSON_CreateObject();
    cJSON_AddNumberToObject(json, "id", slave->id);
    cJSON_AddStringToObject(json, "mac", mac);
    cJSON_AddBoolToObject(json, "connected", slave->connected);
    cJSON_AddBoolToObject(json, "bonded", slave->bonded);
    cJSON_AddBoolToObject(json, "running", slave->running);
    cJSON_AddBoolToObject(json, "awake", slave->awake);
    cJSON_AddNumberToObject(json, "sleep_ms", slave->sleep_ms);
    cJSON_AddNumberToObject(json, "awake_timeout_ms", slave->awake_timeout_ms);
    cJSON_AddNumberToObject(json, "rssi", slave->rssi);
    cJSON_AddNumberToObject(json, "pending", slave->pending_count);
    cJSON_AddNumberToObject(json, "last_event", slave->last_event);
    cJSON_AddNumberToObject(json, "last_error", slave->last_error);
    cJSON_AddNumberToObject(json, "battery_mv", slave->battery_mv);
    cJSON *ac = cJSON_AddObjectToObject(json, "ac");
    cJSON_AddBoolToObject(ac, "power", slave->ac_state.power);
    cJSON_AddNumberToObject(ac, "mode", slave->ac_state.mode);
    cJSON_AddNumberToObject(ac, "temperature", slave->ac_state.temperature_c);
    cJSON_AddNumberToObject(ac, "fan", slave->ac_state.fan);
    cJSON_AddNumberToObject(ac, "swing", slave->ac_state.swing);
    return json;
}

static esp_err_t slaves_get(httpd_req_t *request)
{
    master_slave_view_t slaves[MASTER_MAX_SLAVES];
    master_discovered_t discovered[MASTER_MAX_DISCOVERED];
    size_t slave_count = master_service_slaves_snapshot(slaves, MASTER_MAX_SLAVES);
    size_t discovered_count = master_service_discovered_snapshot(discovered, MASTER_MAX_DISCOVERED);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON *registered = cJSON_AddArrayToObject(root, "registered");
    for (size_t i = 0; i < slave_count; ++i) cJSON_AddItemToArray(registered, slave_to_json(&slaves[i]));
    cJSON *seen = cJSON_AddArrayToObject(root, "discovered");
    for (size_t i = 0; i < discovered_count; ++i) {
        char mac[MASTER_MAC_STRING_SIZE];
        master_ble_address_format(&discovered[i].address, mac);
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "mac", mac);
        cJSON_AddNumberToObject(item, "address_type", discovered[i].address.type);
        cJSON_AddNumberToObject(item, "advertised_id", discovered[i].advertised_id);
        cJSON_AddNumberToObject(item, "rssi", discovered[i].rssi);
        cJSON_AddBoolToObject(item, "provisioned_flag", discovered[i].provisioned_flag);
        cJSON_AddBoolToObject(item, "running_flag", discovered[i].running_flag);
        cJSON_AddItemToArray(seen, item);
    }
    return send_json(request, root, 200);
}

static esp_err_t wifi_post(httpd_req_t *request)
{
    cJSON *body = receive_json(request);
    cJSON *ssid = body ? cJSON_GetObjectItemCaseSensitive(body, "ssid") : NULL;
    cJSON *password = body ? cJSON_GetObjectItemCaseSensitive(body, "password") : NULL;
    if (!cJSON_IsString(ssid) || !cJSON_IsString(password)) {
        cJSON_Delete(body);
        return send_error(request, 400, "ssid and password are required");
    }
    char effective_password[65];
    if (password->valuestring[0] == '\0') {
        master_wifi_credentials_t config;
        master_service_get_wifi_credentials(&config);
        strlcpy(effective_password, config.password, sizeof(effective_password));
    } else {
        strlcpy(effective_password, password->valuestring, sizeof(effective_password));
    }
    esp_err_t err = wifi_manager_apply_credentials(ssid->valuestring, effective_password);
    cJSON_Delete(body);
    if (err != ESP_OK) return send_error(request, 400, esp_err_to_name(err));
    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "ok", true);
    cJSON_AddStringToObject(response, "message", "Saved; STA reconnect started");
    return send_json(request, response, 200);
}

static bool find_discovered_address(const char *mac, master_ble_address_t *address)
{
    master_discovered_t list[MASTER_MAX_DISCOVERED];
    size_t count = master_service_discovered_snapshot(list, MASTER_MAX_DISCOVERED);
    for (size_t i = 0; i < count; ++i) {
        char candidate[MASTER_MAC_STRING_SIZE];
        master_ble_address_format(&list[i].address, candidate);
        if (strcasecmp(mac, candidate) == 0) { *address = list[i].address; return true; }
    }
    return false;
}

static esp_err_t provision_post(httpd_req_t *request)
{
    cJSON *body = receive_json(request);
    cJSON *mac = body ? cJSON_GetObjectItemCaseSensitive(body, "mac") : NULL;
    cJSON *id = body ? cJSON_GetObjectItemCaseSensitive(body, "id") : NULL;
    cJSON *sleep = body ? cJSON_GetObjectItemCaseSensitive(body, "sleep_ms") : NULL;
    cJSON *awake = body ? cJSON_GetObjectItemCaseSensitive(body, "awake_timeout_ms") : NULL;
    if (!cJSON_IsString(mac) || !cJSON_IsNumber(id) || !cJSON_IsNumber(sleep) ||
        !cJSON_IsNumber(awake) || id->valuedouble < 0 || id->valuedouble > 255) {
        cJSON_Delete(body); return send_error(request, 400, "invalid provisioning payload");
    }
    master_ble_address_t address;
    if (!find_discovered_address(mac->valuestring, &address)) {
        cJSON_Delete(body); return send_error(request, 404, "slave has not been discovered");
    }
    esp_err_t err = master_service_provision(&address, id->valueint, sleep->valueint,
                                              awake->valueint);
    cJSON_Delete(body);
    if (err != ESP_OK) return send_error(request, err == ESP_ERR_INVALID_STATE ? 409 : 400,
                                         esp_err_to_name(err));
    cJSON *response = cJSON_CreateObject(); cJSON_AddBoolToObject(response, "ok", true);
    return send_json(request, response, 200);
}

static esp_err_t action_post(httpd_req_t *request)
{
    cJSON *body = receive_json(request);
    cJSON *id = body ? cJSON_GetObjectItemCaseSensitive(body, "id") : NULL;
    cJSON *action = body ? cJSON_GetObjectItemCaseSensitive(body, "action") : NULL;
    if (!cJSON_IsNumber(id) || !cJSON_IsString(action) || id->valuedouble < 0 || id->valuedouble > 255) {
        cJSON_Delete(body); return send_error(request, 400, "id and action are required");
    }
    esp_err_t err = ESP_ERR_NOT_SUPPORTED;
    if (strcmp(action->valuestring, "start") == 0) err = master_service_start(id->valueint);
    else if (strcmp(action->valuestring, "stop") == 0) err = master_service_stop(id->valueint);
    else if (strcmp(action->valuestring, "status") == 0) err = master_service_request_status(id->valueint);
    else if (strcmp(action->valuestring, "forget") == 0) err = master_service_forget(id->valueint);
    else if (strcmp(action->valuestring, "config") == 0) {
        cJSON *new_id = cJSON_GetObjectItemCaseSensitive(body, "new_id");
        cJSON *sleep = cJSON_GetObjectItemCaseSensitive(body, "sleep_ms");
        cJSON *awake = cJSON_GetObjectItemCaseSensitive(body, "awake_timeout_ms");
        if (cJSON_IsNumber(new_id) && new_id->valuedouble >= 0 &&
            new_id->valuedouble <= 255 && cJSON_IsNumber(sleep) &&
            cJSON_IsNumber(awake))
            err = master_service_set_config(id->valueint, new_id->valueint,
                                             sleep->valueint, awake->valueint);
    } else if (strcmp(action->valuestring, "ac") == 0) {
        cJSON *ac = cJSON_GetObjectItemCaseSensitive(body, "ac");
        cJSON *power = ac ? cJSON_GetObjectItemCaseSensitive(ac, "power") : NULL;
        cJSON *mode = ac ? cJSON_GetObjectItemCaseSensitive(ac, "mode") : NULL;
        cJSON *temp = ac ? cJSON_GetObjectItemCaseSensitive(ac, "temperature") : NULL;
        cJSON *fan = ac ? cJSON_GetObjectItemCaseSensitive(ac, "fan") : NULL;
        cJSON *swing = ac ? cJSON_GetObjectItemCaseSensitive(ac, "swing") : NULL;
        if (cJSON_IsBool(power) && cJSON_IsNumber(mode) && cJSON_IsNumber(temp) &&
            cJSON_IsNumber(fan) && cJSON_IsNumber(swing)) {
            smart_ir_ac_state_t state = { .power = cJSON_IsTrue(power), .mode = mode->valueint,
                .temperature_c = temp->valueint, .fan = fan->valueint, .swing = swing->valueint };
            err = master_service_set_ac(id->valueint, &state);
        }
    }
    cJSON_Delete(body);
    if (err != ESP_OK) return send_error(request, err == ESP_ERR_NOT_FOUND ? 404 : 400,
                                         esp_err_to_name(err));
    cJSON *response = cJSON_CreateObject(); cJSON_AddBoolToObject(response, "ok", true);
    return send_json(request, response, 200);
}

static esp_err_t factory_reset_post(httpd_req_t *request)
{
    cJSON *body = receive_json(request);
    cJSON *confirm = body ? cJSON_GetObjectItemCaseSensitive(body, "confirm") : NULL;
    if (!cJSON_IsBool(confirm) || !cJSON_IsTrue(confirm)) {
        cJSON_Delete(body); return send_error(request, 400, "explicit confirmation required");
    }
    cJSON_Delete(body);
    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "ok", true);
    cJSON_AddStringToObject(response, "message", "Factory reset scheduled");
    esp_err_t err = send_json(request, response, 200);
    factory_reset_schedule();
    return err;
}

esp_err_t web_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 8;
    config.stack_size = 8192;
    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &config), TAG, "start HTTP server");
    const httpd_uri_t routes[] = {
        { .uri = "/", .method = HTTP_GET, .handler = index_get },
        { .uri = "/api/system", .method = HTTP_GET, .handler = system_get },
        { .uri = "/api/slaves", .method = HTTP_GET, .handler = slaves_get },
        { .uri = "/api/wifi", .method = HTTP_POST, .handler = wifi_post },
        { .uri = "/api/slaves/provision", .method = HTTP_POST, .handler = provision_post },
        { .uri = "/api/slaves/action", .method = HTTP_POST, .handler = action_post },
        { .uri = "/api/factory-reset", .method = HTTP_POST, .handler = factory_reset_post },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &routes[i]), TAG, "route");
    }
    return ESP_OK;
}

