#include "domain/master_types.h"
#include <stdio.h>

void master_ble_address_format(const master_ble_address_t *address,
                               char output[MASTER_MAC_STRING_SIZE])
{
    snprintf(output, MASTER_MAC_STRING_SIZE, "%02X:%02X:%02X:%02X:%02X:%02X",
             address->value[5], address->value[4], address->value[3],
             address->value[2], address->value[1], address->value[0]);
}

bool master_ble_address_parse(const char *text, master_ble_address_t *address)
{
    unsigned b[6];
    if (!text || !address ||
        sscanf(text, "%2x:%2x:%2x:%2x:%2x:%2x", &b[5], &b[4], &b[3],
               &b[2], &b[1], &b[0]) != 6) {
        return false;
    }
    for (size_t i = 0; i < 6; ++i) {
        address->value[i] = (uint8_t)b[i];
    }
    address->type = 0;
    return true;
}

bool master_ble_address_equal(const master_ble_address_t *a,
                              const master_ble_address_t *b)
{
    if (!a || !b || a->type != b->type) {
        return false;
    }
    for (size_t i = 0; i < 6; ++i) {
        if (a->value[i] != b->value[i]) {
            return false;
        }
    }
    return true;
}
