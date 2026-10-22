# shared/ holds chip-independent code (screen protocol, framebuffer, fonts, battery math, GPIO17 deep-sleep handoff steps).
idf_component_register(SRCS "main.c" "kr_ble.c" "kr_ui.c" "kr_batt.c" "kr_power.c"
                            "epd_spi.c" "epd_session.c"
                            "shared/fb.c" "shared/epd_proto.c"
                            "shared/font.c" "shared/font_jp16.c" "shared/font_jp24.c"
                            "shared/battery_model.c" "shared/safe_state_sleep_model.c"
                       INCLUDE_DIRS "." "shared"
                       PRIV_REQUIRES bt nvs_flash esp_driver_gpio esp_driver_spi
                                     esp_driver_usb_serial_jtag esp_timer esp_adc)
