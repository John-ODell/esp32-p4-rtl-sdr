#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sdcard.h"
#include "sdlog.h"

#define RING     (64 * 1024)                 /* PSRAM; about a minute of a busy ADS-B log */
#define CHUNK    (8 * 1024)                  /* largest single write */
#define FLUSH_MS 2000

static const char *TAG = "sdlog";
static char *s_ring, *s_chunk;
static size_t s_head, s_tail;                /* free-running byte counts */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static FILE *s_f;
static volatile int s_state = SDLOG_OFF;
static volatile bool s_stop;
static TaskHandle_t s_task;
static SemaphoreHandle_t s_done;
static unsigned s_dropped, s_session;
static char s_name[24];

/* Only the writer takes, and sdlog_row never overwrites unread bytes, so the copy itself can
 * run outside the lock. */
static size_t take(char *out, size_t max)
{
    taskENTER_CRITICAL(&s_mux);
    size_t tail = s_tail, n = s_head - s_tail;
    taskEXIT_CRITICAL(&s_mux);
    if (n > max) n = max;
    size_t at = tail % RING, first = n < RING - at ? n : RING - at;
    memcpy(out, s_ring + at, first);
    memcpy(out + first, s_ring, n - first);
    taskENTER_CRITICAL(&s_mux);
    s_tail = tail + n;
    taskEXIT_CRITICAL(&s_mux);
    return n;
}

static void writer(void *arg)
{
    unsigned long long bytes = 0;
    for (;;) {
        bool stop = s_stop;
        size_t n;
        bool wrote = false;
        while ((n = take(s_chunk, CHUNK)) > 0) {
            if (s_state == SDLOG_REC && fwrite(s_chunk, 1, n, s_f) != n) {
                ESP_LOGE(TAG, "%s: write failed", s_name);
                s_state = SDLOG_ERR;
            }
            bytes += n;
            wrote = true;
        }
        if (wrote && s_state == SDLOG_REC && (fflush(s_f) != 0 || fsync(fileno(s_f)) != 0)) {
            ESP_LOGE(TAG, "%s: sync failed", s_name);
            s_state = SDLOG_ERR;
        }
        if (stop) break;
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(FLUSH_MS));
    }
    ESP_LOGI(TAG, "%s closed: %llu bytes this time, %u rows dropped", s_name, bytes, s_dropped);
    xSemaphoreGive(s_done);
    vTaskDelete(NULL);
}

static unsigned next_session(void)
{
    if (s_session) return s_session;
    nvs_handle_t h;
    uint32_t n = 0;
    if (nvs_open("sdr", NVS_READWRITE, &h) == ESP_OK) {
        nvs_get_u32(h, "logn", &n);
        n = n % 9999 + 1;
        nvs_set_u32(h, "logn", n);
        nvs_commit(h);
        nvs_close(h);
    }
    return s_session = n ? n : 1;
}

bool sdlog_begin(const char *kind, const char *header)
{
    if (s_task) return false;
    if (!s_ring) s_ring = heap_caps_malloc(RING, MALLOC_CAP_SPIRAM);
    if (!s_chunk) s_chunk = heap_caps_malloc(CHUNK, MALLOC_CAP_SPIRAM);
    if (!s_done) s_done = xSemaphoreCreateBinary();
    if (!s_ring || !s_chunk || !s_done || !sdcard_mount()) return false;
    mkdir(SDCARD_ROOT "/LOGS", 0777);
    snprintf(s_name, sizeof s_name, "%.4s%04u.CSV", kind, next_session());
    char path[48];
    snprintf(path, sizeof path, SDCARD_ROOT "/LOGS/%s", s_name);
    struct stat st;
    bool fresh = stat(path, &st) != 0 || st.st_size == 0;
    if (!(s_f = fopen(path, "a"))) {
        ESP_LOGW(TAG, "can't open %s", path);
        sdcard_unmount();
        return false;
    }
    s_head = s_tail = 0;
    s_dropped = 0;
    s_stop = false;
    s_state = SDLOG_REC;
    if (fresh) sdlog_row("%s", header);
    /* low priority, core 0 (with USB, which outranks it): card waits never touch the UI core */
    if (xTaskCreatePinnedToCore(writer, "sdlog", 4096, NULL, 2, &s_task, 0) != pdPASS) {
        fclose(s_f);
        s_f = NULL;
        s_state = SDLOG_OFF;
        sdcard_unmount();
        return false;
    }
    ESP_LOGI(TAG, "logging to %s%s", path, fresh ? " (new)" : " (appending)");
    return true;
}

void sdlog_row(const char *fmt, ...)
{
    if (s_state != SDLOG_REC) return;
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line - 1, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof line - 2) n = sizeof line - 2;
    line[n++] = '\n';
    taskENTER_CRITICAL(&s_mux);
    if (RING - (s_head - s_tail) < (size_t)n) s_dropped++;
    else for (int i = 0; i < n; i++) s_ring[s_head++ % RING] = line[i];
    taskEXIT_CRITICAL(&s_mux);
}

const char *sdlog_str(char *out, int size, const char *s)
{
    int j = 0;
    out[j++] = '"';
    for (; *s && j < size - 3; s++) {
        unsigned char ch = *s;
        if (ch == '"') { if (j >= size - 4) break; out[j++] = '"'; out[j++] = '"'; }
        else out[j++] = ch < 0x20 || ch == 0x7f ? '?' : ch;
    }
    out[j++] = '"';
    out[j] = 0;
    return out;
}

void sdlog_end(void)
{
    if (!s_task) return;
    s_stop = true;
    xTaskNotifyGive(s_task);
    xSemaphoreTake(s_done, portMAX_DELAY);
    s_task = NULL;
    fclose(s_f);
    s_f = NULL;
    s_state = SDLOG_OFF;
    sdcard_unmount();
}

int sdlog_state(void) { return s_state; }
