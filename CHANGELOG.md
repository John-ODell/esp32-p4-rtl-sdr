# Unreleased
- Audio out of the kit's speaker connector (ES8311): WFM, NFM and AM, picked by the filter, with
  volume and squelch on the new AUDIO page (it replaces FILTER) and a mode plate on the spectrum.
  `tools/demod_test` checks the demodulator on a PC. Not yet heard on hardware.
- The squelch fades audio in and out over 5 ms instead of switching it, and AM picks up a new
  carrier's level at once, so opening and closing shouldn't click.
- RDS on broadcast FM: the station name shows on the spectrum, and PI, name and RadioText go to
  the serial log. Checked on synthesized stations only.
- Neon look (violet, cyan and magenta), the fifth in MENU › LOOK.
- Signal markers: up to three strong signals outside the passband are labelled with their frequency.
- The S/N readout is a ten-segment bar meter with the dB figure.
- Portrait mode (MENU › LOOK › TURN), USB ports up, with a layout for every screen. Saved across
  reboots; touch calibration is turned in software. Not yet checked on hardware.

# 1.1.0 (2026-10-18)
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
