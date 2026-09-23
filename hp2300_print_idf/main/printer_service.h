#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum { PRINT_IDLE, PRINT_QUEUED, PRINT_SENDING, PRINT_DELIVERED,
               PRINT_FAILED, PRINT_RESULT_UNKNOWN } print_state_t;

typedef struct {
    bool connected, ready, status_known;
    uint16_t vid, pid;
    uint8_t interface_number, alternate_setting, protocol;
    uint8_t bulk_out_endpoint, bulk_in_endpoint;
    uint16_t max_packet_size;
    char manufacturer[64], product[64], serial_number[64];
    char ieee1284_device_id[256];
    uint8_t port_status;
    print_state_t state;
    size_t total, sent;
    uint32_t job_id;
    char error[96];
} printer_snapshot_t;

void printer_service_start(void);
bool printer_submit(uint8_t *data, size_t length, uint32_t *job_id);
void printer_get_snapshot(printer_snapshot_t *snapshot);
const char *printer_state_name(print_state_t state);
