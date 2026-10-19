#pragma once
/* The kit's micro-SD slot (SDMMC slot 0, 4-bit: CLK 43, CMD 44, D0-D3 39-42; card power from
 * on-chip LDO 4 switched by GPIO45 low). Mounted at /sd only while something needs it; the FAT
 * buffers go to PSRAM (CONFIG_FATFS_ALLOC_PREFER_EXTRAM) so the internal RAM stays with USB.
 * Slot 1 belongs to the ESP32-C6 link; unmounting deinitialises slot 0 only. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SDCARD_ROOT "/sd"

/* Counted: each true return needs one sdcard_unmount(). False if there is no card. */
bool sdcard_mount(void);
void sdcard_unmount(void);

/* Read a whole file from the card root into a fresh PSRAM buffer (free() it). Files over
 * max bytes are refused. Names are 8.3 (MAP.BIN). False if no card, no file or an error. */
bool sdcard_read(const char *name, uint8_t **out, size_t *len, size_t max);
