#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "usb/usb_host.h"
#include "status_led.h"

#if !CONFIG_IDF_TARGET_ESP32S3
#error This application requires ESP32-S3
#endif

#define HP_VID 0x03f0
#define HP_PID 0x3654
#define PRINTER_INTERFACE 1
#define PRINTER_ALT 0
#define OUT_CHUNK_SIZE 1024

extern const uint8_t print_job_start[] asm("_binary_1_prn_start");
extern const uint8_t print_job_end[] asm("_binary_1_prn_end");

static const char *TAG = "HP2300";
static usb_host_client_handle_t client;
static usb_device_handle_t device;
static usb_transfer_t *control_transfer;
static usb_transfer_t *out_transfer;
static usb_transfer_t *in_transfer;
static volatile bool control_done;
static volatile bool out_done;
static volatile bool in_done;
static volatile bool gone;
static bool in_enabled;
static uint8_t pending_address;
static uint8_t out_ep;
static uint8_t in_ep;
static uint16_t out_mps;
static uint16_t in_mps;
static status_led_state_t job_phase = LED_OPENING;

static int64_t millis_now(void)
{
    return esp_timer_get_time() / 1000;
}

static void client_event(const usb_host_client_event_msg_t *event, void *arg)
{
    (void)arg;
    if (event->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        ESP_LOGI(TAG, "NEW_DEV address=%u", event->new_dev.address);
        if (!device) pending_address = event->new_dev.address;
    } else if (event->event == USB_HOST_CLIENT_EVENT_DEV_GONE &&
               event->dev_gone.dev_hdl == device) {
        gone = true;
        status_led_set(LED_DISCONNECTED);
        ESP_LOGE(TAG, "Printer disconnected; the job will not be replayed automatically");
    }
}

static void transfer_callback(usb_transfer_t *transfer)
{
    if (transfer == control_transfer) control_done = true;
    if (transfer == out_transfer) out_done = true;
    if (transfer == in_transfer) in_done = true;
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
            ESP_LOG_BUFFER_HEX_LEVEL(TAG, in_transfer->data_buffer, length, ESP_LOG_INFO);
        }
        in_transfer->num_bytes = in_mps;
        err = usb_host_transfer_submit(in_transfer);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Bulk IN resubmit failed: %s", esp_err_to_name(err));
            in_enabled = false;
        }
    }
}

static void stop_job(const char *reason)
{
    status_led_set(gone ? LED_DISCONNECTED : LED_ERROR);
    ESP_LOGE(TAG, "STOP: %s. No automatic retry; inspect printer, then reset the board", reason);
    in_enabled = false;
    if (device && !gone) {
        if (out_ep && usb_host_endpoint_halt(device, out_ep) == ESP_OK)
            usb_host_endpoint_flush(device, out_ep);
        if (in_ep && usb_host_endpoint_halt(device, in_ep) == ESP_OK)
            usb_host_endpoint_flush(device, in_ep);
    }
    for (;;) {
        usb_host_client_handle_events(client, pdMS_TO_TICKS(20));
        status_led_tick();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void require_ok(esp_err_t err, const char *operation)
{
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s: %s", operation, esp_err_to_name(err));
        stop_job(operation);
    }
}

static bool read_status(void)
{
    // USB Printer Class GET_PORT_STATUS, interface 1, one-byte response.
    const uint8_t setup[8] = {0xa1, 0x01, 0, 0, PRINTER_INTERFACE, 0, 1, 0};
    memcpy(control_transfer->data_buffer, setup, sizeof(setup));
    control_transfer->data_buffer[8] = 0;
    control_transfer->num_bytes = 9;
    control_done = false;
    require_ok(usb_host_transfer_submit_control(client, control_transfer),
               "GET_PORT_STATUS submit");
    int64_t started = millis_now();
    while (!control_done) {
        pump();
        if (gone) stop_job("disconnected during status request");
        if (millis_now() - started > 3000) stop_job("status request timeout");
    }
    if (control_transfer->status != USB_TRANSFER_STATUS_COMPLETED ||
        control_transfer->actual_num_bytes < 9) {
        ESP_LOGW(TAG, "Status unavailable: transfer=%d actual=%d",
                 control_transfer->status, control_transfer->actual_num_bytes);
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
    status_led_set(ready ? job_phase : LED_NOT_READY);
    return ready;
}

static void allocate_transfer(usb_transfer_t **transfer, size_t bytes, uint8_t endpoint)
{
    require_ok(usb_host_transfer_alloc(bytes, 0, transfer), "allocate transfer");
    (*transfer)->device_handle = device;
    (*transfer)->bEndpointAddress = endpoint;
    (*transfer)->callback = transfer_callback;
    (*transfer)->context = NULL;
}

static bool parse_printer_interface(const usb_config_desc_t *config)
{
    const uint8_t *bytes = (const uint8_t *)config;
    bool matching = false;
    bool found = false;
    for (size_t pos = 0; pos + 2 <= config->wTotalLength;) {
        uint8_t length = bytes[pos];
        uint8_t type = bytes[pos + 1];
        if (length < 2 || pos + length > config->wTotalLength)
            stop_job("malformed USB configuration descriptor");
        if (type == 4 && length >= 9) {
            matching = bytes[pos + 2] == PRINTER_INTERFACE &&
                       bytes[pos + 3] == PRINTER_ALT &&
                       bytes[pos + 5] == 7 && bytes[pos + 6] == 1 &&
                       bytes[pos + 7] == 2;
            if (matching) found = true;
        } else if (type == 5 && length >= 7 && matching &&
                   (bytes[pos + 3] & 3) == 2) {
            uint8_t endpoint = bytes[pos + 2];
            uint16_t mps = (bytes[pos + 4] | ((uint16_t)bytes[pos + 5] << 8)) & 0x7ff;
            if (endpoint & 0x80) {
                in_ep = endpoint;
                in_mps = mps;
            } else {
                out_ep = endpoint;
                out_mps = mps;
            }
        }
        pos += length;
    }
    return found && out_ep && in_ep && out_mps && in_mps;
}

static void open_and_print(uint8_t address)
{
    status_led_set(LED_OPENING);
    require_ok(usb_host_device_open(client, address, &device), "open USB device");

    const usb_device_desc_t *descriptor = NULL;
    require_ok(usb_host_get_device_descriptor(device, &descriptor), "read device descriptor");
    ESP_LOGI(TAG, "Device address=%u VID=%04x PID=%04x", address,
             descriptor->idVendor, descriptor->idProduct);
    if (descriptor->idVendor != HP_VID || descriptor->idProduct != HP_PID) {
        ESP_LOGW(TAG, "Ignoring non-target USB device");
        require_ok(usb_host_device_close(client, device), "close non-target device");
        device = NULL;
        status_led_set(LED_WRONG_DEVICE);
        return;
    }

    const usb_config_desc_t *config = NULL;
    require_ok(usb_host_get_active_config_descriptor(device, &config),
               "read active configuration");
    if (config->bConfigurationValue != 1 || !parse_printer_interface(config))
        stop_job("HP bidirectional printer interface was not found");
    ESP_LOGI(TAG, "Interface=1 alt=0 OUT=0x%02x MPS=%u IN=0x%02x MPS=%u",
             out_ep, out_mps, in_ep, in_mps);

    require_ok(usb_host_interface_claim(client, device, PRINTER_INTERFACE, PRINTER_ALT),
               "claim printer interface");
    allocate_transfer(&control_transfer, 64, 0);
    allocate_transfer(&out_transfer, OUT_CHUNK_SIZE, out_ep);
    allocate_transfer(&in_transfer, in_mps, in_ep);
    in_transfer->num_bytes = in_mps;
    in_enabled = true;
    require_ok(usb_host_transfer_submit(in_transfer), "start Bulk IN monitor");

    ESP_LOGI(TAG, "Waiting for paper present, selected and no error");
    while (!read_status()) {
        int64_t start = millis_now();
        while (millis_now() - start < 2000) {
            pump();
            if (gone) stop_job("disconnected before printing");
        }
    }

    size_t job_size = (size_t)(print_job_end - print_job_start);
    ESP_LOGI(TAG, "Sending embedded 1.prn: %u bytes", (unsigned)job_size);
    job_phase = LED_SENDING;
    status_led_set(job_phase);
    for (size_t offset = 0; offset < job_size;) {
        size_t count = job_size - offset;
        if (count > OUT_CHUNK_SIZE) count = OUT_CHUNK_SIZE;
        memcpy(out_transfer->data_buffer, print_job_start + offset, count);
        out_transfer->num_bytes = count;
        out_done = false;
        require_ok(usb_host_transfer_submit(out_transfer), "Bulk OUT submit");
        int64_t started = millis_now();
        while (!out_done) {
            pump();
            if (gone) stop_job("disconnected during printing; job may be partial");
            if (millis_now() - started > 60000)
                stop_job("Bulk OUT timeout; job may be partial");
        }
        if (out_transfer->status != USB_TRANSFER_STATUS_COMPLETED ||
            out_transfer->actual_num_bytes != (int)count) {
            ESP_LOGE(TAG, "Bulk OUT transfer=%d actual=%d expected=%u",
                     out_transfer->status, out_transfer->actual_num_bytes, (unsigned)count);
            stop_job("Bulk OUT failed; job may be partial");
        }
        offset += count;
        ESP_LOGI(TAG, "USB accepted %u/%u bytes", (unsigned)offset, (unsigned)job_size);
    }

    ESP_LOGI(TAG, "All bytes delivered to USB; this does not prove paper output completed");
    job_phase = LED_DELIVERED;
    status_led_set(job_phase);
    ESP_LOGI(TAG, "Monitoring status. Reset manually to print another copy");
    for (;;) {
        if (gone) stop_job("printer disconnected after transfer");
        read_status();
        int64_t start = millis_now();
        while (millis_now() - start < 2000) {
            pump();
            if (gone) break;
        }
    }
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
    if (!device && !pending_address && count > 0) pending_address = addresses[0];
}

void app_main(void)
{
    status_led_init();
    status_led_set(LED_WAITING);
    ESP_LOGI(TAG, "ESP-IDF USB printer sender; IDF=%s; built %s %s",
             esp_get_idf_version(), __DATE__, __TIME__);
    ESP_LOGI(TAG, "UART0 TX=GPIO43 RX=GPIO44 115200; USB D-=GPIO19 D+=GPIO20");
    ESP_LOGI(TAG, "Auto-print ONCE per boot; target HP 03f0:3654");
    ESP_LOGI(TAG, "The program does not switch VBUS; external safe 5V host supply is required");

    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    require_ok(usb_host_install(&host_config), "install USB Host");
    const usb_host_client_config_t client_config = {
        .is_synchronous = false,
        .max_num_event_msg = 8,
        .async = {.client_event_callback = client_event, .callback_arg = NULL},
    };
    require_ok(usb_host_client_register(&client_config, &client), "register USB client");
    if (xTaskCreate(host_events, "usb_events", 6144, NULL, 5, NULL) != pdPASS)
        stop_job("create USB Host event task");

    ESP_LOGI(TAG, "Host ready; waiting for HP DeskJet 2300");
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
