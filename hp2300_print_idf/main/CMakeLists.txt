if(IDF_VERSION_MAJOR GREATER_EQUAL 6)
    set(usb_requirement espressif__usb)
else()
    set(usb_requirement usb)
endif()

idf_component_register(
    SRCS "hp2300_print.c" "status_led.c"
    INCLUDE_DIRS "."
    REQUIRES ${usb_requirement} esp_system esp_timer led_strip
    EMBED_FILES "1.prn"
)
