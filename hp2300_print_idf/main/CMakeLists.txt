if(IDF_VERSION_MAJOR GREATER_EQUAL 6)
    set(usb_requirement espressif__usb)
else()
    set(usb_requirement usb)
endif()

idf_component_register(
    SRCS "hp2300_print.c" "status_led.c" "web_service.c" "device_identity.c"
    INCLUDE_DIRS "."
    REQUIRES ${usb_requirement} esp_system esp_timer led_strip esp_wifi esp_netif
             esp_http_server nvs_flash mbedtls esp_psram efuse esp_driver_gpio
    EMBED_TXTFILES "index.html"
)
