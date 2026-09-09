#pragma once

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MFRC522_MAX_UID_SIZE 10

typedef struct {
    spi_host_device_t host;
    gpio_num_t miso_io;
    gpio_num_t mosi_io;
    gpio_num_t sclk_io;
    gpio_num_t cs_io;
    gpio_num_t rst_io;
} mfrc522_config_t;

typedef struct {
    uint8_t bytes[MFRC522_MAX_UID_SIZE];
    size_t size;
} mfrc522_uid_t;

typedef struct {
    spi_device_handle_t spi;
    gpio_num_t rst_io;
} mfrc522_t;

esp_err_t mfrc522_init(mfrc522_t *dev, const mfrc522_config_t *config);
bool mfrc522_is_card_present(mfrc522_t *dev);
esp_err_t mfrc522_read_card_uid(mfrc522_t *dev, mfrc522_uid_t *uid);
