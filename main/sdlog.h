#pragma once
/* CSV logs on the micro-SD card: /sd/LOGS/<KIND><session>.CSV, e.g. ADSB0007.CSV. The session
 * number goes up once per boot (NVS "sdr"/"logn"), so each power-on gets fresh files and
 * re-entering a mode appends to its file. Times are seconds since boot (the kit has no clock).
 *
 * Rows go into a PSRAM ring; a low-priority task writes them to the card every 2 s and syncs,
 * so a slow card never stalls the caller. If the ring fills, rows are dropped and counted. */
#include <stdbool.h>

enum { SDLOG_OFF, SDLOG_REC, SDLOG_ERR };

/* Mount the card and open the file; header is its first line when the file is new. False (and
 * nothing is logged) if there is no card. One log at a time. */
bool sdlog_begin(const char *kind, const char *header);
/* One CSV line, without the newline. Cheap; safe to call from any task. */
void sdlog_row(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Quote s as a CSV field into out (always quoted, " doubled, control bytes as '?'). */
const char *sdlog_str(char *out, int size, const char *s);
/* Flush, close, unmount. */
void sdlog_end(void);
/* SDLOG_OFF, SDLOG_REC, or SDLOG_ERR after a write failed (card pulled or full). */
int sdlog_state(void);
