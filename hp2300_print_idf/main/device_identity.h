#pragma once
#include <stdbool.h>

// BLOCK3 / USER_DATA is reserved in full for the product identity image.
// Do not enable a custom MAC in this block on production boards.
void device_identity_init(void);
bool device_identity_efuse_valid(void);
bool device_identity_write_protected(void);
const char *device_identity_uuid(void);
