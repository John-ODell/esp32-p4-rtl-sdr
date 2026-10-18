# MPI3501 3.5" RPi display (ILI9486 behind a 16-bit shift-register bridge + XPT2046 touch)
# wired to the Waveshare ESP32-P4-Module-DEV-KIT header. Shared by p4-sdr and p4-linux.
idf_component_register(SRCS "rpi_lcd.c" "rpi_text.c" "rpi_canvas.c" "xpt2046.c" "touch_cal.c"
                       INCLUDE_DIRS "include"
                       REQUIRES driver esp_timer nvs_flash)
