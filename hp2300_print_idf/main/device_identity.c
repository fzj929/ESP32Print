#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_efuse.h"
#include "esp_efuse_table.h"
#include "esp_log.h"
#include "device_identity.h"

static const char *TAG = "IDENTITY";
static bool efuse_valid;
static bool write_protected;
static char uuid_text[37];

static uint32_t crc32_ieee(const uint8_t *data, size_t length)
{
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320U : 0);
    }
    return ~crc;
}

void device_identity_init(void)
{
    uint8_t image[32] = {0};
    write_protected = esp_efuse_read_field_bit(ESP_EFUSE_WR_DIS_BLOCK_USR_DATA);
    if (esp_efuse_read_field_blob(ESP_EFUSE_USER_DATA, image, sizeof(image) * 8) != ESP_OK) {
        ESP_LOGE(TAG, "Cannot read USER_DATA eFuse block");
        return;
    }
    uint32_t recorded_crc = (uint32_t)image[24] | ((uint32_t)image[25] << 8) |
                            ((uint32_t)image[26] << 16) | ((uint32_t)image[27] << 24);
    const uint8_t zero[3] = {0};
    efuse_valid = memcmp(image, "BIPS", 4) == 0 && image[4] == 1 &&
                  memcmp(image + 5, zero, 3) == 0 &&
                  memcmp(image + 28, (const uint8_t[4]){0}, 4) == 0 &&
                  crc32_ieee(image, 24) == recorded_crc && write_protected;
    if (!efuse_valid) {
        ESP_LOGW(TAG, "Identity not provisioned or invalid; BLOCK3 write-protected=%d",
                 write_protected);
        return;
    }
    const uint8_t *id = image + 8;
    snprintf(uuid_text, sizeof(uuid_text),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             id[0], id[1], id[2], id[3], id[4], id[5], id[6], id[7],
             id[8], id[9], id[10], id[11], id[12], id[13], id[14], id[15]);
    ESP_LOGI(TAG, "Immutable device ID: %s", uuid_text);
}

bool device_identity_efuse_valid(void) { return efuse_valid; }
bool device_identity_write_protected(void) { return write_protected; }
const char *device_identity_uuid(void) { return efuse_valid ? uuid_text : NULL; }
