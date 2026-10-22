/* SSD2683 SPI/GPIO backend for epd_io_t (SPI3). BUSY wait has a timeout; power rail is not touched here. */
#include "epd_spi.h"
#include <string.h>
#include "board_pins.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
static const char *TAG = "epd_spi";
static spi_device_handle_t s_dev;
static bool s_bus_ready;
#define BUSY_LOG_MAX 24
static struct { const char *label; int ms; } s_busy_log[BUSY_LOG_MAX];
static int s_busy_n;
void epd_spi_trace_reset(void) { s_busy_n = 0; }
int epd_spi_trace_get(const char *label)
{
    for (int i = s_busy_n - 1; i >= 0; i--) {
        if (strcmp(s_busy_log[i].label, label) == 0) { return s_busy_log[i].ms; }
    }
    return -1;
}
void epd_spi_trace_dump(const char *title)
{
    int total = 0;
    printf("        %s 各阶段 BUSY 等待:\n", title);
    for (int i = 0; i < s_busy_n; i++) {
        printf("          %-16s %5d ms\n", s_busy_log[i].label, s_busy_log[i].ms);
        total += s_busy_log[i].ms;
    }
    printf("          %-16s %5d ms\n", "(BUSY 合计)", total);
}
static epd_pump_fn s_pump;
static void *s_pump_ctx;
void epd_spi_set_pump(epd_pump_fn fn, void *ctx)
{
    s_pump = fn;
    s_pump_ctx = ctx;
}
static void pump_run(void)
{
    if (s_pump) {
        s_pump(s_pump_ctx);
    }
}
static void cs(int level) { gpio_set_level(PIN_EPD_CS, level); }
static void dc(int level) { gpio_set_level(PIN_EPD_DC, level); }
static int spi_write(const uint8_t *buf, size_t len)
{
    if (len == 0) { return EPD_OK; }
    spi_transaction_t t = { .length = len * 8, .tx_buffer = buf };
    return spi_device_polling_transmit(s_dev, &t) == ESP_OK ? EPD_OK : EPD_ERR_IO;
}
static int io_cmd(void *ctx, uint8_t c)
{
    (void)ctx;
    pump_run();
    dc(0);
    cs(0);
    int r = spi_write(&c, 1);
    cs(1);
    return r;
}
static int io_data(void *ctx, const uint8_t *buf, size_t len)
{
    (void)ctx;
    pump_run();
    dc(1);
    cs(0);
    int r = spi_write(buf, len);
    cs(1);
    return r;
}
static int io_busy_wait(void *ctx, int timeout_ms, const char *label)
{
    (void)ctx;
    int64_t t0 = esp_timer_get_time();
    while (gpio_get_level(PIN_EPD_BUSY) == 0) {
        if ((esp_timer_get_time() - t0) / 1000 > timeout_ms) {
            ESP_LOGE(TAG, "BUSY timeout label=%s after %dms", label, timeout_ms);
            return EPD_ERR_BUSY_TIMEOUT;
        }
        if (s_pump) {
            s_pump(s_pump_ctx);
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    if (s_busy_n < BUSY_LOG_MAX) {
        s_busy_log[s_busy_n].label = label;
        s_busy_log[s_busy_n].ms = (int)((esp_timer_get_time() - t0) / 1000);
        s_busy_n++;
    }
    return EPD_OK;
}
static int io_power(void *ctx, int on) { (void)ctx; (void)on; return EPD_OK; }
static void io_delay(void *ctx, int ms)
{
    (void)ctx;
    while (ms > 0) {
        pump_run();
        int step = ms > 10 ? 10 : ms;
        vTaskDelay(pdMS_TO_TICKS(step));
        ms -= step;
    }
    pump_run();
}
static int io_reset(void *ctx)
{
    (void)ctx;
    gpio_set_level(PIN_EPD_RST, 1); io_delay(NULL, 10);
    gpio_set_level(PIN_EPD_RST, 0); io_delay(NULL, 20);
    gpio_set_level(PIN_EPD_RST, 1); io_delay(NULL, 10);
    return EPD_OK;
}
static int io_read_temp(void *ctx, int *out_celsius)
{
    (void)ctx;
    uint8_t rx = 0;
    spi_transaction_t t = {
        .flags = SPI_TRANS_USE_RXDATA,
        .length = 0,
        .rxlength = 8,
    };
    dc(1);
    cs(0);
    esp_err_t e = spi_device_polling_transmit(s_dev, &t);
    cs(1);
    if (e != ESP_OK) {
        return EPD_ERR_IO;
    }
    rx = t.rx_data[0];
    if (rx == 0xFF) {
        ESP_LOGW(TAG, "temp raw=0xFF，疑似总线浮空或未应答");
        return EPD_ERR_IO;
    }
    *out_celsius = (int)(int8_t)rx;
    return EPD_OK;
}
esp_err_t epd_spi_init(epd_io_t *out_io)
{
    gpio_config_t out = {
        .pin_bit_mask = (1ULL << PIN_EPD_CS) | (1ULL << PIN_EPD_DC)
                      | (1ULL << PIN_EPD_RST),
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&out));
    cs(1);
    dc(0);
    gpio_set_level(PIN_EPD_RST, 1);
    gpio_config_t in = {
        .pin_bit_mask = 1ULL << PIN_EPD_BUSY,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&in));
    if (!s_bus_ready) {
        spi_bus_config_t bus = {
            .mosi_io_num = PIN_EPD_MOSI,
            .miso_io_num = -1,
            .sclk_io_num = PIN_EPD_SCK,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = 4096,
        };
        esp_err_t e = spi_bus_initialize(EPD_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
        if (e != ESP_OK) { return e; }
        s_bus_ready = true;
    }
    spi_device_interface_config_t dev = {
        .clock_speed_hz = 10 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = -1,
        .queue_size = 4,
        .flags = SPI_DEVICE_3WIRE | SPI_DEVICE_HALFDUPLEX,
    };
    esp_err_t e = spi_bus_add_device(EPD_SPI_HOST, &dev, &s_dev);
    if (e != ESP_OK) {
        spi_bus_free(EPD_SPI_HOST);
        s_bus_ready = false;
        return e;
    }
    out_io->cmd = io_cmd;
    out_io->data = io_data;
    out_io->busy_wait = io_busy_wait;
    out_io->power = io_power;
    out_io->reset = io_reset;
    out_io->delay_ms = io_delay;
    out_io->read_temp = io_read_temp;
    out_io->ctx = NULL;
    return ESP_OK;
}
esp_err_t epd_spi_prepare_power_off(void)
{
    esp_err_t first = ESP_OK;
    if (s_dev) {
        esp_err_t e = spi_bus_remove_device(s_dev);
        s_dev = NULL;
        if (e != ESP_OK) { ESP_LOGE(TAG, "remove_device 失败: %s", esp_err_to_name(e)); first = e; }
    }
    if (s_bus_ready) {
        esp_err_t e = spi_bus_free(EPD_SPI_HOST);
        s_bus_ready = false;
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "bus_free 失败: %s", esp_err_to_name(e));
            if (first == ESP_OK) { first = e; }
        }
    }
    static const gpio_num_t park[] = {
        PIN_EPD_CS, PIN_EPD_DC, PIN_EPD_RST, PIN_EPD_SCK, PIN_EPD_MOSI,
    };
    for (size_t i = 0; i < sizeof park / sizeof park[0]; i++) {
        gpio_config_t hiz = {
            .pin_bit_mask = 1ULL << park[i],
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        esp_err_t e = gpio_config(&hiz);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "park GPIO%d 失败: %s", (int)park[i], esp_err_to_name(e));
            if (first == ESP_OK) { first = e; }
        }
    }
    return first;
}
void epd_spi_deinit(void)
{
    if (s_dev) { spi_bus_remove_device(s_dev); s_dev = NULL; }
    if (s_bus_ready) { spi_bus_free(EPD_SPI_HOST); s_bus_ready = false; }
    gpio_set_level(PIN_EPD_CS, 1);
    gpio_set_level(PIN_EPD_SCK, 0);
}
int epd_spi_busy_level(void) { return gpio_get_level(PIN_EPD_BUSY); }
