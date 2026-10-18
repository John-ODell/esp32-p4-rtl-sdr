# librtlsdr from the RTL-SDR Blog fork (V4 support), GPL-2.0-or-later, sources in blog/ unmodified,
# on a libusb-1.0 subset implemented over the ESP-IDF USB Host Library (shim/).
idf_component_register(
    SRCS "blog/src/librtlsdr.c"
         "blog/src/tuner_e4k.c" "blog/src/tuner_fc0012.c" "blog/src/tuner_fc0013.c"
         "blog/src/tuner_fc2580.c" "blog/src/tuner_r82xx.c"
         "shim/libusb_esp.c"
    INCLUDE_DIRS "include" "blog/include"
    PRIV_INCLUDE_DIRS "shim"
    REQUIRES usb)
target_compile_definitions(${COMPONENT_LIB} PRIVATE rtlsdr_STATIC)
# Upstream code, upstream warnings: don't let -Werror stop the build on them.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error -Wno-unused-variable -Wno-unused-function
    -Wno-unused-but-set-variable -Wno-format -Wno-maybe-uninitialized -Wno-implicit-fallthrough)
