#pragma once

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"

#include <stdint.h>

#define SSD1306_WIDTH 128
#define SSD1306_HEIGHT 64
#define SSD1306_BUFFER_SIZE (SSD1306_WIDTH * SSD1306_HEIGHT / 8)

typedef struct {
    i2c_port_num_t port;
    gpio_num_t sda_io;
    gpio_num_t scl_io;
    uint8_t address;
} ssd1306_config_t;

typedef struct {
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t dev;
    uint8_t buffer[SSD1306_BUFFER_SIZE];
} ssd1306_t;

esp_err_t ssd1306_init(ssd1306_t *display, const ssd1306_config_t *config);
esp_err_t ssd1306_clear(ssd1306_t *display);
void ssd1306_draw_text(ssd1306_t *display, uint8_t x, uint8_t page, const char *text);
esp_err_t ssd1306_show(ssd1306_t *display);
