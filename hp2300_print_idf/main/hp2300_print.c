#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "usb/usb_host.h"
#include "status_led.h"
#include "printer_service.h"
#include "web_service.h"

#if !CONFIG_IDF_TARGET_ESP32S3
#error This application requires ESP32-S3
#endif

#define OUT_CHUNK_SIZE 1024

static const char *TAG = "PRINTER";
static SemaphoreHandle_t job_mutex;
static printer_snapshot_t job_state;
static uint8_t *pending_job;
static size_t pending_size;
static uint32_t next_job_id;
static usb_host_client_handle_t client;
static usb_device_handle_t device;
static usb_transfer_t *control_transfer;
static usb_transfer_t *out_transfer;
static usb_transfer_t *in_transfer;
static volatile bool control_done;
static volatile bool out_done;
static volatile bool in_done;
static volatile bool gone;
static bool control_inflight;
static bool out_inflight;
static bool in_inflight;
static bool interface_claimed;
static bool usb_fault;
static char fault_reason[96];
static bool in_enabled;
static uint8_t pending_address;
static bool skipped_addresses[128];
static uint8_t printer_interface;
static uint8_t printer_alt;
static uint8_t printer_protocol;
static uint8_t out_ep;
static uint8_t in_ep;
static uint16_t out_mps;
static uint16_t in_mps;
static status_led_state_t job_phase = LED_OPENING;

const char *printer_state_name(print_state_t value)
{
    switch (value) {
    case PRINT_IDLE: return "idle";
    case PRINT_QUEUED: return "queued";
    case PRINT_SENDING: return "sending_usb";
    case PRINT_DELIVERED: return "usb_delivered";
    case PRINT_FAILED: return "failed_before_usb";
    case PRINT_RESULT_UNKNOWN: return "failed_unknown";
    default: return "unknown";
    }
}

void printer_get_snapshot(printer_snapshot_t *snapshot)
{
    xSemaphoreTake(job_mutex, portMAX_DELAY);
    *snapshot = job_state;
    xSemaphoreGive(job_mutex);
}

bool printer_submit(uint8_t *data, size_t length, uint32_t *job_id)
{
    if (!data || !length || !job_id) return false;
    xSemaphoreTake(job_mutex, portMAX_DELAY);
    bool accepted = job_state.connected && job_state.ready && !pending_job &&
        job_state.state != PRINT_SENDING && job_state.state != PRINT_RESULT_UNKNOWN;
    if (accepted) {
        pending_job = data;
        pending_size = length;
        job_state.state = PRINT_QUEUED;
        job_state.total = length;
        job_state.sent = 0;
        job_state.error[0] = 0;
        job_state.job_id = ++next_job_id;
        *job_id = job_state.job_id;
        ESP_LOGI(TAG, "Job #%" PRIu32 " queued: %u bytes", *job_id, (unsigned)length);
    }
    xSemaphoreGive(job_mutex);
    return accepted;
}

static int64_t millis_now(void)
{
    return esp_timer_get_time() / 1000;
}

static void client_event(const usb_host_client_event_msg_t *event, void *arg)
{
    (void)arg;
    if (event->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        ESP_LOGI(TAG, "NEW_DEV address=%u", event->new_dev.address);
        if (event->new_dev.address < 128) skipped_addresses[event->new_dev.address] = false;
        if (!device) pending_address = event->new_dev.address;
    } else if (event->event == USB_HOST_CLIENT_EVENT_DEV_GONE &&
               event->dev_gone.dev_hdl == device) {
        gone = true;
        xSemaphoreTake(job_mutex, portMAX_DELAY);
        job_state.connected = false;
        job_state.ready = false;
        job_state.status_known = false;
        if (job_state.state == PRINT_SENDING) {
            job_state.state = PRINT_RESULT_UNKNOWN;
            snprintf(job_state.error, sizeof(job_state.error), "Printer disconnected; page may be partial");
        }
        xSemaphoreGive(job_mutex);
        status_led_set(LED_DISCONNECTED);
        ESP_LOGW(TAG, "Printer disconnected; cleaning up USB connection without replay");
    }
}

static void transfer_callback(usb_transfer_t *transfer)
{
    if (transfer == control_transfer) {
        control_inflight = false;
        control_done = true;
    }
    if (transfer == out_transfer) {
        out_inflight = false;
        out_done = true;
    }
    if (transfer == in_transfer) {
        in_inflight = false;
        in_done = true;
    }
}

static void host_events(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t flags = 0;
        esp_err_t err = usb_host_lib_handle_events(pdMS_TO_TICKS(1000), &flags);
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
            ESP_LOGE(TAG, "Host event error: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (flags) ESP_LOGD(TAG, "Host flags=0x%08" PRIx32, flags);
    }
}

static void pump(void)
{
    status_led_tick();
    esp_err_t err = usb_host_client_handle_events(client, pdMS_TO_TICKS(10));
    if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
        ESP_LOGE(TAG, "Client event error: %s", esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (in_enabled && in_done) {
        in_done = false;
        if (gone || in_transfer->status != USB_TRANSFER_STATUS_COMPLETED) {
            ESP_LOGW(TAG, "Bulk IN stopped, transfer status=%d", in_transfer->status);
            in_enabled = false;
            return;
        }
        int length = in_transfer->actual_num_bytes;
        if (length > 0) {
            ESP_LOGI(TAG, "Bulk IN received %d byte(s)", length);
        }
        in_transfer->num_bytes = in_mps;
        err = usb_host_transfer_submit(in_transfer);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Bulk IN resubmit failed: %s", esp_err_to_name(err));
            in_enabled = false;
        } else {
            in_inflight = true;
        }
    }
}

static void set_usb_fault(const char *reason)
{
    if (!usb_fault) snprintf(fault_reason, sizeof(fault_reason), "%s", reason);
    usb_fault = true;
    ESP_LOGE(TAG, "USB connection fault: %s", reason);
}

static bool usb_ok(esp_err_t err, const char *operation)
{
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s: %s", operation, esp_err_to_name(err));
        set_usb_fault(operation);
        return false;
    }
    return true;
}

static bool read_status(void)
{
    // USB Printer Class GET_PORT_STATUS uses the discovered interface number.
    const uint8_t setup[8] = {0xa1, 0x01, 0, 0, printer_interface, printer_alt, 1, 0};
    memcpy(control_transfer->data_buffer, setup, sizeof(setup));
    control_transfer->data_buffer[8] = 0;
    control_transfer->num_bytes = 9;
    control_done = false;
    if (!usb_ok(usb_host_transfer_submit_control(client, control_transfer),
                "GET_PORT_STATUS submit")) return false;
    control_inflight = true;
    int64_t started = millis_now();
    while (!control_done) {
        pump();
        if (gone || usb_fault) return false;
        if (millis_now() - started > 3000) {
            set_usb_fault("status request timeout");
            return false;
        }
    }
    if (control_transfer->status != USB_TRANSFER_STATUS_COMPLETED ||
        control_transfer->actual_num_bytes < 9) {
        ESP_LOGW(TAG, "Status unavailable: transfer=%d actual=%d",
                 control_transfer->status, control_transfer->actual_num_bytes);
        xSemaphoreTake(job_mutex, portMAX_DELAY);
        job_state.ready = false;
        job_state.status_known = false;
        xSemaphoreGive(job_mutex);
        status_led_set(LED_STATUS_UNKNOWN);
        return false;
    }
    uint8_t value = control_transfer->data_buffer[8];
    bool paper_empty = (value & 0x20) != 0;
    bool selected = (value & 0x10) != 0;
    bool no_error = (value & 0x08) != 0;
    ESP_LOGI(TAG, "STATUS raw=0x%02x paper_empty=%s selected=%s error=%s",
             value, paper_empty ? "YES" : "NO", selected ? "YES" : "NO",
             no_error ? "NO" : "YES");
    bool ready = !paper_empty && selected && no_error;
    xSemaphoreTake(job_mutex, portMAX_DELAY);
    job_state.port_status = value;
    job_state.ready = ready;
    job_state.status_known = true;
    xSemaphoreGive(job_mutex);
    status_led_set(ready ? job_phase : LED_NOT_READY);
    return ready;
}

static bool allocate_transfer(usb_transfer_t **transfer, size_t bytes, uint8_t endpoint)
{
    if (!usb_ok(usb_host_transfer_alloc(bytes, 0, transfer), "allocate transfer")) return false;
    (*transfer)->device_handle = device;
    (*transfer)->bEndpointAddress = endpoint;
    (*transfer)->callback = transfer_callback;
    (*transfer)->context = NULL;
    return true;
}

static bool parse_printer_interface(const usb_config_desc_t *config)
{
    const uint8_t *bytes = (const uint8_t *)config;
    bool matching = false;
    uint8_t candidate_interface = 0, candidate_alt = 0, candidate_protocol = 0;
    uint8_t candidate_out = 0, candidate_in = 0;
    uint16_t candidate_out_mps = 0, candidate_in_mps = 0;
    for (size_t pos = 0; pos + 2 <= config->wTotalLength;) {
        uint8_t length = bytes[pos];
        uint8_t type = bytes[pos + 1];
        if (length < 2 || pos + length > config->wTotalLength) {
            set_usb_fault("malformed USB configuration descriptor");
            return false;
        }
        if (type == 4 && length >= 9) {
            if (matching && candidate_out && candidate_out_mps) break;
            matching = bytes[pos + 5] == 7 && bytes[pos + 6] == 1 &&
                       (bytes[pos + 7] == 1 || bytes[pos + 7] == 2);
            candidate_interface = bytes[pos + 2];
            candidate_alt = bytes[pos + 3];
            candidate_protocol = bytes[pos + 7];
            candidate_out = candidate_in = 0;
            candidate_out_mps = candidate_in_mps = 0;
        } else if (type == 5 && length >= 7 && matching &&
                   (bytes[pos + 3] & 3) == 2) {
            uint8_t endpoint = bytes[pos + 2];
            uint16_t mps = (bytes[pos + 4] | ((uint16_t)bytes[pos + 5] << 8)) & 0x7ff;
            if (endpoint & 0x80) {
                candidate_in = endpoint;
                candidate_in_mps = mps;
            } else {
                candidate_out = endpoint;
                candidate_out_mps = mps;
            }
        }
        pos += length;
    }
    if (!matching || !candidate_out || !candidate_out_mps) return false;
    printer_interface = candidate_interface;
    printer_alt = candidate_alt;
    printer_protocol = candidate_protocol;
    out_ep = candidate_out;
    out_mps = candidate_out_mps;
    in_ep = candidate_in;
    in_mps = candidate_in_mps;
    return true;
}

static bool optional_control_read(const uint8_t setup[8], uint16_t requested,
                                  const uint8_t **payload, int *payload_length)
{
    if (gone || usb_fault) return false;
    memcpy(control_transfer->data_buffer, setup, 8);
    control_transfer->num_bytes = 8 + requested;
    control_done = false;
    esp_err_t err = usb_host_transfer_submit_control(client, control_transfer);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Optional control request submit failed: %s", esp_err_to_name(err));
        return false;
    }
    control_inflight = true;
    int64_t started = millis_now();
    while (!control_done && !gone && millis_now() - started < 3000) pump();
    if (!control_done && !gone) set_usb_fault("optional control request timeout");
    if (gone || usb_fault) return false;
    if (control_transfer->status != USB_TRANSFER_STATUS_COMPLETED ||
        control_transfer->actual_num_bytes < 8) {
        ESP_LOGW(TAG, "Optional control request failed: transfer=%d",
                 control_transfer->status);
        return false;
    }
    *payload = control_transfer->data_buffer + 8;
    *payload_length = control_transfer->actual_num_bytes - 8;
    return true;
}

static uint16_t read_language_id(void)
{
    const uint8_t setup[8] = {0x80, 0x06, 0x00, 0x03, 0, 0, 0x04, 0};
    const uint8_t *data;
    int length;
    if (optional_control_read(setup, 4, &data, &length) &&
        length >= 4 && data[1] == 3)
        return (uint16_t)data[2] | ((uint16_t)data[3] << 8);
    return 0x0409;
}

static void read_usb_string(uint8_t index, uint16_t language, char *output, size_t capacity)
{
    if (!index || capacity == 0) return;
    const uint8_t setup[8] = {0x80, 0x06, index, 0x03,
                              language & 0xff, language >> 8, 0x7e, 0};
    const uint8_t *data;
    int length;
    if (!optional_control_read(setup, 126, &data, &length) ||
        length < 2 || data[1] != 3) return;
    if (data[0] < length) length = data[0];
    size_t used = 0;
    for (int i = 2; i + 1 < length && used + 1 < capacity; i += 2) {
        uint16_t codepoint = (uint16_t)data[i] | ((uint16_t)data[i + 1] << 8);
        // The identity fields are diagnostic hints. Preserve ASCII and avoid
        // emitting malformed UTF-8 for unsupported UTF-16 code points.
        output[used++] = codepoint >= 32 && codepoint < 127 ? (char)codepoint : '?';
    }
    output[used] = 0;
}

static void read_ieee1284_device_id(void)
{
    const uint8_t setup[8] = {0xa1, 0x00, 0, 0,
                              printer_interface, printer_alt, 0x00, 0x02};
    const uint8_t *data;
    int length;
    if (!optional_control_read(setup, 512, &data, &length) || length < 3) return;
    int declared = ((int)data[0] << 8) | data[1];
    int text_length = length - 2;
    if (declared >= 2 && declared - 2 < text_length) text_length = declared - 2;
    if (text_length >= (int)sizeof(job_state.ieee1284_device_id))
        text_length = sizeof(job_state.ieee1284_device_id) - 1;
    char text[sizeof(job_state.ieee1284_device_id)];
    for (int i = 0; i < text_length; ++i)
        text[i] = data[i + 2] >= 32 && data[i + 2] < 127 ? data[i + 2] : ' ';
    text[text_length] = 0;
    xSemaphoreTake(job_mutex, portMAX_DELAY);
    snprintf(job_state.ieee1284_device_id, sizeof(job_state.ieee1284_device_id), "%s", text);
    xSemaphoreGive(job_mutex);
    ESP_LOGI(TAG, "IEEE 1284 Device ID available (%d bytes)", text_length);
}

static void cleanup_connection(void)
{
    in_enabled = false;
    if (device && interface_claimed) {
        if (out_ep && usb_host_endpoint_halt(device, out_ep) == ESP_OK)
            usb_host_endpoint_flush(device, out_ep);
        if (in_ep && usb_host_endpoint_halt(device, in_ep) == ESP_OK)
            usb_host_endpoint_flush(device, in_ep);
    }
    int64_t deadline = millis_now() + 3000;
    while ((control_inflight || out_inflight || in_inflight) && millis_now() < deadline)
        usb_host_client_handle_events(client, pdMS_TO_TICKS(20));
    // A transfer must never be freed while it is still in-flight. If the Host
    // stack cannot drain it after detach, restart automatically as a fallback.
    if (control_inflight || out_inflight || in_inflight) {
        ESP_LOGE(TAG, "USB transfers did not drain; restarting Host automatically");
        esp_restart();
    }
    if (usb_host_transfer_free(in_transfer) != ESP_OK ||
        usb_host_transfer_free(out_transfer) != ESP_OK ||
        usb_host_transfer_free(control_transfer) != ESP_OK) {
        ESP_LOGE(TAG, "USB transfer cleanup failed; restarting Host automatically");
        esp_restart();
    }
    in_transfer = out_transfer = control_transfer = NULL;
    if (device && interface_claimed &&
        usb_host_interface_release(client, device, printer_interface) != ESP_OK) {
        ESP_LOGE(TAG, "USB interface release failed; restarting Host automatically");
        esp_restart();
    }
    interface_claimed = false;
    if (device && usb_host_device_close(client, device) != ESP_OK) {
        ESP_LOGE(TAG, "USB device close failed; restarting Host automatically");
        esp_restart();
    }
    device = NULL;
    out_ep = in_ep = 0;
    out_mps = in_mps = 0;
    control_done = out_done = in_done = false;
    uint8_t *discard;
    xSemaphoreTake(job_mutex, portMAX_DELAY);
    discard = pending_job;
    pending_job = NULL;
    pending_size = 0;
    if (job_state.state == PRINT_SENDING)
        job_state.state = PRINT_RESULT_UNKNOWN;
    else if (job_state.state == PRINT_QUEUED)
        job_state.state = PRINT_FAILED;
    job_state.connected = false;
    job_state.ready = false;
    job_state.status_known = false;
    if (usb_fault) snprintf(job_state.error, sizeof(job_state.error), "%s", fault_reason);
    else if (job_state.state == PRINT_RESULT_UNKNOWN)
        snprintf(job_state.error, sizeof(job_state.error), "Printer disconnected; page may be partial");
    xSemaphoreGive(job_mutex);
    free(discard);
    status_led_set(gone ? LED_DISCONNECTED : LED_ERROR);
    ESP_LOGI(TAG, "USB connection closed; waiting for re-enumeration");
}

static void open_and_print(uint8_t address)
{
    gone = false;
    usb_fault = false;
    fault_reason[0] = 0;
    job_phase = LED_WAITING;
    status_led_set(LED_OPENING);
    if (!usb_ok(usb_host_device_open(client, address, &device), "open USB device")) goto cleanup;

    const usb_device_desc_t *descriptor = NULL;
    if (!usb_ok(usb_host_get_device_descriptor(device, &descriptor),
                "read device descriptor")) goto cleanup;
    ESP_LOGI(TAG, "Device address=%u VID=%04x PID=%04x", address,
             descriptor->idVendor, descriptor->idProduct);
    const usb_config_desc_t *config = NULL;
    if (!usb_ok(usb_host_get_active_config_descriptor(device, &config),
                "read active configuration")) goto cleanup;
    if (!parse_printer_interface(config)) {
        if (!usb_fault) {
            ESP_LOGW(TAG, "No standard USB Printer Class Protocol 1/2 interface with Bulk OUT");
            if (address < 128) skipped_addresses[address] = true;
        }
        goto cleanup;
    }
    ESP_LOGI(TAG, "Interface=%u alt=%u protocol=%u OUT=0x%02x MPS=%u IN=0x%02x MPS=%u",
             printer_interface, printer_alt, printer_protocol,
             out_ep, out_mps, in_ep, in_mps);

    if (!usb_ok(usb_host_interface_claim(client, device, printer_interface, printer_alt),
                "claim printer interface")) goto cleanup;
    interface_claimed = true;
    if (!allocate_transfer(&control_transfer, 520, 0) ||
        !allocate_transfer(&out_transfer, OUT_CHUNK_SIZE, out_ep) ||
        (in_ep && !allocate_transfer(&in_transfer, in_mps, in_ep))) goto cleanup;
    uint16_t language = read_language_id();
    char manufacturer[64] = {0}, product[64] = {0}, serial[64] = {0};
    read_usb_string(descriptor->iManufacturer, language, manufacturer, sizeof(manufacturer));
    read_usb_string(descriptor->iProduct, language, product, sizeof(product));
    read_usb_string(descriptor->iSerialNumber, language, serial, sizeof(serial));
    if (gone || usb_fault) goto cleanup;
    xSemaphoreTake(job_mutex, portMAX_DELAY);
    job_state.connected = true;
    job_state.vid = descriptor->idVendor;
    job_state.pid = descriptor->idProduct;
    job_state.interface_number = printer_interface;
    job_state.alternate_setting = printer_alt;
    job_state.protocol = printer_protocol;
    job_state.bulk_out_endpoint = out_ep;
    job_state.bulk_in_endpoint = in_ep;
    job_state.max_packet_size = out_mps;
    snprintf(job_state.manufacturer, sizeof(job_state.manufacturer), "%s", manufacturer);
    snprintf(job_state.product, sizeof(job_state.product), "%s", product);
    snprintf(job_state.serial_number, sizeof(job_state.serial_number), "%s", serial);
    xSemaphoreGive(job_mutex);
    read_ieee1284_device_id();
    if (gone || usb_fault) goto cleanup;
    if (in_transfer) {
        in_transfer->num_bytes = in_mps;
        in_enabled = true;
        if (!usb_ok(usb_host_transfer_submit(in_transfer), "start Bulk IN monitor"))
            goto cleanup;
        in_inflight = true;
    }

    ESP_LOGI(TAG, "Printer connected; waiting for uploaded PRN page");
    int64_t last_status = 0;
    for (;;) {
        if (gone || usb_fault) break;
        if (millis_now() - last_status >= 2000) {
            last_status = millis_now();
            read_status();
            if (gone || usb_fault) break;
        }
        uint8_t *data = NULL;
        size_t job_size = 0;
        uint32_t id = 0;
        xSemaphoreTake(job_mutex, portMAX_DELAY);
        if (pending_job && job_state.ready) {
            data = pending_job;
            job_size = pending_size;
            id = job_state.job_id;
            pending_job = NULL;
            pending_size = 0;
            job_state.state = PRINT_SENDING;
        }
        xSemaphoreGive(job_mutex);
        if (!data) { pump(); continue; }

        ESP_LOGI(TAG, "Job #%" PRIu32 " USB send start: %u bytes", id, (unsigned)job_size);
        job_phase = LED_SENDING;
        status_led_set(job_phase);
        for (size_t offset = 0; offset < job_size;) {
            size_t count = job_size - offset;
            if (count > OUT_CHUNK_SIZE) count = OUT_CHUNK_SIZE;
            memcpy(out_transfer->data_buffer, data + offset, count);
            out_transfer->num_bytes = count;
            out_done = false;
            if (!usb_ok(usb_host_transfer_submit(out_transfer), "Bulk OUT submit")) {
                free(data);
                goto cleanup;
            }
            out_inflight = true;
            int64_t started = millis_now();
            while (!out_done) {
                pump();
                if (gone || usb_fault) {
                    free(data);
                    goto cleanup;
                }
                if (millis_now() - started > 60000) {
                    set_usb_fault("Bulk OUT timeout; job may be partial");
                    free(data);
                    goto cleanup;
                }
            }
            if (out_transfer->status != USB_TRANSFER_STATUS_COMPLETED ||
                out_transfer->actual_num_bytes != (int)count)
            {
                set_usb_fault("Bulk OUT failed; job may be partial");
                free(data);
                goto cleanup;
            }
            offset += count;
            xSemaphoreTake(job_mutex, portMAX_DELAY);
            job_state.sent = offset;
            xSemaphoreGive(job_mutex);
            if (offset == job_size || offset % (64 * 1024) == 0)
                ESP_LOGI(TAG, "Job #%" PRIu32 " USB accepted %u/%u", id,
                         (unsigned)offset, (unsigned)job_size);
        }
        free(data);
        xSemaphoreTake(job_mutex, portMAX_DELAY);
        job_state.state = PRINT_DELIVERED;
        xSemaphoreGive(job_mutex);
        job_phase = LED_DELIVERED;
        status_led_set(job_phase);
        ESP_LOGI(TAG, "Job #%" PRIu32 " delivered to USB; paper completion unknown", id);
    }
cleanup:
    cleanup_connection();
    if (address < 128 && skipped_addresses[address]) status_led_set(LED_WRONG_DEVICE);
}

static void snapshot(void)
{
    usb_host_lib_info_t info = {0};
    if (usb_host_lib_info(&info) != ESP_OK) return;
    uint8_t addresses[8];
    int count = 0;
    esp_err_t err = usb_host_device_addr_list_fill(sizeof(addresses), addresses, &count);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Address list failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "USB objects=%d configured=%d clients=%d opened=%s",
             info.num_devices, count, info.num_clients, device ? "YES" : "NO");
    if (!device && !pending_address) {
        for (int i = 0; i < count; ++i) {
            if (addresses[i] >= 128 || !skipped_addresses[addresses[i]]) {
                pending_address = addresses[i];
                break;
            }
        }
    }
}

void app_main(void)
{
    job_mutex = xSemaphoreCreateMutex();
    configASSERT(job_mutex);
    memset(&job_state, 0, sizeof(job_state));
    job_state.state = PRINT_IDLE;
    status_led_init();
    status_led_set(LED_WAITING);
    ESP_LOGI(TAG, "ESP-IDF USB printer sender; IDF=%s; built %s %s",
             esp_get_idf_version(), __DATE__, __TIME__);
    ESP_LOGI(TAG, "UART0 TX=GPIO43 RX=GPIO44 115200; USB D-=GPIO19 D+=GPIO20");
    ESP_LOGI(TAG, "Upload-to-print mode; USB Printer Class Protocol 1/2 test support");
    ESP_LOGI(TAG, "The program does not switch VBUS; external safe 5V host supply is required");

    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    ESP_ERROR_CHECK(usb_host_install(&host_config));
    const usb_host_client_config_t client_config = {
        .is_synchronous = false,
        .max_num_event_msg = 8,
        .async = {.client_event_callback = client_event, .callback_arg = NULL},
    };
    ESP_ERROR_CHECK(usb_host_client_register(&client_config, &client));
    if (xTaskCreate(host_events, "usb_events", 6144, NULL, 5, NULL) != pdPASS)
        abort();

    if (xTaskCreate(web_service_start, "web_start", 8192, NULL, 4, NULL) != pdPASS)
        abort();
    ESP_LOGI(TAG, "Host ready; waiting for a USB Printer Class device");
    int64_t last_snapshot = millis_now();
    for (;;) {
        pump();
        if (pending_address && !device) {
            uint8_t address = pending_address;
            pending_address = 0;
            open_and_print(address);
        }
        if (millis_now() - last_snapshot >= 3000) {
            last_snapshot = millis_now();
            snapshot();
        }
    }
}
