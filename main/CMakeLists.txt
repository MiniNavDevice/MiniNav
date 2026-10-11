idf_component_register(
    SRCS "app_main.cpp" "board.cpp" "net.cpp" "aap.cpp" "video.cpp" "demo.cpp"
    INCLUDE_DIRS "."
    REQUIRES LovyanGFX esp_wifi esp_netif esp_event nvs_flash mbedtls esp_timer fatfs sdmmc
             esp_driver_sdmmc esp_driver_gpio esp_driver_usb_serial_jtag lwip
    EMBED_FILES "demo.h264")
