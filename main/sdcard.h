#pragma once
/* The kit's micro-SD slot (SDMMC slot 0, 4-bit: CLK 43, CMD 44, D0-D3 39-42; card power from
 * on-chip LDO 4 switched by GPIO45 low). Mounted only for the length of a read, so the FAT
 * buffers never sit in the internal RAM the USB transfers need. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Read a whole file from the card root into a fresh PSRAM buffer (free() it). Files over
 * max bytes are refused. Names are 8.3 (MAP.BIN). False if no card, no file or an error. */
bool sdcard_read(const char *name, uint8_t **out, size_t *len, size_t max);
