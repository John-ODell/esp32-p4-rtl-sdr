idf_component_register(SRCS "main.c" "sdr.c" "spectrum.c" "view.c" "panel.c" "adsb.c" "adsb_view.c" "amap.c" "sdcard.c" "sdlog.c" "scan_view.c" "radio_scan.c" "ui.c"
                       INCLUDE_DIRS "."
                       REQUIRES rpi_display rtlsdr usb driver esp_timer nvs_flash fatfs sdmmc esp_driver_sdmmc esp_wifi esp_event bt)
