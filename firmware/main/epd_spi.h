/* SSD2683 SPI/GPIO backend for epd_io_t (SPI3). BUSY wait has a timeout; power rail is not touched here. */
#ifndef EPD_SPI_H
#define EPD_SPI_H 
#include "driver/spi_master.h"
#include "epd_proto.h"
#include "esp_err.h"
#define EPD_SPI_HOST SPI3_HOST
esp_err_t epd_spi_init(epd_io_t *out_io);
void epd_spi_deinit(void);
int epd_spi_busy_level(void);
esp_err_t epd_spi_prepare_power_off(void);
void epd_spi_trace_reset(void);
void epd_spi_trace_dump(const char *title);
int epd_spi_trace_get(const char *label);
typedef void (*epd_pump_fn)(void *ctx);
void epd_spi_set_pump(epd_pump_fn fn, void *ctx);
#endif
