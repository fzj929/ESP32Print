#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "driver/gpio.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "lwip/ip4_addr.h"
#if ESP_IDF_VERSION_MAJOR >= 6
#include "psa/crypto.h"
#else
#include "mbedtls/md.h"
#include "mbedtls/pkcs5.h"
#include "mbedtls/sha256.h"
#endif
#include "cJSON.h"
#include "device_identity.h"
#include "printer_service.h"
#include "web_service.h"

#define MAX_PAGE_BYTES (2U * 1024U * 1024U)
#define TOKEN_SECONDS 1800
#define TOKEN_HARD_SECONDS (24 * 60 * 60)
#define MAX_AUTH_SESSIONS 4
#define PBKDF2_ROUNDS 20000
#define DEFAULT_AP_SSID "mybips.com"
#define DEFAULT_AP_PASSWORD "cCwX6goRgJ"

static const char *TAG = "WEB";
static const char *INITIAL_PASSWORD = "mybips.com";
static SemaphoreHandle_t auth_lock;
typedef struct {
    char token[65];
    char session_id[33];
    int64_t idle_deadline;
    int64_t hard_deadline;
} auth_session_t;
static auth_session_t auth_sessions[MAX_AUTH_SESSIONS];
static unsigned failed_logins;
static int64_t login_lock_until;
static bool password_changed;
static bool network_change_pending;
static char ap_ssid[33] = DEFAULT_AP_SSID;
static char ap_password[64] = DEFAULT_AP_PASSWORD;

static void delayed_reboot(void *arg);

static bool valid_ap_credentials(const char *ssid, const char *password)
{
    if (!ssid || !password) return false;
    size_t ssid_length = strlen(ssid), password_length = strlen(password);
    if (ssid_length < 1 || ssid_length > 32 ||
        password_length < 8 || password_length > 63) return false;
    for (size_t i = 0; i < ssid_length; ++i) {
        unsigned char c = (unsigned char)ssid[i];
        if (c < 0x20 || c == 0x7f) return false;
    }
    for (size_t i = 0; i < password_length; ++i) {
        unsigned char c = (unsigned char)password[i];
        if (c < 0x20 || c > 0x7e) return false;
    }
    return true;
}

static void load_ap_credentials(void)
{
    nvs_handle_t nvs;
    if (nvs_open("auth", NVS_READONLY, &nvs) != ESP_OK) return;
    char ssid[sizeof(ap_ssid)] = {0}, password[sizeof(ap_password)] = {0};
    size_t ssid_size = sizeof(ssid), password_size = sizeof(password);
    if (nvs_get_str(nvs, "ap_ssid", ssid, &ssid_size) == ESP_OK &&
        nvs_get_str(nvs, "ap_pass", password, &password_size) == ESP_OK &&
        valid_ap_credentials(ssid, password)) {
        snprintf(ap_ssid, sizeof(ap_ssid), "%s", ssid);
        snprintf(ap_password, sizeof(ap_password), "%s", password);
    }
    nvs_close(nvs);
}

static bool save_ap_credentials(const char *ssid, const char *password)
{
    nvs_handle_t nvs;
    if (nvs_open("auth", NVS_READWRITE, &nvs) != ESP_OK) return false;
    bool ok = nvs_set_str(nvs, "ap_ssid", ssid) == ESP_OK &&
              nvs_set_str(nvs, "ap_pass", password) == ESP_OK &&
              nvs_commit(nvs) == ESP_OK;
    nvs_close(nvs);
    return ok;
}

typedef enum {
    PAGE_NONE, PAGE_RECEIVING, PAGE_QUEUED, PAGE_SENDING,
    PAGE_DELIVERED, PAGE_FAILED, PAGE_UNKNOWN
} page_state_t;
typedef struct {
    char id[33];
    char owner[33];
    char job_id[65];
    char page_sha256[65];
    char serial_number[64];
    uint16_t vid, pid;
    uint8_t interface_number, alternate_setting, protocol;
    uint16_t total_pages, next_page, current_page;
    uint32_t usb_job_id;
    int64_t lease_deadline;
    page_state_t page_state;
    bool active, cancel_requested;
} print_session_t;
static SemaphoreHandle_t session_lock;
static print_session_t print_session;

static const char *page_state_name(page_state_t state)
{
    switch (state) {
    case PAGE_RECEIVING: return "receiving";
    case PAGE_QUEUED: return "queued";
    case PAGE_SENDING: return "sending_usb";
    case PAGE_DELIVERED: return "usb_delivered";
    case PAGE_FAILED: return "failed_before_usb";
    case PAGE_UNKNOWN: return "failed_unknown";
    default: return "idle";
    }
}

static void refresh_print_session_locked(void)
{
    if (!print_session.id[0]) return;
    printer_snapshot_t printer;
    printer_get_snapshot(&printer);
    if (print_session.usb_job_id && printer.job_id == print_session.usb_job_id) {
        if (printer.state == PRINT_SENDING) print_session.page_state = PAGE_SENDING;
        else if (printer.state == PRINT_DELIVERED) {
            print_session.page_state = PAGE_DELIVERED;
            print_session.usb_job_id = 0;
            print_session.next_page = print_session.current_page + 1;
            print_session.lease_deadline = esp_timer_get_time() + 120LL * 1000000;
            if (print_session.cancel_requested ||
                print_session.next_page > print_session.total_pages)
                print_session.active = false;
        } else if (printer.state == PRINT_RESULT_UNKNOWN) {
            print_session.page_state = PAGE_UNKNOWN;
        } else if (printer.state == PRINT_FAILED) {
            print_session.page_state = PAGE_FAILED;
        }
    }
    if (print_session.active && !printer.connected) {
        if (print_session.page_state == PAGE_QUEUED ||
            print_session.page_state == PAGE_SENDING)
            print_session.page_state = PAGE_UNKNOWN;
        else if (print_session.page_state == PAGE_NONE ||
                 print_session.page_state == PAGE_RECEIVING) {
            print_session.page_state = PAGE_FAILED;
            print_session.active = false;
        }
        // A page already accepted over USB stays delivered. Never infer paper
        // completion or silently replay a page after disconnect.
        return;
    }
    if (print_session.active && (printer.vid != print_session.vid ||
        printer.pid != print_session.pid ||
        printer.interface_number != print_session.interface_number ||
        printer.alternate_setting != print_session.alternate_setting ||
        printer.protocol != print_session.protocol ||
        strcmp(printer.serial_number, print_session.serial_number) != 0)) {
        print_session.page_state = PAGE_FAILED;
        print_session.active = false;
        return;
    }
    if (print_session.active && print_session.page_state != PAGE_RECEIVING &&
        print_session.page_state != PAGE_QUEUED &&
        print_session.page_state != PAGE_SENDING &&
        print_session.page_state != PAGE_UNKNOWN &&
        esp_timer_get_time() >= print_session.lease_deadline)
        print_session.active = false;
}

static cJSON *session_json_locked(void)
{
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "session_id", print_session.id);
    cJSON_AddStringToObject(data, "job_id", print_session.job_id);
    cJSON_AddStringToObject(data, "owner_session_id", print_session.owner);
    cJSON_AddBoolToObject(data, "active", print_session.active);
    cJSON_AddNumberToObject(data, "total_pages", print_session.total_pages);
    cJSON_AddNumberToObject(data, "next_page", print_session.next_page);
    cJSON_AddNumberToObject(data, "current_page", print_session.current_page);
    cJSON_AddStringToObject(data, "page_state", page_state_name(print_session.page_state));
    cJSON_AddNumberToObject(data, "usb_job_id", print_session.usb_job_id);
    int64_t remaining = (print_session.lease_deadline - esp_timer_get_time()) / 1000;
    cJSON_AddNumberToObject(data, "lease_remaining_ms", remaining > 0 ? remaining : 0);
    return data;
}

static void random_hex(char *output, size_t bytes)
{
    uint8_t random[32];
    if (bytes > sizeof(random)) bytes = sizeof(random);
    esp_fill_random(random, bytes);
    for (size_t i = 0; i < bytes; ++i)
        snprintf(output + 2 * i, 3, "%02x", random[i]);
}

static void check_recovery_gpio(void)
{
    const gpio_config_t input = {
        .pin_bit_mask = 1ULL << GPIO_NUM_4,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&input));
    if (!gpio_get_level(GPIO_NUM_4)) return;
    for (int i = 0; i < 100; ++i) {
        if (!gpio_get_level(GPIO_NUM_4)) return;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
#if CONFIG_BIPS_DEVELOPMENT_MODE
    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open("auth", NVS_READWRITE, &nvs));
    uint32_t counter = 0;
    nvs_get_u32(nvs, "reset_counter", &counter);
    esp_err_t err = nvs_erase_all(nvs);
    if (err == ESP_OK) err = nvs_set_u32(nvs, "reset_counter", counter + 1);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    ESP_ERROR_CHECK(err);
    ESP_LOGW(TAG, "DEVELOPMENT password reset by GPIO4; count=%" PRIu32, counter + 1);
    while (gpio_get_level(GPIO_NUM_4)) vTaskDelay(pdMS_TO_TICKS(20));
#else
    ESP_LOGE(TAG, "GPIO4 recovery requested but secure recovery is not provisioned; no credentials changed");
#endif
}

static void add_response_identity(cJSON *root)
{
    // Identity provisioning is not implemented yet. Never substitute a MAC for
    // the immutable manufacturing ID required by the product plan.
    cJSON_AddNullToObject(root, "device_id");
    char request_id[20];
    snprintf(request_id, sizeof(request_id), "req-%08" PRIx32, esp_random());
    cJSON_AddStringToObject(root, "request_id", request_id);
}

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");

static void json_error(httpd_req_t *req, const char *status, const char *code, const char *message)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", false);
    add_response_identity(root);
    cJSON *error = cJSON_AddObjectToObject(root, "error");
    cJSON_AddStringToObject(error, "code", code);
    cJSON_AddStringToObject(error, "message", message);
    char *body = cJSON_PrintUnformatted(root);
    httpd_resp_sendstr(req, body ? body : "{\"ok\":false}");
    free(body);
    cJSON_Delete(root);
}

static void json_data(httpd_req_t *req, const char *status, cJSON *data)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    add_response_identity(root);
    cJSON_AddItemToObject(root, "data", data);
    char *body = cJSON_PrintUnformatted(root);
    httpd_resp_sendstr(req, body ? body : "{\"ok\":false}");
    free(body);
    cJSON_Delete(root);
}

static cJSON *read_json(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 2048) return NULL;
    char *body = malloc(req->content_len + 1);
    if (!body) return NULL;
    size_t offset = 0;
    while (offset < req->content_len) {
        int got = httpd_req_recv(req, body + offset, req->content_len - offset);
        if (got <= 0) {
            free(body);
            return NULL;
        }
        offset += got;
    }
    body[offset] = 0;
    cJSON *result = cJSON_Parse(body);
    free(body);
    return result;
}

static bool derive_password(const char *password, const uint8_t salt[16], uint8_t result[32])
{
#if ESP_IDF_VERSION_MAJOR >= 6
    psa_key_derivation_operation_t op = PSA_KEY_DERIVATION_OPERATION_INIT;
    psa_status_t status = psa_crypto_init();
    if (status == PSA_SUCCESS) {
        status = psa_key_derivation_setup(&op, PSA_ALG_PBKDF2_HMAC(PSA_ALG_SHA_256));
    }
    if (status == PSA_SUCCESS) {
        status = psa_key_derivation_input_integer(&op, PSA_KEY_DERIVATION_INPUT_COST, PBKDF2_ROUNDS);
    }
    if (status == PSA_SUCCESS) {
        status = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SALT, salt, 16);
    }
    if (status == PSA_SUCCESS) {
        status = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_PASSWORD,
                                                (const uint8_t *)password, strlen(password));
    }
    if (status == PSA_SUCCESS) {
        status = psa_key_derivation_output_bytes(&op, result, 32);
    }
    psa_key_derivation_abort(&op);
    return status == PSA_SUCCESS;
#else
    return mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256,
        (const unsigned char *)password, strlen(password), salt, 16,
        PBKDF2_ROUNDS, 32, result) == 0;
#endif
}

static bool verify_password(const char *password)
{
    if (!password_changed) return strcmp(password, INITIAL_PASSWORD) == 0;
    nvs_handle_t nvs;
    if (nvs_open("auth", NVS_READONLY, &nvs) != ESP_OK) return false;
    uint8_t salt[16] = {0}, expected[32] = {0}, actual[32] = {0};
    size_t salt_len = sizeof(salt), hash_len = sizeof(expected);
    bool valid = nvs_get_blob(nvs, "salt", salt, &salt_len) == ESP_OK &&
                 nvs_get_blob(nvs, "hash", expected, &hash_len) == ESP_OK &&
                 salt_len == 16 && hash_len == 32 && derive_password(password, salt, actual);
    nvs_close(nvs);
    unsigned difference = 0;
    for (size_t i = 0; i < 32; ++i) difference |= (unsigned)(actual[i] ^ expected[i]);
    return valid && difference == 0;
}

static bool authorized_session(httpd_req_t *req, bool require_changed, char *session_id)
{
    char header[80];
    if (httpd_req_get_hdr_value_str(req, "Authorization", header, sizeof(header)) != ESP_OK ||
        strncmp(header, "Bearer ", 7) != 0) {
        json_error(req, "401 Unauthorized", "AUTH_REQUIRED", "请先登录");
        return false;
    }
    xSemaphoreTake(auth_lock, portMAX_DELAY);
    bool valid = false;
    int64_t now = esp_timer_get_time();
    if (strlen(header + 7) == 64) {
        for (size_t i = 0; i < MAX_AUTH_SESSIONS; ++i) {
            auth_session_t *entry = &auth_sessions[i];
            if (entry->token[0] && strcmp(header + 7, entry->token) == 0 &&
                now < entry->idle_deadline && now < entry->hard_deadline) {
                entry->idle_deadline = now + (int64_t)TOKEN_SECONDS * 1000000;
                if (session_id) snprintf(session_id, 33, "%s", entry->session_id);
                valid = true;
                break;
            }
        }
    }
    bool changed = password_changed;
    xSemaphoreGive(auth_lock);
    if (!valid) json_error(req, "401 Unauthorized", "AUTH_REQUIRED", "登录已失效，请重新登录");
    else if (require_changed && !changed)
        json_error(req, "403 Forbidden", "PASSWORD_CHANGE_REQUIRED", "请先修改初始密码");
    return valid && (!require_changed || changed);
}

static bool authorized(httpd_req_t *req, bool require_changed)
{
    return authorized_session(req, require_changed, NULL);
}

static esp_err_t page_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)index_html_start,
                           (ssize_t)(index_html_end - index_html_start));
}

static esp_err_t login_handler(httpd_req_t *req)
{
    cJSON *body = read_json(req);
    const cJSON *password = body ? cJSON_GetObjectItemCaseSensitive(body, "password") : NULL;
    int64_t now = esp_timer_get_time();
    if (now < login_lock_until) {
        json_error(req, "429 Too Many Requests", "RATE_LIMITED", "登录尝试过多，请稍后再试");
        cJSON_Delete(body);
        return ESP_OK;
    }
    if (!cJSON_IsString(password) || strlen(password->valuestring) > 64 ||
        !verify_password(password->valuestring)) {
        if (++failed_logins >= 5) {
            login_lock_until = now + 30LL * 1000000;
            failed_logins = 0;
        }
        ESP_LOGW(TAG, "Login failed");
        json_error(req, "401 Unauthorized", "BAD_PASSWORD", "密码错误");
    } else {
        failed_logins = 0;
        xSemaphoreTake(auth_lock, portMAX_DELAY);
        auth_session_t *entry = NULL;
        for (size_t i = 0; i < MAX_AUTH_SESSIONS; ++i) {
            if (!auth_sessions[i].token[0] || now >= auth_sessions[i].idle_deadline ||
                now >= auth_sessions[i].hard_deadline) {
                entry = &auth_sessions[i];
                break;
            }
        }
        cJSON *data = NULL;
        if (entry) {
            random_hex(entry->token, 32);
            random_hex(entry->session_id, 16);
            entry->idle_deadline = now + (int64_t)TOKEN_SECONDS * 1000000;
            entry->hard_deadline = now + (int64_t)TOKEN_HARD_SECONDS * 1000000;
            data = cJSON_CreateObject();
            cJSON_AddStringToObject(data, "access_token", entry->token);
            cJSON_AddStringToObject(data, "auth_session_id", entry->session_id);
            cJSON_AddNumberToObject(data, "expires_in", TOKEN_SECONDS);
            cJSON_AddBoolToObject(data, "must_change_password", !password_changed);
        }
        xSemaphoreGive(auth_lock);
        if (data) {
            ESP_LOGI(TAG, "Administrator logged in; must_change_password=%d", !password_changed);
            json_data(req, "200 OK", data);
        } else {
            json_error(req, "503 Service Unavailable", "SESSION_LIMIT", "登录会话已满");
        }
    }
    cJSON_Delete(body);
    return ESP_OK;
}

static esp_err_t password_handler(httpd_req_t *req)
{
    if (!authorized(req, false)) return ESP_OK;
    cJSON *body = read_json(req);
    const cJSON *old = body ? cJSON_GetObjectItemCaseSensitive(body, "old_password") : NULL;
    const cJSON *new_password = body ? cJSON_GetObjectItemCaseSensitive(body, "new_password") : NULL;
    const cJSON *ssid = body ? cJSON_GetObjectItemCaseSensitive(body, "wifi_ssid") : NULL;
    const cJSON *wifi_password = body ? cJSON_GetObjectItemCaseSensitive(body, "wifi_password") : NULL;
    bool initial_setup = !password_changed;
    if (!cJSON_IsString(old) || !cJSON_IsString(new_password) ||
        strlen(new_password->valuestring) < 8 || strlen(new_password->valuestring) > 64 ||
        !verify_password(old->valuestring)) {
        json_error(req, "400 Bad Request", "INVALID_PASSWORD", "当前密码错误或新密码不符合要求");
        cJSON_Delete(body);
        return ESP_OK;
    }
    if (initial_setup && (!cJSON_IsString(ssid) || !cJSON_IsString(wifi_password) ||
        !valid_ap_credentials(ssid->valuestring, wifi_password->valuestring))) {
        json_error(req, "400 Bad Request", "INVALID_WIFI_CONFIG",
                   "热点名称须为 1～32 字节，Wi-Fi 密码须为 8～63 位可打印 ASCII 字符");
        cJSON_Delete(body);
        return ESP_OK;
    }
    uint8_t salt[16], hash[32];
    esp_fill_random(salt, sizeof(salt));
    nvs_handle_t nvs;
    bool ok = derive_password(new_password->valuestring, salt, hash) &&
              nvs_open("auth", NVS_READWRITE, &nvs) == ESP_OK;
    if (ok) {
        ok = nvs_set_blob(nvs, "salt", salt, sizeof(salt)) == ESP_OK &&
             nvs_set_blob(nvs, "hash", hash, sizeof(hash)) == ESP_OK &&
             nvs_set_u8(nvs, "changed", 1) == ESP_OK &&
             (!initial_setup || (nvs_set_str(nvs, "ap_ssid", ssid->valuestring) == ESP_OK &&
                                 nvs_set_str(nvs, "ap_pass", wifi_password->valuestring) == ESP_OK)) &&
             nvs_commit(nvs) == ESP_OK;
        nvs_close(nvs);
    }
    if (ok) {
        if (initial_setup) {
            snprintf(ap_ssid, sizeof(ap_ssid), "%s", ssid->valuestring);
            snprintf(ap_password, sizeof(ap_password), "%s", wifi_password->valuestring);
        }
        xSemaphoreTake(auth_lock, portMAX_DELAY);
        password_changed = true;
        memset(auth_sessions, 0, sizeof(auth_sessions));
        xSemaphoreGive(auth_lock);
        ESP_LOGI(TAG, "Administrator password changed; token revoked");
        cJSON *data = cJSON_CreateObject();
        cJSON_AddStringToObject(data, "wifi_ssid", ap_ssid);
        cJSON_AddBoolToObject(data, "reconnect_required", initial_setup);
        bool scheduled = initial_setup &&
            xTaskCreate(delayed_reboot, "wifi_restart", 2048, NULL, 3, NULL) == pdPASS;
        if (scheduled) {
            xSemaphoreTake(session_lock, portMAX_DELAY);
            network_change_pending = true;
            xSemaphoreGive(session_lock);
        }
        cJSON_AddBoolToObject(data, "reboot_scheduled", scheduled);
        json_data(req, "200 OK", data);
    } else {
        ESP_LOGE(TAG, "Password storage failed");
        json_error(req, "500 Internal Server Error", "STORAGE_ERROR", "密码保存失败");
    }
    cJSON_Delete(body);
    return ESP_OK;
}

static esp_err_t logout_handler(httpd_req_t *req)
{
    if (!authorized(req, false)) return ESP_OK;
    char header[80];
    if (httpd_req_get_hdr_value_str(req, "Authorization", header, sizeof(header)) == ESP_OK) {
        xSemaphoreTake(auth_lock, portMAX_DELAY);
        for (size_t i = 0; i < MAX_AUTH_SESSIONS; ++i) {
            if (auth_sessions[i].token[0] && strcmp(header + 7, auth_sessions[i].token) == 0) {
                memset(&auth_sessions[i], 0, sizeof(auth_sessions[i]));
                break;
            }
        }
        xSemaphoreGive(auth_lock);
    }
    json_data(req, "200 OK", cJSON_CreateObject());
    return ESP_OK;
}

static esp_err_t refresh_handler(httpd_req_t *req)
{
    if (!authorized(req, false)) return ESP_OK;
    char header[80];
    httpd_req_get_hdr_value_str(req, "Authorization", header, sizeof(header));
    cJSON *data = cJSON_CreateObject();
    xSemaphoreTake(auth_lock, portMAX_DELAY);
    for (size_t i = 0; i < MAX_AUTH_SESSIONS; ++i) {
        auth_session_t *entry = &auth_sessions[i];
        if (entry->token[0] && strcmp(header + 7, entry->token) == 0) {
            random_hex(entry->token, 32);
            cJSON_AddStringToObject(data, "access_token", entry->token);
            cJSON_AddStringToObject(data, "auth_session_id", entry->session_id);
            cJSON_AddNumberToObject(data, "expires_in", TOKEN_SECONDS);
            break;
        }
    }
    xSemaphoreGive(auth_lock);
    json_data(req, "200 OK", data);
    return ESP_OK;
}

static esp_err_t status_handler(httpd_req_t *req)
{
    if (!authorized(req, true)) return ESP_OK;
    printer_snapshot_t state;
    printer_get_snapshot(&state);
    cJSON *data = cJSON_CreateObject();
    cJSON_AddBoolToObject(data, "connected", state.connected);
    cJSON_AddBoolToObject(data, "ready", state.ready);
    cJSON_AddBoolToObject(data, "status_known", state.status_known);
    cJSON_AddStringToObject(data, "readiness", !state.connected || !state.status_known
                            ? "unknown" : state.ready ? "ready" : "not_ready");
    if (state.status_known) {
        cJSON_AddBoolToObject(data, "paper_empty", (state.port_status & 0x20) != 0);
        cJSON_AddBoolToObject(data, "selected", (state.port_status & 0x10) != 0);
        cJSON_AddBoolToObject(data, "reported_error", (state.port_status & 0x08) == 0);
    } else {
        cJSON_AddNullToObject(data, "paper_empty");
        cJSON_AddNullToObject(data, "selected");
        cJSON_AddNullToObject(data, "reported_error");
    }
    cJSON_AddStringToObject(data, "mechanical_completion", "unknown");
    cJSON_AddStringToObject(data, "physical_job_state",
                            state.connected ? "unknown" : "disconnected");
    cJSON_AddStringToObject(data, "status_source", "usb_printer_class_port_status");
    cJSON_AddNumberToObject(data, "vid", state.vid);
    cJSON_AddNumberToObject(data, "pid", state.pid);
    cJSON_AddNumberToObject(data, "interface_number", state.interface_number);
    cJSON_AddNumberToObject(data, "alternate_setting", state.alternate_setting);
    cJSON_AddNumberToObject(data, "protocol", state.protocol);
    char raw[3]; snprintf(raw, sizeof(raw), "%02x", state.port_status);
    cJSON_AddStringToObject(data, "raw_port_status", raw);
    cJSON_AddStringToObject(data, "state", printer_state_name(state.state));
    cJSON_AddNumberToObject(data, "job_id", state.job_id);
    cJSON_AddNumberToObject(data, "total", state.total);
    cJSON_AddNumberToObject(data, "sent", state.sent);
    cJSON_AddStringToObject(data, "error", state.error);
    xSemaphoreTake(session_lock, portMAX_DELAY);
    refresh_print_session_locked();
    cJSON_AddStringToObject(data, "session_state", print_session.active ?
                            (print_session.page_state == PAGE_RECEIVING ? "receiving" :
                             print_session.page_state == PAGE_QUEUED ||
                             print_session.page_state == PAGE_SENDING ? "printing" :
                             print_session.page_state == PAGE_UNKNOWN ? "failed_unknown" :
                             "waiting_next_page") : "idle");
    cJSON_AddStringToObject(data, "page_state", page_state_name(print_session.page_state));
    cJSON_AddNumberToObject(data, "next_page", print_session.next_page);
    xSemaphoreGive(session_lock);
    json_data(req, "200 OK", data);
    return ESP_OK;
}

static void ieee_field(const char *raw, const char *key, char *output, size_t capacity)
{
    if (!raw || !capacity) return;
    size_t key_length = strlen(key);
    for (const char *entry = raw; *entry;) {
        const char *end = strchr(entry, ';');
        if (!end) end = entry + strlen(entry);
        const char *colon = memchr(entry, ':', end - entry);
        if (colon && (size_t)(colon - entry) == key_length &&
            strncasecmp(entry, key, key_length) == 0) {
            const char *value = colon + 1;
            while (value < end && *value == ' ') ++value;
            size_t length = end - value;
            while (length && value[length - 1] == ' ') --length;
            if (length >= capacity) length = capacity - 1;
            memcpy(output, value, length);
            output[length] = 0;
            return;
        }
        if (!*end) break;
        entry = end + 1;
    }
}

static esp_err_t info_handler(httpd_req_t *req)
{
    if (!authorized(req, false)) return ESP_OK;
    printer_snapshot_t state;
    printer_get_snapshot(&state);
    cJSON *data = cJSON_CreateObject();
    cJSON_AddBoolToObject(data, "connected", state.connected);
    if (state.connected) {
        cJSON *identity = cJSON_AddObjectToObject(data, "identity");
        char vid[5], pid[5], out[5], in[5];
        snprintf(vid, sizeof(vid), "%04x", state.vid);
        snprintf(pid, sizeof(pid), "%04x", state.pid);
        snprintf(out, sizeof(out), "0x%02x", state.bulk_out_endpoint);
        snprintf(in, sizeof(in), "0x%02x", state.bulk_in_endpoint);
        cJSON_AddStringToObject(identity, "vendor_id", vid);
        cJSON_AddStringToObject(identity, "product_id", pid);
        if (state.manufacturer[0]) cJSON_AddStringToObject(identity, "manufacturer", state.manufacturer);
        else cJSON_AddNullToObject(identity, "manufacturer");
        if (state.product[0]) cJSON_AddStringToObject(identity, "product", state.product);
        else cJSON_AddNullToObject(identity, "product");
        if (state.serial_number[0]) cJSON_AddStringToObject(identity, "serial_number", state.serial_number);
        else cJSON_AddNullToObject(identity, "serial_number");
        cJSON *usb = cJSON_AddObjectToObject(data, "usb");
        cJSON_AddNumberToObject(usb, "interface_number", state.interface_number);
        cJSON_AddNumberToObject(usb, "alternate_setting", state.alternate_setting);
        cJSON_AddNumberToObject(usb, "interface_class", 7);
        cJSON_AddNumberToObject(usb, "interface_subclass", 1);
        cJSON_AddNumberToObject(usb, "protocol", state.protocol);
        cJSON_AddStringToObject(usb, "bulk_out_endpoint", out);
        if (state.bulk_in_endpoint) cJSON_AddStringToObject(usb, "bulk_in_endpoint", in);
        else cJSON_AddNullToObject(usb, "bulk_in_endpoint");
        cJSON_AddNumberToObject(usb, "max_packet_size", state.max_packet_size);
    }
    cJSON *ieee = cJSON_AddObjectToObject(data, "ieee1284");
    cJSON_AddBoolToObject(ieee, "available", state.ieee1284_device_id[0] != 0);
    if (state.ieee1284_device_id[0]) {
        cJSON_AddStringToObject(ieee, "raw_device_id", state.ieee1284_device_id);
        char mfg[64] = {0}, model[64] = {0}, commands[128] = {0};
        ieee_field(state.ieee1284_device_id, "MFG", mfg, sizeof(mfg));
        if (!mfg[0]) ieee_field(state.ieee1284_device_id, "MANUFACTURER", mfg, sizeof(mfg));
        ieee_field(state.ieee1284_device_id, "MDL", model, sizeof(model));
        if (!model[0]) ieee_field(state.ieee1284_device_id, "MODEL", model, sizeof(model));
        ieee_field(state.ieee1284_device_id, "CMD", commands, sizeof(commands));
        cJSON_AddStringToObject(ieee, "manufacturer", mfg);
        cJSON_AddStringToObject(ieee, "model", model);
        cJSON *sets = cJSON_AddArrayToObject(ieee, "command_sets");
        char *save = NULL;
        for (char *part = strtok_r(commands, ",", &save); part;
             part = strtok_r(NULL, ",", &save))
            cJSON_AddItemToArray(sets, cJSON_CreateString(part));
    }
    cJSON *profile = cJSON_AddObjectToObject(data, "profile");
    cJSON_AddStringToObject(profile, "capability_level",
                            state.connected ? "transport_compatible" : "discovered");
    cJSON_AddNullToObject(profile, "profile_id");
    json_data(req, "200 OK", data);
    return ESP_OK;
}

static esp_err_t device_status_handler(httpd_req_t *req)
{
    if (!authorized(req, false)) return ESP_OK;
    printer_snapshot_t printer;
    printer_get_snapshot(&printer);
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    wifi_sta_list_t stations = {0};
    esp_err_t wifi_err = esp_wifi_ap_get_sta_list(&stations);
    cJSON *data = cJSON_CreateObject();
    cJSON_AddBoolToObject(data, "provisioned", false);
    cJSON_AddBoolToObject(data, "identity_efuse_valid", device_identity_efuse_valid());
    cJSON_AddBoolToObject(data, "identity_write_protected", device_identity_write_protected());
    cJSON_AddBoolToObject(data, "development_mode", CONFIG_BIPS_DEVELOPMENT_MODE);
    cJSON_AddNumberToObject(data, "chip_revision", chip.revision);
    cJSON_AddNumberToObject(data, "cpu_cores", chip.cores);
    cJSON_AddNumberToObject(data, "uptime_ms", esp_timer_get_time() / 1000);
    cJSON_AddNumberToObject(data, "reset_reason", esp_reset_reason());
    cJSON_AddStringToObject(data, "softap_ip", "192.168.188.1");
    cJSON_AddStringToObject(data, "wifi_ssid", ap_ssid);
    cJSON_AddNumberToObject(data, "wifi_clients", wifi_err == ESP_OK ? stations.num : 0);
    cJSON_AddNumberToObject(data, "internal_free", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(data, "internal_largest", heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(data, "psram_free", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(data, "psram_largest", heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    cJSON_AddBoolToObject(data, "printer_connected", printer.connected);
    cJSON_AddStringToObject(data, "print_state", printer_state_name(printer.state));
    json_data(req, "200 OK", data);
    return ESP_OK;
}

static void delayed_reboot(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(2000));
    ESP_LOGW(TAG, "Configuration/reboot request: restarting board");
    esp_restart();
}

static esp_err_t wifi_config_handler(httpd_req_t *req)
{
    if (!authorized(req, true)) return ESP_OK;
    if (req->method == HTTP_GET) {
        cJSON *data = cJSON_CreateObject();
        cJSON_AddStringToObject(data, "wifi_ssid", ap_ssid);
        cJSON_AddStringToObject(data, "softap_ip", "192.168.188.1");
        cJSON_AddBoolToObject(data, "reboot_pending", network_change_pending);
        json_data(req, "200 OK", data);
        return ESP_OK;
    }
    cJSON *body = read_json(req);
    const cJSON *ssid = body ? cJSON_GetObjectItemCaseSensitive(body, "wifi_ssid") : NULL;
    const cJSON *password = body ? cJSON_GetObjectItemCaseSensitive(body, "wifi_password") : NULL;
    if (!cJSON_IsString(ssid) || !cJSON_IsString(password) ||
        !valid_ap_credentials(ssid->valuestring, password->valuestring)) {
        json_error(req, "400 Bad Request", "INVALID_WIFI_CONFIG",
                   "热点名称须为 1～32 字节，Wi-Fi 密码须为 8～63 位可打印 ASCII 字符");
        cJSON_Delete(body);
        return ESP_OK;
    }
    printer_snapshot_t printer;
    printer_get_snapshot(&printer);
    xSemaphoreTake(session_lock, portMAX_DELAY);
    refresh_print_session_locked();
    bool busy = network_change_pending || print_session.active ||
                printer.state == PRINT_QUEUED || printer.state == PRINT_SENDING ||
                printer.state == PRINT_RESULT_UNKNOWN;
    if (!busy) network_change_pending = true;
    xSemaphoreGive(session_lock);
    if (busy) {
        json_error(req, "409 Conflict", "DEVICE_BUSY", "打印会话进行中，暂不能修改热点");
        cJSON_Delete(body);
        return ESP_OK;
    }
    if (!save_ap_credentials(ssid->valuestring, password->valuestring)) {
        xSemaphoreTake(session_lock, portMAX_DELAY);
        network_change_pending = false;
        xSemaphoreGive(session_lock);
        json_error(req, "500 Internal Server Error", "STORAGE_ERROR", "热点配置保存失败");
        cJSON_Delete(body);
        return ESP_OK;
    }
    snprintf(ap_ssid, sizeof(ap_ssid), "%s", ssid->valuestring);
    snprintf(ap_password, sizeof(ap_password), "%s", password->valuestring);
    bool scheduled = xTaskCreate(delayed_reboot, "wifi_restart", 2048, NULL, 3, NULL) == pdPASS;
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "wifi_ssid", ap_ssid);
    cJSON_AddBoolToObject(data, "reconnect_required", true);
    cJSON_AddBoolToObject(data, "reboot_scheduled", scheduled);
    json_data(req, "202 Accepted", data);
    if (!scheduled) {
        xSemaphoreTake(session_lock, portMAX_DELAY);
        network_change_pending = false;
        xSemaphoreGive(session_lock);
    }
    ESP_LOGI(TAG, "Wi-Fi configuration saved; SSID=%s, reboot scheduled=%d", ap_ssid, scheduled);
    cJSON_Delete(body);
    return ESP_OK;
}

static esp_err_t reboot_handler(httpd_req_t *req)
{
    if (!authorized(req, true)) return ESP_OK;
    printer_snapshot_t state;
    printer_get_snapshot(&state);
    if (state.state == PRINT_QUEUED || state.state == PRINT_SENDING ||
        state.state == PRINT_RESULT_UNKNOWN) {
        json_error(req, "409 Conflict", "DEVICE_BUSY", "打印进行中或结果不确定，拒绝重启");
        return ESP_OK;
    }
    if (xTaskCreate(delayed_reboot, "reboot", 2048, NULL, 3, NULL) != pdPASS) {
        json_error(req, "503 Service Unavailable", "NO_MEMORY", "无法安排重启");
        return ESP_OK;
    }
    json_data(req, "202 Accepted", cJSON_CreateObject());
    return ESP_OK;
}

static bool valid_job_id(const char *value)
{
    size_t length = value ? strlen(value) : 0;
    if (length < 1 || length > 64) return false;
    for (size_t i = 0; i < length; ++i) {
        char c = value[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
              (c >= 'A' && c <= 'Z') || c == '-' || c == '_')) return false;
    }
    return true;
}

static bool valid_sha256(const char *value)
{
    if (!value || strlen(value) != 64) return false;
    for (size_t i = 0; i < 64; ++i)
        if (!((value[i] >= '0' && value[i] <= '9') ||
              (value[i] >= 'a' && value[i] <= 'f'))) return false;
    return true;
}

static esp_err_t session_create_handler(httpd_req_t *req)
{
    char owner[33] = {0};
    if (!authorized_session(req, true, owner)) return ESP_OK;
#if !CONFIG_BIPS_DEVELOPMENT_MODE
    json_error(req, "503 Service Unavailable", "DEVICE_UNPROVISIONED",
               "量产身份与证书绑定未完成，打印已锁定");
    return ESP_OK;
#endif
    cJSON *body = read_json(req);
    const cJSON *job = body ? cJSON_GetObjectItemCaseSensitive(body, "job_id") : NULL;
    const cJSON *pages = body ? cJSON_GetObjectItemCaseSensitive(body, "total_pages") : NULL;
    if (!cJSON_IsString(job) || !valid_job_id(job->valuestring) ||
        !cJSON_IsNumber(pages) || pages->valuedouble != pages->valueint ||
        pages->valueint < 1 || pages->valueint > 999) {
        json_error(req, "400 Bad Request", "INVALID_SESSION", "作业号或总页数无效");
        cJSON_Delete(body);
        return ESP_OK;
    }
    printer_snapshot_t printer;
    printer_get_snapshot(&printer);
    if (!printer.connected || !printer.ready || printer.state == PRINT_RESULT_UNKNOWN) {
        json_error(req, "409 Conflict", "PRINTER_NOT_READY", "打印机未就绪");
        cJSON_Delete(body);
        return ESP_OK;
    }
    const cJSON *fingerprint = cJSON_GetObjectItemCaseSensitive(body, "printer_fingerprint");
    if (cJSON_IsObject(fingerprint)) {
        const cJSON *vid = cJSON_GetObjectItemCaseSensitive(fingerprint, "vendor_id");
        const cJSON *pid = cJSON_GetObjectItemCaseSensitive(fingerprint, "product_id");
        const cJSON *serial = cJSON_GetObjectItemCaseSensitive(fingerprint, "serial_number");
        char actual_vid[5], actual_pid[5];
        snprintf(actual_vid, sizeof(actual_vid), "%04x", printer.vid);
        snprintf(actual_pid, sizeof(actual_pid), "%04x", printer.pid);
        if (!cJSON_IsString(vid) || !cJSON_IsString(pid) ||
            strcasecmp(vid->valuestring, actual_vid) != 0 ||
            strcasecmp(pid->valuestring, actual_pid) != 0 ||
            (cJSON_IsString(serial) &&
             strcmp(serial->valuestring, printer.serial_number) != 0)) {
            json_error(req, "409 Conflict", "PRINTER_CHANGED", "当前打印机与作业指纹不一致");
            cJSON_Delete(body);
            return ESP_OK;
        }
    }
    const cJSON *profile = cJSON_GetObjectItemCaseSensitive(body, "printer_profile");
    if (profile) {
        char prefix[15];
        snprintf(prefix, sizeof(prefix), "usb:%04x:%04x", printer.vid, printer.pid);
        size_t prefix_length = strlen(prefix);
        if (!cJSON_IsString(profile) ||
            strncasecmp(profile->valuestring, prefix, prefix_length) != 0 ||
            (profile->valuestring[prefix_length] != 0 &&
             profile->valuestring[prefix_length] != ':')) {
            json_error(req, "409 Conflict", "PRINTER_PROFILE_MISMATCH", "打印机 profile 不匹配");
            cJSON_Delete(body);
            return ESP_OK;
        }
    }
    xSemaphoreTake(session_lock, portMAX_DELAY);
    refresh_print_session_locked();
    if (network_change_pending) {
        xSemaphoreGive(session_lock);
        json_error(req, "409 Conflict", "WIFI_RESTART_PENDING", "热点即将重启，请稍后重新连接");
        cJSON_Delete(body);
        return ESP_OK;
    }
    if (print_session.active) {
        xSemaphoreGive(session_lock);
        json_error(req, "409 Conflict", "PRINTER_RESERVED", "已有打印会话占用打印机");
        cJSON_Delete(body);
        return ESP_OK;
    }
    memset(&print_session, 0, sizeof(print_session));
    random_hex(print_session.id, 16);
    snprintf(print_session.owner, sizeof(print_session.owner), "%s", owner);
    snprintf(print_session.job_id, sizeof(print_session.job_id), "%s", job->valuestring);
    print_session.vid = printer.vid;
    print_session.pid = printer.pid;
    print_session.interface_number = printer.interface_number;
    print_session.alternate_setting = printer.alternate_setting;
    print_session.protocol = printer.protocol;
    snprintf(print_session.serial_number, sizeof(print_session.serial_number),
             "%s", printer.serial_number);
    print_session.total_pages = pages->valueint;
    print_session.next_page = 1;
    print_session.active = true;
    print_session.lease_deadline = esp_timer_get_time() + 120LL * 1000000;
    cJSON *data = session_json_locked();
    xSemaphoreGive(session_lock);
    ESP_LOGI(TAG, "Print session created: job=%s pages=%d", job->valuestring, pages->valueint);
    cJSON_Delete(body);
    json_data(req, "201 Created", data);
    return ESP_OK;
}

static bool parse_session_uri(const char *uri, char id[33], const char **suffix)
{
    const char *prefix = "/api/v1/print-sessions/";
    size_t base = strlen(prefix);
    if (strncmp(uri, prefix, base) != 0 || strlen(uri + base) < 32) return false;
    memcpy(id, uri + base, 32);
    id[32] = 0;
    for (int i = 0; i < 32; ++i)
        if (!((id[i] >= '0' && id[i] <= '9') ||
              (id[i] >= 'a' && id[i] <= 'f'))) return false;
    *suffix = uri + base + 32;
    return true;
}

static esp_err_t session_get_handler(httpd_req_t *req)
{
    char owner[33] = {0}, id[33];
    if (!authorized_session(req, true, owner)) return ESP_OK;
    const char *suffix;
    if (!parse_session_uri(req->uri, id, &suffix) || *suffix) {
        json_error(req, "404 Not Found", "SESSION_NOT_FOUND", "打印会话不存在");
        return ESP_OK;
    }
    xSemaphoreTake(session_lock, portMAX_DELAY);
    refresh_print_session_locked();
    bool found = strcmp(print_session.id, id) == 0;
    cJSON *data = found ? session_json_locked() : NULL;
    xSemaphoreGive(session_lock);
    if (found) json_data(req, "200 OK", data);
    else json_error(req, "404 Not Found", "SESSION_NOT_FOUND", "打印会话不存在");
    return ESP_OK;
}

static esp_err_t session_delete_handler(httpd_req_t *req)
{
    char owner[33] = {0}, id[33];
    if (!authorized_session(req, true, owner)) return ESP_OK;
    const char *suffix;
    if (!parse_session_uri(req->uri, id, &suffix) || *suffix) {
        json_error(req, "404 Not Found", "SESSION_NOT_FOUND", "打印会话不存在");
        return ESP_OK;
    }
    xSemaphoreTake(session_lock, portMAX_DELAY);
    refresh_print_session_locked();
    bool found = print_session.active && strcmp(print_session.id, id) == 0;
    bool own = found && strcmp(print_session.owner, owner) == 0;
    if (own) {
        if (print_session.page_state == PAGE_RECEIVING ||
            print_session.page_state == PAGE_QUEUED ||
            print_session.page_state == PAGE_SENDING)
            print_session.cancel_requested = true;
        else if (print_session.page_state != PAGE_UNKNOWN)
            print_session.active = false;
    }
    cJSON *data = own ? session_json_locked() : NULL;
    xSemaphoreGive(session_lock);
    if (!found) json_error(req, "404 Not Found", "SESSION_NOT_FOUND", "打印会话不存在");
    else if (!own) json_error(req, "403 Forbidden", "SESSION_OWNER_MISMATCH", "不是会话所有者");
    else json_data(req, "200 OK", data);
    return ESP_OK;
}

static esp_err_t page_upload_handler(httpd_req_t *req);

static esp_err_t session_post_handler(httpd_req_t *req)
{
    char owner[33] = {0}, id[33];
    if (!authorized_session(req, true, owner)) return ESP_OK;
    const char *suffix;
    if (!parse_session_uri(req->uri, id, &suffix)) {
        json_error(req, "404 Not Found", "SESSION_NOT_FOUND", "不支持的会话操作");
        return ESP_OK;
    }
    if (strncmp(suffix, "/pages/", 7) == 0) return page_upload_handler(req);
    if (strcmp(suffix, "/resolve-unknown") == 0) {
        cJSON *body = read_json(req);
        const cJSON *action = body ? cJSON_GetObjectItemCaseSensitive(body, "action") : NULL;
        bool confirmed = cJSON_IsString(action) &&
                         strcmp(action->valuestring, "release_without_retry") == 0;
        cJSON_Delete(body);
        if (!confirmed) {
            json_error(req, "400 Bad Request", "INVALID_ACTION", "必须显式确认不重试");
            return ESP_OK;
        }
        xSemaphoreTake(session_lock, portMAX_DELAY);
        refresh_print_session_locked();
        bool unknown = print_session.active && strcmp(print_session.id, id) == 0 &&
                       print_session.page_state == PAGE_UNKNOWN;
        xSemaphoreGive(session_lock);
        if (!unknown) {
            json_error(req, "409 Conflict", "PAGE_NOT_READY", "会话不是结果不确定状态");
            return ESP_OK;
        }
        if (xTaskCreate(delayed_reboot, "resolve_reboot", 2048, NULL, 3, NULL) != pdPASS) {
            json_error(req, "503 Service Unavailable", "INSUFFICIENT_MEMORY", "无法执行恢复重启");
            return ESP_OK;
        }
        ESP_LOGW(TAG, "Unknown print result manually released without retry; rebooting USB host");
        json_data(req, "202 Accepted", cJSON_CreateObject());
        return ESP_OK;
    }
    if (strcmp(suffix, "/heartbeat") != 0) {
        json_error(req, "404 Not Found", "SESSION_NOT_FOUND", "不支持的会话操作");
        return ESP_OK;
    }
    xSemaphoreTake(session_lock, portMAX_DELAY);
    refresh_print_session_locked();
    bool found = print_session.active && strcmp(print_session.id, id) == 0;
    bool own = found && strcmp(print_session.owner, owner) == 0;
    if (own && print_session.page_state != PAGE_UNKNOWN)
        print_session.lease_deadline = esp_timer_get_time() + 120LL * 1000000;
    cJSON *data = own ? session_json_locked() : NULL;
    xSemaphoreGive(session_lock);
    if (!found) json_error(req, "404 Not Found", "SESSION_NOT_FOUND", "打印会话不存在");
    else if (!own) json_error(req, "403 Forbidden", "SESSION_OWNER_MISMATCH", "不是会话所有者");
    else json_data(req, "200 OK", data);
    return ESP_OK;
}

static void abort_receiving_page(const char *id, int page_number)
{
    xSemaphoreTake(session_lock, portMAX_DELAY);
    if (strcmp(print_session.id, id) == 0 &&
        print_session.current_page == page_number &&
        print_session.page_state == PAGE_RECEIVING) {
        print_session.current_page = 0;
        print_session.page_state = PAGE_NONE;
        print_session.page_sha256[0] = 0;
        if (print_session.cancel_requested) print_session.active = false;
    }
    xSemaphoreGive(session_lock);
}

static esp_err_t page_receive(httpd_req_t *req)
{
    char owner[33] = {0}, id[33];
    if (!authorized_session(req, true, owner)) return ESP_OK;
#if !CONFIG_BIPS_DEVELOPMENT_MODE
    json_error(req, "503 Service Unavailable", "DEVICE_UNPROVISIONED",
               "量产安全配置未完成，打印已锁定");
    return ESP_OK;
#endif
    const char *suffix;
    if (!parse_session_uri(req->uri, id, &suffix) ||
        strncmp(suffix, "/pages/", 7) != 0) {
        json_error(req, "404 Not Found", "SESSION_NOT_FOUND", "打印会话不存在");
        return ESP_OK;
    }
    char *end = NULL;
    long page_number = strtol(suffix + 7, &end, 10);
    if (end == suffix + 7 || *end || page_number < 1 || page_number > 999) {
        json_error(req, "400 Bad Request", "PAGE_OUT_OF_ORDER", "页码无效");
        return ESP_OK;
    }
    if (req->content_len <= 0 || req->content_len > MAX_PAGE_BYTES) {
        json_error(req, "413 Payload Too Large", "PAGE_TOO_LARGE", "页面必须为 1 字节至 2 MiB");
        return ESP_OK;
    }
    char expected[65] = {0};
    bool has_hash = httpd_req_get_hdr_value_str(req, "X-Page-SHA256", expected,
                                                 sizeof(expected)) == ESP_OK;
    if ((has_hash && !valid_sha256(expected)) ||
        (!has_hash && !CONFIG_BIPS_DEVELOPMENT_MODE)) {
        json_error(req, "400 Bad Request", "PAGE_HASH_MISMATCH", "缺少或无效的 SHA-256");
        return ESP_OK;
    }
    xSemaphoreTake(session_lock, portMAX_DELAY);
    refresh_print_session_locked();
    if (!print_session.active || strcmp(print_session.id, id) != 0) {
        xSemaphoreGive(session_lock);
        json_error(req, "404 Not Found", "SESSION_NOT_FOUND", "打印会话不存在或已结束");
        return ESP_OK;
    }
    if (strcmp(print_session.owner, owner) != 0) {
        xSemaphoreGive(session_lock);
        json_error(req, "403 Forbidden", "SESSION_OWNER_MISMATCH", "不是会话所有者");
        return ESP_OK;
    }
    if (print_session.current_page == page_number &&
        print_session.page_state != PAGE_NONE &&
        print_session.page_state != PAGE_FAILED) {
        bool same = has_hash && strcmp(print_session.page_sha256, expected) == 0;
        bool receiving = print_session.page_state == PAGE_RECEIVING;
        cJSON *data = same && !receiving ? session_json_locked() : NULL;
        xSemaphoreGive(session_lock);
        if (data) json_data(req, "202 Accepted", data);
        else json_error(req, "409 Conflict", receiving && same ? "UPLOAD_IN_PROGRESS" : "PAGE_CONFLICT",
                        "这一页已经接收或发送");
        return ESP_OK;
    }
    if (page_number != print_session.next_page) {
        xSemaphoreGive(session_lock);
        json_error(req, "409 Conflict", "PAGE_OUT_OF_ORDER", "页码不是下一页");
        return ESP_OK;
    }
    if (print_session.cancel_requested || print_session.page_state == PAGE_UNKNOWN ||
        print_session.page_state == PAGE_QUEUED || print_session.page_state == PAGE_SENDING) {
        xSemaphoreGive(session_lock);
        json_error(req, "409 Conflict", "PAGE_NOT_READY", "上一页尚未结束");
        return ESP_OK;
    }
    print_session.current_page = page_number;
    print_session.page_state = PAGE_RECEIVING;
    snprintf(print_session.page_sha256, sizeof(print_session.page_sha256), "%s", expected);
    print_session.lease_deadline = esp_timer_get_time() + 120LL * 1000000;
    xSemaphoreGive(session_lock);

    uint8_t *page = heap_caps_malloc(req->content_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!page) {
        abort_receiving_page(id, page_number);
        json_error(req, "503 Service Unavailable", "INSUFFICIENT_MEMORY", "PSRAM 页面缓冲不足");
        return ESP_OK;
    }
#if ESP_IDF_VERSION_MAJOR < 6
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
#endif
    size_t offset = 0;
    int64_t started = esp_timer_get_time();
    ESP_LOGI(TAG, "Page %ld upload started: %d bytes", page_number, req->content_len);
    while (offset < req->content_len) {
        if (esp_timer_get_time() - started > 120LL * 1000000) break;
        size_t wanted = req->content_len - offset;
        if (wanted > 16384) wanted = 16384;
        int got = httpd_req_recv(req, (char *)page + offset, wanted);
        if (got <= 0) break;
#if ESP_IDF_VERSION_MAJOR < 6
        mbedtls_sha256_update(&sha, page + offset, got);
#endif
        offset += got;
    }
    if (offset != req->content_len) {
        ESP_LOGW(TAG, "Page upload incomplete: %u/%d", (unsigned)offset, req->content_len);
#if ESP_IDF_VERSION_MAJOR < 6
        mbedtls_sha256_free(&sha);
#endif
        free(page);
        abort_receiving_page(id, page_number);
        json_error(req, "400 Bad Request", "PAGE_LENGTH_MISMATCH", "页面未完整接收");
        return ESP_OK;
    }
    uint8_t digest[32];
    char actual[65];
#if ESP_IDF_VERSION_MAJOR >= 6
    size_t digest_len = 0;
    if (psa_crypto_init() != PSA_SUCCESS ||
        psa_hash_compute(PSA_ALG_SHA_256, page, offset, digest, sizeof(digest), &digest_len) != PSA_SUCCESS ||
        digest_len != sizeof(digest)) {
        free(page);
        abort_receiving_page(id, page_number);
        json_error(req, "503 Service Unavailable", "HASH_FAILED", "无法计算页面 SHA-256");
        return ESP_OK;
    }
#else
    mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);
#endif
    for (int i = 0; i < 32; ++i) snprintf(actual + i * 2, 3, "%02x", digest[i]);
    if (has_hash && strcmp(expected, actual) != 0) {
        free(page);
        abort_receiving_page(id, page_number);
        json_error(req, "400 Bad Request", "PAGE_HASH_MISMATCH", "页面 SHA-256 不匹配");
        return ESP_OK;
    }
    xSemaphoreTake(session_lock, portMAX_DELAY);
    if (strcmp(print_session.id, id) != 0 || !print_session.active ||
        print_session.cancel_requested || print_session.page_state != PAGE_RECEIVING) {
        xSemaphoreGive(session_lock);
        free(page);
        abort_receiving_page(id, page_number);
        json_error(req, "409 Conflict", "SESSION_NOT_FOUND", "会话在上传期间结束");
        return ESP_OK;
    }
    uint32_t usb_job_id = 0;
    if (!printer_submit(page, offset, &usb_job_id)) {
        print_session.page_state = PAGE_FAILED;
        xSemaphoreGive(session_lock);
        free(page);
        json_error(req, "409 Conflict", "PRINTER_NOT_READY", "打印机状态已改变");
        return ESP_OK;
    }
    snprintf(print_session.page_sha256, sizeof(print_session.page_sha256), "%s", actual);
    print_session.usb_job_id = usb_job_id;
    print_session.page_state = PAGE_QUEUED;
    cJSON *data = session_json_locked();
    xSemaphoreGive(session_lock);
    ESP_LOGI(TAG, "Page %ld queued as USB job #%" PRIu32, page_number, usb_job_id);
    json_data(req, "202 Accepted", data);
    return ESP_OK;
}

static void page_receive_task(void *arg)
{
    httpd_req_t *request = arg;
    page_receive(request);
    httpd_req_async_handler_complete(request);
    vTaskDelete(NULL);
}

static esp_err_t page_upload_handler(httpd_req_t *req)
{
    httpd_req_t *async = NULL;
    esp_err_t err = httpd_req_async_handler_begin(req, &async);
    if (err != ESP_OK) return err;
    if (xTaskCreate(page_receive_task, "page_receive", 8192, async, 4, NULL) != pdPASS) {
        json_error(async, "503 Service Unavailable", "INSUFFICIENT_MEMORY", "无法启动上传任务");
        httpd_req_async_handler_complete(async);
    }
    return ESP_OK;
}

static void register_uri(httpd_handle_t server, const char *path, httpd_method_t method,
                         esp_err_t (*handler)(httpd_req_t *))
{
    httpd_uri_t uri = {.uri = path, .method = method, .handler = handler};
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uri));
}

void web_service_start(void *arg)
{
    (void)arg;
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);
    device_identity_init();
    check_recovery_gpio();
    load_ap_credentials();
    nvs_handle_t nvs;
    if (nvs_open("auth", NVS_READONLY, &nvs) == ESP_OK) {
        uint8_t changed = 0;
        if (nvs_get_u8(nvs, "changed", &changed) == ESP_OK) password_changed = changed == 1;
        nvs_close(nvs);
    }
    auth_lock = xSemaphoreCreateMutex();
    configASSERT(auth_lock);
    session_lock = xSemaphoreCreateMutex();
    configASSERT(session_lock);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *ap = esp_netif_create_default_wifi_ap();
    ESP_ERROR_CHECK(esp_netif_dhcps_stop(ap));
    esp_netif_ip_info_t ip = {0};
    IP4_ADDR(&ip.ip, 192, 168, 188, 1);
    IP4_ADDR(&ip.gw, 192, 168, 188, 1);
    IP4_ADDR(&ip.netmask, 255, 255, 255, 0);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(ap, &ip));
    ESP_ERROR_CHECK(esp_netif_dhcps_start(ap));
    wifi_init_config_t wifi_init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    wifi_config_t wifi = {0};
    wifi.ap.ssid_len = strlen(ap_ssid);
    memcpy(wifi.ap.ssid, ap_ssid, wifi.ap.ssid_len);
    snprintf((char *)wifi.ap.password, sizeof(wifi.ap.password), "%s", ap_password);
    wifi.ap.channel = 1;
    wifi.ap.max_connection = 4;
    wifi.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "SoftAP %s started at 192.168.188.1; clients <= 4", ap_ssid);
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 15;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.recv_wait_timeout = 30;
    config.send_wait_timeout = 15;
    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &config));
    register_uri(server, "/", HTTP_GET, page_handler);
    register_uri(server, "/api/v1/auth/login", HTTP_POST, login_handler);
    register_uri(server, "/api/v1/auth/password", HTTP_PUT, password_handler);
    register_uri(server, "/api/v1/auth/logout", HTTP_POST, logout_handler);
    register_uri(server, "/api/v1/auth/refresh", HTTP_POST, refresh_handler);
    register_uri(server, "/api/v1/printer/status", HTTP_GET, status_handler);
    register_uri(server, "/api/v1/printer/info", HTTP_GET, info_handler);
    register_uri(server, "/api/v1/device/status", HTTP_GET, device_status_handler);
    register_uri(server, "/api/v1/device/wifi", HTTP_GET, wifi_config_handler);
    register_uri(server, "/api/v1/device/wifi", HTTP_PUT, wifi_config_handler);
    register_uri(server, "/api/v1/device/reboot", HTTP_POST, reboot_handler);
    register_uri(server, "/api/v1/print-sessions", HTTP_POST, session_create_handler);
    register_uri(server, "/api/v1/print-sessions/*", HTTP_GET, session_get_handler);
    register_uri(server, "/api/v1/print-sessions/*", HTTP_POST, session_post_handler);
    register_uri(server, "/api/v1/print-sessions/*", HTTP_DELETE, session_delete_handler);
    ESP_LOGI(TAG, "Maintenance page ready at http://192.168.188.1/");
    vTaskDelete(NULL);
}
