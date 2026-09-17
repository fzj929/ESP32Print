#pragma once

typedef enum {
    LED_DELIVERED = 0,
    LED_WAITING = 1,
    LED_OPENING = 2,
    LED_NOT_READY = 3,
    LED_SENDING = 4,
    LED_STATUS_UNKNOWN = 5,
    LED_DISCONNECTED = 6,
    LED_ERROR = 7,
    LED_WRONG_DEVICE = 8,
} status_led_state_t;

void status_led_init(void);
void status_led_set(status_led_state_t state);
void status_led_tick(void);
