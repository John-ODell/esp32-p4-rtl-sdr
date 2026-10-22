/* ES8311 over I2C (SDA 7, SCL 8) and I2S (MCLK 13, BCLK 12, WS 10; the P4 sends on GPIO9 to the
 * codec's DSDIN), amplifier enable on GPIO53. 48 kHz mono, 16 bit.
 *
 * The reader follows the IQ stream in order. The I2S buffer (30 ms) paces it, so when the codec's
 * clock and the dongle's drift apart the backlog slowly grows or shrinks: past 100 ms it skips
 * ahead, and a short wait for data is covered by the buffer. */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "es8311_codec.h"
#include "sdr.h"
#include "audio.h"

static const char *TAG = "audio";

#define PIN_SDA   7
#define PIN_SCL   8
#define PIN_MCLK  13
#define PIN_BCLK  12
#define PIN_WS    10
#define PIN_DOUT  9                           /* to the codec's DSDIN */
#define PIN_PA    53

#define READ_MAX  (2 * 4800)                  /* bytes per pass: 2 ms of IQ, 96 samples out */
#define BYTES_MS  (2 * DEMOD_IN_RATE / 1000)
#define LAG_MAX   (100 * BYTES_MS)
#define LAG_KEEP  (20 * BYTES_MS)

static esp_codec_dev_handle_t s_dev;
static volatile bool s_paused = true, s_open;
static volatile int s_req_vol = -1, s_req_sql = -1;
static volatile bool s_req_mode, s_req_floor;
static volatile demod_mode_t s_mode = DEMOD_WFM;
static volatile uint32_t s_bw = 200000;
static volatile int s_load, s_lag;

static void set_volume(int v)
{
    esp_codec_dev_set_out_mute(s_dev, v == 0);
    if (v) esp_codec_dev_set_out_vol(s_dev, 40 + 6 * v);   /* 1: -27 dB .. 10: 0 dB */
}

static void audio_task(void *arg)
{
    uint8_t *iq = heap_caps_malloc(READ_MAX, MALLOC_CAP_SPIRAM);
    int16_t *pcm = heap_caps_malloc((READ_MAX / 100 + 4) * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!iq || !pcm) { ESP_LOGE(TAG, "no memory"); vTaskDelete(NULL); }
    uint64_t pos = 0, dropped = 0;
    int64_t busy = 0, t_stat = esp_timer_get_time();
    int lag_max = 0;
    bool was_paused = true;
    for (;;) {
        if (s_req_vol >= 0) { set_volume(s_req_vol); s_req_vol = -1; }
        if (s_req_sql >= 0) { demod_set_squelch(s_req_sql); s_req_sql = -1; }
        if (s_req_mode) { s_req_mode = false; demod_set(s_mode, s_bw); }
        if (s_req_floor) { s_req_floor = false; demod_reset_floor(); }
        int64_t now = esp_timer_get_time();
        if (now - t_stat >= 5000000) {
            s_load = (int)(100 * busy / (now - t_stat));
            s_lag = lag_max / BYTES_MS;
            busy = 0;
            lag_max = 0;
            t_stat = now;
        }
        if (s_paused) {
            was_paused = true;
            s_open = false;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        uint64_t total = sdr_total_bytes();
        if (was_paused || total - pos > LAG_MAX) {           /* (re)start a little behind */
            pos = total > LAG_KEEP ? (total - LAG_KEEP) & ~1ULL : 0;
            was_paused = false;
        }
        if ((int)(total - pos) > lag_max) lag_max = (int)(total - pos);
        size_t n = sdr_ring_read(iq, &pos, READ_MAX, &dropped) & ~1u;
        if (n < 100) { vTaskDelay(1); continue; }
        int64_t t0 = esp_timer_get_time();
        int k = demod_process(iq, n / 2, pcm);
        busy += esp_timer_get_time() - t0;
        s_open = demod_open();
        if (k) esp_codec_dev_write(s_dev, pcm, k * sizeof(int16_t));   /* blocks while the buffer is full */
    }
}

bool audio_init(void)
{
    i2c_master_bus_handle_t bus;
    i2c_master_bus_config_t bc = {.i2c_port = I2C_NUM_0, .sda_io_num = PIN_SDA, .scl_io_num = PIN_SCL,
                                  .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
                                  .flags.enable_internal_pullup = true};
    if (i2c_new_master_bus(&bc, &bus) != ESP_OK) return false;
    if (i2c_master_probe(bus, ES8311_CODEC_DEFAULT_ADDR >> 1, 50) != ESP_OK) {
        ESP_LOGW(TAG, "no ES8311 at 0x%02x: no audio", ES8311_CODEC_DEFAULT_ADDR >> 1);
        i2c_del_master_bus(bus);
        return false;
    }

    i2s_chan_handle_t tx;
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cc.dma_desc_num = 6;
    cc.dma_frame_num = 240;                   /* 6 x 5 ms */
    cc.auto_clear = true;                     /* silence, not a loop, if the reader stalls */
    if (i2s_new_channel(&cc, &tx, NULL) != ESP_OK) return false;
    i2s_std_config_t sc = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(DEMOD_OUT_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {.mclk = PIN_MCLK, .bclk = PIN_BCLK, .ws = PIN_WS, .dout = PIN_DOUT, .din = I2S_GPIO_UNUSED},
    };
    if (i2s_channel_init_std_mode(tx, &sc) != ESP_OK) return false;

    audio_codec_i2c_cfg_t ic = {.port = I2C_NUM_0, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = bus};
    audio_codec_i2s_cfg_t dc = {.port = I2S_NUM_0, .tx_handle = tx};
    es8311_codec_cfg_t ec = {
        .ctrl_if = audio_codec_new_i2c_ctrl(&ic),
        .gpio_if = audio_codec_new_gpio(),
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = PIN_PA,
        .use_mclk = true,
        .hw_gain = {.pa_voltage = 5.0f, .codec_dac_voltage = 3.3f},
    };
    const audio_codec_if_t *codec = es8311_codec_new(&ec);
    if (!codec) return false;
    esp_codec_dev_cfg_t dev = {.dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = codec,
                               .data_if = audio_codec_new_i2s_data(&dc)};
    s_dev = esp_codec_dev_new(&dev);
    esp_codec_dev_sample_info_t fs = {.bits_per_sample = 16, .channel = 1, .sample_rate = DEMOD_OUT_RATE};
    if (!s_dev || esp_codec_dev_open(s_dev, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "ES8311 did not open: no audio");
        return false;
    }
    set_volume(0);
    if (!demod_init()) return false;
    xTaskCreatePinnedToCore(audio_task, "audio", 6144, NULL, 6, NULL, 0);
    ESP_LOGI(TAG, "ES8311 ready, 48 kHz");
    return true;
}

bool audio_ok(void) { return s_dev != NULL; }

void audio_set_mode(demod_mode_t mode, uint32_t bw_hz)
{
    s_mode = mode;
    s_bw = bw_hz;
    s_req_mode = true;
}

void audio_set_volume(int v) { s_req_vol = v < 0 ? 0 : v > 10 ? 10 : v; }
void audio_set_squelch(int level) { s_req_sql = level < 0 ? 0 : level > 9 ? 9 : level; }
void audio_retuned(void) { s_req_floor = true; }
void audio_pause(bool on) { s_paused = on; }
bool audio_squelch_open(void) { return s_open; }

void audio_stats(int *load_pct, int *lag_ms)
{
    *load_pct = s_load;
    *lag_ms = s_lag;
}
