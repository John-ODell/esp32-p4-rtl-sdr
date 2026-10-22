#!/usr/bin/env python3
"""Write dist/workbench-manifest.json for web flashers (the Northbridge Landmark Workbench).

Each image entry gives its release URL, flash address, size, SHA-256 and the chip revisions
it boots on, read from that variant's build-<rev>/sdkconfig (revisions as esptool-js reports
them: major * 100 + minor, so v1.0 = 100).

usage: make_manifest.py VERSION REV [REV ...]     (run from the repo root, after the builds)
"""
import hashlib, json, os, sys

REPO = "John-ODell/esp32-p4-rtl-sdr"


def sdkconfig_int(path, key):
    for line in open(path):
        if line.startswith(key + "="):
            return int(line.split("=", 1)[1])
    raise SystemExit(f"{path}: {key} not set")


def main():
    ver, revs = sys.argv[1], sys.argv[2:]
    images = []
    for rev in revs:
        name = f"p4-sdr-{ver}-{rev}.bin"
        data = open(os.path.join("dist", name), "rb").read()
        cfg = os.path.join(f"build-{rev}", "sdkconfig")
        images.append({
            "file": name,
            "url": f"https://github.com/{REPO}/releases/download/v{ver}/{name}",
            "address": 0,
            "size": len(data),
            "sha256": hashlib.sha256(data).hexdigest(),
            "chipRevisionMin": sdkconfig_int(cfg, "CONFIG_ESP_REV_MIN_FULL"),
            "chipRevisionMax": sdkconfig_int(cfg, "CONFIG_ESP_REV_MAX_FULL"),
        })
    manifest = {
        "manifestVersion": 1,             # bump if a field changes meaning
        "name": "P4 SDR",
        "version": ver,
        "chip": "ESP32-P4",
        "board": "Waveshare ESP32-P4-Module-DEV-KIT",
        "display": "MHS-3.5inch Display-IPS, 3.5in RPi display (ILI9486 + XPT2046)",
        "description": "RTL-SDR spectrum and waterfall receiver with ADS-B aircraft list and map, "
                       "and Wi-Fi and BLE scanners on the module's ESP32-C6.",
        "home": f"https://github.com/{REPO}",
        "requires": [
            "RTL-SDR Blog V4 dongle in one of the kit's USB-A ports, jumper H3 on HOST",
            "Optional: FAT32 micro-SD card with MAP.BIN (tools/mapgen) for the ADS-B map",
        ],
        "notes": [
            "Each image is a full flash image written at 0x0; it erases saved settings and touch calibration.",
            "Pick the image whose chipRevisionMin..chipRevisionMax holds the chip revision.",
            "First boot after flashing runs the 5-point touch calibration; tap each target.",
            "Serial console on the kit's USB TO UART port at 115200 logs fps, USB and decoder stats.",
        ],
        "images": images,
    }
    with open(os.path.join("dist", "workbench-manifest.json"), "w") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")
    print("dist/workbench-manifest.json", ", ".join(
        f"{i['file']} rev {i['chipRevisionMin']}-{i['chipRevisionMax']}" for i in images))


if __name__ == "__main__":
    main()
