# Third-party material

- `components/rtlsdr/blog/`: librtlsdr from the RTL-SDR Blog fork (rtlsdrblog/rtl-sdr-blog,
  commit aed0ea1), copied unmodified. GPL-2.0-or-later; see `blog/COPYING` and the file headers.
  The libusb shim in `components/rtlsdr/shim/` was written for this project.
- `components/rpi_display/`: the 3.5" RPi display driver from
  [esp32-p4-3.5-inch-rpi-display](https://github.com/John-ODell/esp32-p4-3.5-inch-rpi-display)
  (MIT, same author), with the off-screen canvas added. Its `font8x16.h` is an 8x16 raster of
  DejaVu Sans Mono, taken from ValentinStars/esp32p4-linux (MIT). DejaVu fonts are under the
  Bitstream Vera / DejaVu license, which is permissive.
- `esp_hosted` and `esp_wifi_remote` (Espressif, Apache-2.0) are not included. The component
  manager fetches them at build time, and the prebuilt images link them.
- ESP-IDF (Apache-2.0) is not included. It is needed to build, and the prebuilt images link it.
- `tools/mapgen/make_map.py` downloads its data at run time; none is included here.
  Natural Earth is public domain. OpenStreetMap data is © OpenStreetMap contributors and
  available under the ODbL, so a `MAP.BIN` with OSM layers carries that attribution.
- `docs/images/esp32-p4-module-dev-kit.jpg`: product photo from Waveshare's
  ESP32-P4-Module-DEV-KIT page, used only to identify the hardware. It is not covered by this
  repository's license. `docs/images/mhs-3.5inch-display-ips.jpg` is the author's own photo of the
  tested screen. The other images in `docs/images` are renders from `tools/preview`.
