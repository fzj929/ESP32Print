#include "status_led.h"

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "led_strip.h"

#define STATUS_LED_GPIO 48
#define STATUS_LED_BRIGHTNESS 80

static const char *TAG = "STATUS_LED";
static led_strip_handle_t strip;
static status_led_state_t current = LED_WAITING;
static int64_t epoch_ms;
static int previous = -1;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void apply(bool on)
{
    uint8_t r = 0, g = 0, b = 0;
    if (on) {
        switch (current) {
        case LED_DELIVERED: g = STATUS_LED_BRIGHTNESS; break;
        case LED_OPENING: g = b = STATUS_LED_BRIGHTNESS; break;
        case LED_NOT_READY: r = g = STATUS_LED_BRIGHTNESS; break;
        case LED_SENDING: b = STATUS_LED_BRIGHTNESS; break;
        case LED_STATUS_UNKNOWN: r = b = STATUS_LED_BRIGHTNESS; break;
        case LED_WRONG_DEVICE:
            r = STATUS_LED_BRIGHTNESS;
            g = STATUS_LED_BRIGHTNESS / 3;
            break;
        default: r = STATUS_LED_BRIGHTNESS; break;
        }
    }
    if (strip) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(led_strip_set_pixel(strip, 0, r, g, b));
        ESP_ERROR_CHECK_WITHOUT_ABORT(led_strip_refresh(strip));
    }
}

void status_led_tick(void)
{
    unsigned count = (unsigned)current;
    uint32_t cycle = count * 400U + 1600U;
    uint32_t pos = (uint32_t)(now_ms() - epoch_ms) % cycle;
    bool on = count == 0 || (pos < count * 400U && pos % 400U < 200U);
    if (previous != (int)on) {
        apply(on);
        previous = (int)on;
    }
}

void status_led_set(status_led_state_t state)
{
    if (current != state) {
        current = state;
        epoch_ms = now_ms();
        previous = -1;
    }
    status_led_tick();
}

void status_led_init(void)
{
    const led_strip_config_t strip_config = {
        .strip_gpio_num = STATUS_LED_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags.invert_out = false,
    };
    const led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 0,
        .flags.with_dma = false,
    };
    esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &strip);
    if (err != ESP_OK) {
        strip = NULL;
        ESP_LOGW(TAG, "GPIO48 RGB initialization failed: %s", esp_err_to_name(err));
        return;
    }
    epoch_ms = now_ms();
    previous = -1;
    status_led_tick();
}
