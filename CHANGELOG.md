# Unreleased
- CSV logs on the SD card: ADS-B aircraft, Wi-Fi networks and BLE devices go to `LOGS/` while
  each mode is open, with a REC marker in the header.
- The 5 s serial stats lines report free internal RAM, its low-water mark and the largest DMA block.
- `tools/capture_log.py --attach` logs without resetting the board and survives serial dropouts.

# 1.0.0 (2026-10-18)
- First release, for ESP-IDF 5.5 on the Waveshare ESP32-P4-Module-DEV-KIT with the MPI3501 3.5" screen.
- RTL-SDR Blog V4 over the kit's USB host: spectrum and waterfall with tap-to-tune, and a menu with
  gain, range, zoom, filter shading, 8 band presets and four looks.
- ADS-B mode: aircraft list and map, with an optional SD-card base map made by `tools/mapgen`.
- Wi-Fi and BLE scanners on the module's ESP32-C6 (esp_hosted 1.4).
- Prebuilt images for chip revision v1.x (tested) and v3.1+ (untested), with `SHA256SUMS` and
  `workbench-manifest.json`.
