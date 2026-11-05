#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ble_mesh_example_init.h"
#include "esp_ble_mesh_common_api.h"
#include "esp_ble_mesh_config_model_api.h"
#include "esp_ble_mesh_networking_api.h"
#include "esp_ble_mesh_provisioning_api.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "smart_ir_mesh.h"

#define TAG "mesh_master"
#define MASTER_ADDR 0x0001
#define FIRST_NODE_ADDR 0x0100
#define APP_IDX 0x0000
#define MSG_TTL 3
#define COMMAND_TIMEOUT_MS 4000U
#define WAKE_SYNC_TIMEOUT_MS 1000U
#define QUEUE_RETRY_INTERVAL_MS 500U
#define DEFAULT_SLEEP_MS 10000U
#define DEFAULT_AWAKE_MS 5000U
#define STORE_NAMESPACE "mesh_master"
#define STORE_KEY "nodes"

typedef struct {
    bool used;
    bool ready;
    uint8_t id;
    uint8_t uuid[16];
    uint16_t addr;
    bool cycle_enabled;
    bool pending_stop;
    bool pending_ac;
    uint8_t pending_ac_state[5];
    uint32_t sleep_ms;
    uint32_t awake_ms;
} slave_slot_t;

typedef struct {
    uint32_t version;
    slave_slot_t slots[SMART_IR_MAX_SLAVES];
} master_store_t;

typedef struct {
    bool used;
    bool ready;
    uint8_t id;
    uint8_t uuid[16];
    uint16_t addr;
} slave_slot_v2_t;

typedef struct {
    uint32_t version;
    slave_slot_v2_t slots[SMART_IR_MAX_SLAVES];
} master_store_v2_t;

typedef struct {
    bool awake_known;
    bool awake;
    uint8_t inflight_seq;
    uint8_t inflight_command;
    uint8_t inflight_sync_flags;
} slave_runtime_t;

static uint8_t s_uuid[16];
static uint8_t s_sequence;
static uint8_t s_pending_uuid[SMART_IR_MAX_SLAVES][16];
static bool s_pending[SMART_IR_MAX_SLAVES];
static slave_runtime_t s_runtime[SMART_IR_MAX_SLAVES];
_Static_assert(SMART_IR_MAX_SLAVES <= 254,
               "Smart IR slave IDs are uint8_t; set BLE_MESH_MAX_PROV_NODES <= 254");

static master_store_t s_store = { .version = 3 };
static const uint8_t s_app_key[16] = {
    0x53, 0x6d, 0x61, 0x72, 0x74, 0x49, 0x52, 0x2d,
    0x4d, 0x65, 0x73, 0x68, 0x2d, 0x4b, 0x65, 0x79,
};

static esp_ble_mesh_cfg_srv_t s_cfg_srv = {
    .net_transmit = ESP_BLE_MESH_TRANSMIT(2, 20),
    .relay = ESP_BLE_MESH_RELAY_DISABLED,
    .relay_retransmit = ESP_BLE_MESH_TRANSMIT(2, 20),
    .beacon = ESP_BLE_MESH_BEACON_DISABLED,
    .friend_state = ESP_BLE_MESH_FRIEND_NOT_SUPPORTED,
    .default_ttl = 7,
};
static esp_ble_mesh_client_t s_cfg_cli;
static const esp_ble_mesh_client_op_pair_t s_op_pair[] = {
    { SMART_IR_OP_COMMAND, SMART_IR_OP_STATUS },
};
static esp_ble_mesh_client_t s_vendor_cli = {
    .op_pair_size = ARRAY_SIZE(s_op_pair), .op_pair = s_op_pair,
};
static esp_ble_mesh_model_op_t s_vendor_ops[] = {
    ESP_BLE_MESH_MODEL_OP(SMART_IR_OP_STATUS, 3),
    ESP_BLE_MESH_MODEL_OP(SMART_IR_OP_LIFECYCLE, 3),
    ESP_BLE_MESH_MODEL_OP_END,
};
static esp_ble_mesh_model_t s_root_models[] = {
    ESP_BLE_MESH_MODEL_CFG_SRV(&s_cfg_srv),
    ESP_BLE_MESH_MODEL_CFG_CLI(&s_cfg_cli),
};
static esp_ble_mesh_model_t s_vendor_models[] = {
    ESP_BLE_MESH_VENDOR_MODEL(SMART_IR_CID, SMART_IR_CLIENT_MODEL_ID,
                              s_vendor_ops, NULL, &s_vendor_cli),
};
static esp_ble_mesh_elem_t s_elements[] = {
    ESP_BLE_MESH_ELEMENT(0, s_root_models, s_vendor_models),
};
static esp_ble_mesh_comp_t s_comp = {
    .cid = SMART_IR_CID, .element_count = ARRAY_SIZE(s_elements), .elements = s_elements,
};
static esp_ble_mesh_prov_t s_prov = {
    .prov_uuid = s_uuid, .prov_unicast_addr = MASTER_ADDR,
    .prov_start_address = FIRST_NODE_ADDR,
};

static void store_save(void)
{
    nvs_handle_t h;
    if (nvs_open(STORE_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, STORE_KEY, &s_store, sizeof(s_store));
        nvs_commit(h);
        nvs_close(h);
    }
}

static void store_load(void)
{
    nvs_handle_t h;
    bool changed = false;
    if (nvs_open(STORE_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        size_t length = 0;
        if (nvs_get_blob(h, STORE_KEY, NULL, &length) == ESP_OK) {
            if (length == sizeof(master_store_t)) {
                master_store_t loaded = {0};
                if (nvs_get_blob(h, STORE_KEY, &loaded, &length) == ESP_OK && loaded.version == 3) {
                    s_store = loaded;
                }
            } else if (length == sizeof(master_store_v2_t)) {
                master_store_v2_t old = {0};
                if (nvs_get_blob(h, STORE_KEY, &old, &length) == ESP_OK && old.version == 2) {
                    for (size_t i = 0; i < SMART_IR_MAX_SLAVES; ++i) {
                        s_store.slots[i].used = old.slots[i].used;
                        s_store.slots[i].ready = old.slots[i].ready;
                        s_store.slots[i].id = old.slots[i].id;
                        memcpy(s_store.slots[i].uuid, old.slots[i].uuid, 16);
                        s_store.slots[i].addr = old.slots[i].addr;
                    }
                    changed = true;
                    ESP_LOGI(TAG, "Migrated master node table to schema 3");
                }
            }
        }
        nvs_close(h);
    }
    for (size_t i = 0; i < SMART_IR_MAX_SLAVES; ++i) {
        if (s_store.slots[i].used && s_store.slots[i].sleep_ms == 0) {
            s_store.slots[i].sleep_ms = DEFAULT_SLEEP_MS;
            s_store.slots[i].awake_ms = DEFAULT_AWAKE_MS;
            changed = true;
        }
    }
    if (changed) store_save();
}

static slave_slot_t *slot_by_addr(uint16_t addr)
{
    for (size_t i = 0; i < SMART_IR_MAX_SLAVES; ++i) {
        if (s_store.slots[i].used && s_store.slots[i].addr == addr) return &s_store.slots[i];
    }
    return NULL;
}

static slave_slot_t *slot_by_id(uint8_t id)
{
    for (size_t i = 0; i < SMART_IR_MAX_SLAVES; ++i) {
        if (s_store.slots[i].used && s_store.slots[i].id == id) return &s_store.slots[i];
    }
    return NULL;
}

static size_t slot_index(const slave_slot_t *slot)
{
    return (size_t)(slot - s_store.slots);
}

static void write_u32_le(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static int active_or_pending_count(void)
{
    int count = 0;
    for (size_t i = 0; i < SMART_IR_MAX_SLAVES; ++i) {
        if (s_store.slots[i].used) ++count;
        if (s_pending[i]) ++count;
    }
    return count;
}

static void set_common(esp_ble_mesh_client_common_param_t *common, uint16_t addr,
                       esp_ble_mesh_model_t *model, uint32_t opcode)
{
    memset(common, 0, sizeof(*common));
    common->opcode = opcode;
    common->model = model;
    common->ctx.net_idx = ESP_BLE_MESH_KEY_PRIMARY;
    common->ctx.app_idx = APP_IDX;
    common->ctx.addr = addr;
    common->ctx.send_ttl = MSG_TTL;
    common->msg_timeout = 4000;
}

static esp_err_t send_command(uint8_t id, uint8_t command,
                              const uint8_t *payload, size_t payload_len)
{
    slave_slot_t *slot = slot_by_id(id);
    if (!slot || !slot->ready) return ESP_ERR_INVALID_STATE;
    if (payload_len > 8) return ESP_ERR_INVALID_SIZE;

    uint8_t message[11] = { ++s_sequence, id, command };
    if (payload_len) memcpy(message + 3, payload, payload_len);
    esp_ble_mesh_msg_ctx_t ctx = {
        .net_idx = ESP_BLE_MESH_KEY_PRIMARY, .app_idx = APP_IDX,
        .addr = slot->addr, .send_ttl = MSG_TTL,
    };
    uint32_t timeout_ms = command == SMART_IR_CMD_WAKE_SYNC ?
                          WAKE_SYNC_TIMEOUT_MS : COMMAND_TIMEOUT_MS;
    esp_err_t err = esp_ble_mesh_client_model_send_msg(
        s_vendor_cli.model, &ctx, SMART_IR_OP_COMMAND,
        3 + payload_len, message, timeout_ms, true, ROLE_PROVISIONER);
    if (err == ESP_OK) {
        slave_runtime_t *runtime = &s_runtime[slot_index(slot)];
        runtime->inflight_seq = s_sequence;
        runtime->inflight_command = command;
        runtime->inflight_sync_flags =
            command == SMART_IR_CMD_WAKE_SYNC && payload_len ? payload[0] : 0;
        ESP_LOGI(TAG, "TX slave=%u addr=0x%04x cmd=0x%02x seq=%u",
                 id, slot->addr, command, s_sequence);
    }
    return err;
}

static esp_err_t dispatch_wake_queue(slave_slot_t *slot)
{
    slave_runtime_t *runtime = &s_runtime[slot_index(slot)];
    if (runtime->inflight_command) return ESP_ERR_INVALID_STATE;

    uint8_t payload[6] = {0};
    size_t payload_len = 1;
    if (slot->pending_stop) {
        payload[0] = SMART_IR_WAKE_FLAG_STOP;
    } else if (slot->pending_ac) {
        payload[0] = SMART_IR_WAKE_FLAG_AC;
        memcpy(payload + 1, slot->pending_ac_state, sizeof(slot->pending_ac_state));
        payload_len = sizeof(payload);
    }
    return send_command(slot->id, SMART_IR_CMD_WAKE_SYNC, payload, payload_len);
}

static void queue_retry_task(void *arg)
{
    (void)arg;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(QUEUE_RETRY_INTERVAL_MS));
        for (size_t i = 0; i < SMART_IR_MAX_SLAVES; ++i) {
            slave_slot_t *slot = &s_store.slots[i];
            slave_runtime_t *runtime = &s_runtime[i];
            if (!slot->used || !slot->ready || runtime->inflight_command ||
                (!slot->pending_stop && !slot->pending_ac)) {
                continue;
            }

            esp_err_t err = dispatch_wake_queue(slot);
            if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
                ESP_LOGW(TAG, "Queue retry failed for slave %u: %s",
                         slot->id, esp_err_to_name(err));
            }
        }
    }
}

static void configure_node(uint16_t addr)
{
    esp_ble_mesh_client_common_param_t common;
    esp_ble_mesh_cfg_client_get_state_t get = {0};
    set_common(&common, addr, s_cfg_cli.model, ESP_BLE_MESH_MODEL_OP_COMPOSITION_DATA_GET);
    get.comp_data_get.page = 0;
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_ble_mesh_config_client_get_state(&common, &get));
}

static void add_unprovisioned(const esp_ble_mesh_prov_cb_param_t *param)
{
    const uint8_t *uuid = param->provisioner_recv_unprov_adv_pkt.dev_uuid;
    if (active_or_pending_count() >= SMART_IR_MAX_SLAVES) return;
    for (size_t i = 0; i < SMART_IR_MAX_SLAVES; ++i) {
        if (s_store.slots[i].used && memcmp(s_store.slots[i].uuid, uuid, 16) == 0) return;
        if (s_pending[i] && memcmp(s_pending_uuid[i], uuid, 16) == 0) return;
    }
    size_t pending_slot = 0;
    while (pending_slot < SMART_IR_MAX_SLAVES && s_pending[pending_slot]) ++pending_slot;
    if (pending_slot == SMART_IR_MAX_SLAVES) return;

    esp_ble_mesh_unprov_dev_add_t dev = {0};
    memcpy(dev.addr, param->provisioner_recv_unprov_adv_pkt.addr, BD_ADDR_LEN);
    dev.addr_type = param->provisioner_recv_unprov_adv_pkt.addr_type;
    memcpy(dev.uuid, uuid, 16);
    dev.oob_info = param->provisioner_recv_unprov_adv_pkt.oob_info;
    dev.bearer = param->provisioner_recv_unprov_adv_pkt.bearer;
    esp_err_t err = esp_ble_mesh_provisioner_add_unprov_dev(
        &dev, ADD_DEV_RM_AFTER_PROV_FLAG | ADD_DEV_START_PROV_NOW_FLAG |
              ADD_DEV_FLUSHABLE_DEV_FLAG);
    if (err == ESP_OK) {
        s_pending[pending_slot] = true;
        memcpy(s_pending_uuid[pending_slot], uuid, 16);
        ESP_LOGI(TAG, "Provisioning slave candidate %d/%d", active_or_pending_count(),
                 SMART_IR_MAX_SLAVES);
    }
}

static void provisioning_cb(esp_ble_mesh_prov_cb_event_t event,
                            esp_ble_mesh_prov_cb_param_t *param)
{
    switch (event) {
    case ESP_BLE_MESH_PROVISIONER_RECV_UNPROV_ADV_PKT_EVT:
        add_unprovisioned(param);
        break;
    case ESP_BLE_MESH_PROVISIONER_PROV_COMPLETE_EVT: {
        slave_slot_t *slot = NULL;
        for (size_t i = 0; i < SMART_IR_MAX_SLAVES; ++i) {
            if (!s_store.slots[i].used) { slot = &s_store.slots[i]; break; }
        }
        if (!slot) break;
        slot->used = true;
        slot->ready = false;
        slot->id = (uint8_t)(slot - s_store.slots + 1);
        slot->addr = param->provisioner_prov_complete.unicast_addr;
        slot->sleep_ms = DEFAULT_SLEEP_MS;
        slot->awake_ms = DEFAULT_AWAKE_MS;
        memcpy(slot->uuid, param->provisioner_prov_complete.device_uuid, 16);
        for (size_t i = 0; i < SMART_IR_MAX_SLAVES; ++i) {
            if (s_pending[i] && memcmp(s_pending_uuid[i], slot->uuid, 16) == 0) s_pending[i] = false;
        }
        store_save();
        char name[12];
        snprintf(name, sizeof(name), "SLAVE-%u", slot->id);
        esp_ble_mesh_provisioner_set_node_name(param->provisioner_prov_complete.node_idx, name);
        ESP_LOGI(TAG, "Slave %u provisioned at 0x%04x", slot->id, slot->addr);
        configure_node(slot->addr);
        break;
    }
    case ESP_BLE_MESH_PROVISIONER_ADD_LOCAL_APP_KEY_COMP_EVT:
        if (param->provisioner_add_app_key_comp.err_code == 0) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(esp_ble_mesh_provisioner_bind_app_key_to_local_model(
                MASTER_ADDR, APP_IDX, SMART_IR_CLIENT_MODEL_ID, SMART_IR_CID));
        }
        break;
    default:
        break;
    }
}

static void config_client_cb(esp_ble_mesh_cfg_client_cb_event_t event,
                             esp_ble_mesh_cfg_client_cb_param_t *param)
{
    if (!param || !param->params) return;
    uint16_t addr = param->params->ctx.addr;
    if (param->error_code) {
        ESP_LOGW(TAG, "Config error=%d addr=0x%04x opcode=0x%04" PRIx32,
                 param->error_code, addr, param->params->opcode);
        return;
    }
    esp_ble_mesh_client_common_param_t common;
    esp_ble_mesh_cfg_client_set_state_t set = {0};
    if (event == ESP_BLE_MESH_CFG_CLIENT_GET_STATE_EVT &&
        param->params->opcode == ESP_BLE_MESH_MODEL_OP_COMPOSITION_DATA_GET) {
        set_common(&common, addr, s_cfg_cli.model, ESP_BLE_MESH_MODEL_OP_APP_KEY_ADD);
        set.app_key_add.net_idx = ESP_BLE_MESH_KEY_PRIMARY;
        set.app_key_add.app_idx = APP_IDX;
        memcpy(set.app_key_add.app_key, s_app_key, 16);
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_ble_mesh_config_client_set_state(&common, &set));
    } else if (event == ESP_BLE_MESH_CFG_CLIENT_SET_STATE_EVT &&
               param->params->opcode == ESP_BLE_MESH_MODEL_OP_APP_KEY_ADD) {
        set_common(&common, addr, s_cfg_cli.model, ESP_BLE_MESH_MODEL_OP_MODEL_APP_BIND);
        set.model_app_bind.element_addr = addr;
        set.model_app_bind.model_app_idx = APP_IDX;
        set.model_app_bind.model_id = SMART_IR_SERVER_MODEL_ID;
        set.model_app_bind.company_id = SMART_IR_CID;
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_ble_mesh_config_client_set_state(&common, &set));
    } else if (event == ESP_BLE_MESH_CFG_CLIENT_SET_STATE_EVT &&
               param->params->opcode == ESP_BLE_MESH_MODEL_OP_MODEL_APP_BIND) {
        slave_slot_t *slot = slot_by_addr(addr);
        if (slot) {
            slot->ready = true;
            store_save();
            uint8_t id = slot->id;
            ESP_LOGI(TAG, "Slave %u configured and ready", id);
            send_command(id, SMART_IR_CMD_SET_CONFIG, &id, 1);
        }
    } else if (event == ESP_BLE_MESH_CFG_CLIENT_TIMEOUT_EVT) {
        ESP_LOGW(TAG, "Config timeout addr=0x%04x; restarting configuration", addr);
        configure_node(addr);
    }
}

static void custom_model_cb(esp_ble_mesh_model_cb_event_t event,
                            esp_ble_mesh_model_cb_param_t *param)
{
    if (event == ESP_BLE_MESH_MODEL_OPERATION_EVT &&
        (param->model_operation.opcode == SMART_IR_OP_STATUS ||
         param->model_operation.opcode == SMART_IR_OP_LIFECYCLE)) {
        const uint8_t *msg = param->model_operation.msg;
        uint16_t len = param->model_operation.length;
        if (len < 3) return;
        uint8_t seq = msg[0], id = msg[1], response = msg[2];
        slave_slot_t *slot = slot_by_id(id);
        if (!slot) return;
        slave_runtime_t *runtime = &s_runtime[slot_index(slot)];

        bool lifecycle = param->model_operation.opcode == SMART_IR_OP_LIFECYCLE;
        if (lifecycle && response != SMART_IR_EVT_WAKE && response != SMART_IR_EVT_SLEEP) {
            ESP_LOGW(TAG, "Ignoring invalid lifecycle event 0x%02x from slave %u",
                     response, id);
            return;
        }
        if (!lifecycle && (response == SMART_IR_EVT_WAKE || response == SMART_IR_EVT_SLEEP)) {
            ESP_LOGW(TAG, "Ignoring lifecycle event on command-response opcode");
            return;
        }

        if (response == SMART_IR_EVT_WAKE) {
            runtime->awake_known = true;
            runtime->awake = true;
            ESP_LOGI(TAG, "Slave %u awake; dispatching queued work", id);
            /* queue_retry_task owns queued delivery. Only acknowledge an
             * empty wake here so awake_ms=0 can return to sleep promptly. */
            if (!slot->pending_stop && !slot->pending_ac && !runtime->inflight_command) {
                esp_err_t err = dispatch_wake_queue(slot);
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "Unable to acknowledge wake: %s", esp_err_to_name(err));
                }
            }
            return;
        }
        if (response == SMART_IR_EVT_SLEEP) {
            runtime->awake_known = true;
            runtime->awake = false;
            ESP_LOGI(TAG, "Slave %u entered deep sleep", id);
            return;
        }

        /* Any valid command response proves that the slave radio and
         * application are currently awake. In particular, a STOP ACK means
         * the slave remains awake in idle mode. */
        runtime->awake_known = true;
        runtime->awake = true;

        uint8_t nack_error = len > 3 ? msg[3] : 0xff;
        if (response == SMART_IR_EVT_NACK) {
            ESP_LOGW(TAG, "RX slave=%u NACK seq=%u error=0x%02x", id, seq, nack_error);
        } else if (response == SMART_IR_EVT_STATUS && len >= 10) {
            uint32_t uptime = (uint32_t)msg[5] | ((uint32_t)msg[6] << 8) |
                              ((uint32_t)msg[7] << 16) | ((uint32_t)msg[8] << 24);
            slot->cycle_enabled = msg[3] != 0;
            store_save();
            ESP_LOGI(TAG, "RX slave=%u STATUS running=%s uptime=%" PRIu32 "ms AC=%s %uC",
                     id, msg[3] ? "yes" : "no", uptime, msg[4] ? "on" : "off", msg[9]);
        } else {
            ESP_LOGI(TAG, "RX slave=%u event=0x%02x seq=%u", id, response, seq);
        }

        if (runtime->inflight_command && runtime->inflight_seq == seq) {
            uint8_t command = runtime->inflight_command;
            uint8_t sync_flags = runtime->inflight_sync_flags;
            bool accepted = response == SMART_IR_EVT_ACK || response == SMART_IR_EVT_STATUS;
            bool ac_processed = response == SMART_IR_EVT_NACK &&
                                nack_error == SMART_IR_NACK_UNSUPPORTED;
            if (command == SMART_IR_CMD_START && accepted) {
                slot->cycle_enabled = true;
            } else if (command == SMART_IR_CMD_STOP && accepted) {
                slot->cycle_enabled = false;
                slot->pending_stop = false;
            } else if (command == SMART_IR_CMD_WAKE_SYNC) {
                if ((sync_flags & SMART_IR_WAKE_FLAG_STOP) && accepted) {
                    slot->cycle_enabled = false;
                    slot->pending_stop = false;
                }
                if ((sync_flags & SMART_IR_WAKE_FLAG_AC) && (accepted || ac_processed)) {
                    slot->pending_ac = false;
                }
            }
            runtime->inflight_command = 0;
            runtime->inflight_sync_flags = 0;
            store_save();
        }
    } else if (event == ESP_BLE_MESH_CLIENT_MODEL_SEND_TIMEOUT_EVT) {
        uint16_t addr = param->client_send_timeout.ctx->addr;
        slave_slot_t *slot = slot_by_addr(addr);
        if (slot) {
            slave_runtime_t *runtime = &s_runtime[slot_index(slot)];
            /* A timeout only means that no response was heard. It is not
             * proof that the slave entered deep sleep. */
            runtime->awake_known = false;
            runtime->inflight_command = 0;
            runtime->inflight_sync_flags = 0;
        }
        ESP_LOGW(TAG, "Command response timeout for addr=0x%04x", addr);
    }
}

static void print_help(void)
{
    printf("\nCommands:\n"
           "  list\n  timing <id> <sleep_ms> <awake_ms>\n"
           "  start <id>\n  stop <id>\n  status <id>\n"
           "  ac <id> <power 0|1> <mode 0..4> <temp 16..30> <fan 0..5> <swing 0..3>\n\n");
}

static bool parse_u32(const char *text, uint32_t *value)
{
    if (!text || !*text || *text == '-') return false;
    char *end = NULL;
    unsigned long parsed = strtoul(text, &end, 0);
    if (!end || *end != '\0' || parsed > UINT32_MAX) return false;
    *value = (uint32_t)parsed;
    return true;
}

static size_t console_read_line(char *line, size_t capacity)
{
    static bool skip_lf_after_cr;
    size_t length = 0;

    while (true) {
        int ch = fgetc(stdin);
        if (ch == EOF) {
            clearerr(stdin);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (skip_lf_after_cr) {
            skip_lf_after_cr = false;
            if (ch == '\n') continue;
        }

        if (ch == '\r' || ch == '\n') {
            skip_lf_after_cr = (ch == '\r');
            line[length] = '\0';
            printf("\r\n");
            fflush(stdout);
            return length;
        }

        if (ch == '\b' || ch == 0x7f) {
            if (length > 0) {
                --length;
                printf("\b \b");
                fflush(stdout);
            }
            continue;
        }

        if (isprint((unsigned char)ch) && length + 1 < capacity) {
            line[length++] = (char)ch;
            putchar(ch);
            fflush(stdout);
        }
    }
}

static void console_task(void *arg)
{
    (void)arg;
    char line[128];
    print_help();
    while (true) {
        printf("mesh> ");
        fflush(stdout);
        if (console_read_line(line, sizeof(line)) == 0) continue;

        char *cmd = strtok(line, " ");
        if (!cmd) continue;
        if (!strcmp(cmd, "help")) { print_help(); continue; }
        if (!strcmp(cmd, "list")) {
            for (size_t i = 0; i < SMART_IR_MAX_SLAVES; ++i) {
                slave_slot_t *s = &s_store.slots[i];
                slave_runtime_t *runtime = &s_runtime[i];
                const char *link = !runtime->awake_known ? "unknown" :
                                   runtime->awake ? "awake" : "sleeping";
                printf("slave %u: %s addr=0x%04x ready=%s cycle=%s link=%s "
                       "timing=%" PRIu32 "/%" PRIu32 "ms queue=%s%s\n",
                       (unsigned)(i + 1), s->used ? "provisioned" : "empty",
                       s->addr, s->ready ? "yes" : "no",
                       s->cycle_enabled ? "on" : "off", link,
                       s->sleep_ms, s->awake_ms,
                       s->pending_stop ? "stop" : "",
                       s->pending_ac ? (s->pending_stop ? "+ac" : "ac") :
                                       (s->pending_stop ? "" : "empty"));
            }
            continue;
        }
        char *id_text = strtok(NULL, " \r\n");
        if (!id_text) { print_help(); continue; }
        uint32_t parsed_id = 0;
        if (!parse_u32(id_text, &parsed_id) || parsed_id < 1 || parsed_id > SMART_IR_MAX_SLAVES) {
            printf("invalid slave id\n");
            continue;
        }
        uint8_t id = (uint8_t)parsed_id;
        slave_slot_t *slot = slot_by_id(id);
        slave_runtime_t *runtime = slot ? &s_runtime[slot_index(slot)] : NULL;
        esp_err_t err = ESP_ERR_INVALID_ARG;
        if (!strcmp(cmd, "timing")) {
            uint32_t sleep_ms = 0, awake_ms = 0;
            bool valid = parse_u32(strtok(NULL, " \r\n"), &sleep_ms) &&
                         parse_u32(strtok(NULL, " \r\n"), &awake_ms) && sleep_ms > 0;
            if (valid && slot && !slot->cycle_enabled) {
                uint8_t payload[8];
                write_u32_le(payload, sleep_ms);
                write_u32_le(payload + 4, awake_ms);
                err = send_command(id, SMART_IR_CMD_SET_TIMING, payload, sizeof(payload));
                if (err == ESP_OK) {
                    slot->sleep_ms = sleep_ms;
                    slot->awake_ms = awake_ms;
                    store_save();
                }
            }
        } else if (!strcmp(cmd, "start")) {
            err = send_command(id, SMART_IR_CMD_START, NULL, 0);
            if (err == ESP_OK && slot) {
                slot->pending_stop = false;
                store_save();
            }
        } else if (!strcmp(cmd, "stop")) {
            if (slot && slot->cycle_enabled) {
                slot->pending_stop = true;
                slot->pending_ac = false;
                store_save();
                printf("stop queued for slave %u; retrying until acknowledged\n", id);
                err = ESP_OK;
            } else {
                err = send_command(id, SMART_IR_CMD_STOP, NULL, 0);
            }
        } else if (!strcmp(cmd, "status")) {
            if (slot && runtime->awake_known && !runtime->awake) {
                printf("slave %u is sleeping; status is available at its next wake window\n", id);
                err = ESP_OK;
            } else {
                err = send_command(id, SMART_IR_CMD_GET_STATUS, NULL, 0);
            }
        }
        else if (!strcmp(cmd, "ac")) {
            uint8_t p[5];
            bool valid = true;
            for (size_t i = 0; i < 5; ++i) {
                char *value = strtok(NULL, " \r\n");
                uint32_t parsed = 0;
                if (!parse_u32(value, &parsed) || parsed > UINT8_MAX) { valid = false; break; }
                p[i] = (uint8_t)parsed;
            }
            valid = valid && p[0] <= 1 && p[1] <= 4 && p[2] >= 16 && p[2] <= 30 &&
                    p[3] <= 5 && p[4] <= 3;
            if (valid && slot && slot->cycle_enabled) {
                memcpy(slot->pending_ac_state, p, sizeof(p));
                slot->pending_ac = true;
                store_save();
                printf("AC command queued for slave %u; retrying until acknowledged\n", id);
                err = ESP_OK;
            } else if (valid) {
                err = send_command(id, SMART_IR_CMD_SET_AC_STATE, p, sizeof(p));
            }
        }
        if (err != ESP_OK) printf("command failed: %s\n", esp_err_to_name(err));
    }
}

static esp_err_t mesh_init(void)
{
    const uint8_t match[] = { SMART_IR_UUID_0, SMART_IR_UUID_1 };
    esp_ble_mesh_register_prov_callback(provisioning_cb);
    esp_ble_mesh_register_config_client_callback(config_client_cb);
    esp_ble_mesh_register_custom_model_callback(custom_model_cb);
    ESP_RETURN_ON_ERROR(esp_ble_mesh_init(&s_prov, &s_comp), TAG, "mesh init");
    ESP_RETURN_ON_ERROR(esp_ble_mesh_client_model_init(&s_vendor_models[0]), TAG, "vendor client init");
    ESP_RETURN_ON_ERROR(esp_ble_mesh_provisioner_set_dev_uuid_match(match, sizeof(match), 0, false), TAG, "UUID match");
    ESP_RETURN_ON_ERROR(esp_ble_mesh_provisioner_prov_enable(
        (esp_ble_mesh_prov_bearer_t)(ESP_BLE_MESH_PROV_ADV | ESP_BLE_MESH_PROV_GATT)), TAG, "provisioner enable");
    esp_err_t err = esp_ble_mesh_provisioner_add_local_app_key(
        s_app_key, ESP_BLE_MESH_KEY_PRIMARY, APP_IDX);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    return ESP_OK;
}

void app_main(void)
{
    ESP_LOGI(TAG,
             "Smart IR master %s (environment=%s, protocol=%u, transport=BLE Mesh)",
             esp_app_get_description()->version, SMART_IR_ENVIRONMENT,
             SMART_IR_PROTOCOL_VERSION);
    ESP_LOGI(TAG, "Provisioner capacity: %d slave nodes", SMART_IR_MAX_SLAVES);
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    store_load();
    ESP_ERROR_CHECK(bluetooth_init());
    ble_mesh_get_dev_uuid(s_uuid);
    ESP_ERROR_CHECK(mesh_init());
    ESP_LOGI(TAG, "Startup complete; Mesh provisioning and command console enabled");
    xTaskCreate(console_task, "mesh_console", 4096, NULL, 4, NULL);
    xTaskCreate(queue_retry_task, "mesh_queue", 3072, NULL, 3, NULL);
}
