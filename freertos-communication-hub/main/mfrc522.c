#include "mfrc522.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"

#include <string.h>

#define MFRC522_SPI_CLOCK_HZ 1000000

#define MFRC522_REG_COMMAND 0x01
#define MFRC522_REG_COM_IRQ 0x04
#define MFRC522_REG_ERROR 0x06
#define MFRC522_REG_FIFO_DATA 0x09
#define MFRC522_REG_FIFO_LEVEL 0x0A
#define MFRC522_REG_CONTROL 0x0C
#define MFRC522_REG_BIT_FRAMING 0x0D
#define MFRC522_REG_MODE 0x11
#define MFRC522_REG_TX_CONTROL 0x14
#define MFRC522_REG_TX_ASK 0x15
#define MFRC522_REG_T_MODE 0x2A
#define MFRC522_REG_T_PRESCALER 0x2B
#define MFRC522_REG_T_RELOAD_H 0x2C
#define MFRC522_REG_T_RELOAD_L 0x2D

#define MFRC522_CMD_IDLE 0x00
#define MFRC522_CMD_TRANSCEIVE 0x0C
#define MFRC522_CMD_SOFT_RESET 0x0F

#define PICC_CMD_REQA 0x26
#define PICC_CMD_ANTICOLL_CL1 0x93

#define MFRC522_IRQ_RX_OR_IDLE 0x30
#define MFRC522_START_SEND 0x80
#define MFRC522_FIFO_FLUSH 0x80
#define MFRC522_ERROR_MASK 0x13
#define MFRC522_TRANSCEIVE_TIMEOUT_MS 25

static esp_err_t mfrc522_write_reg(mfrc522_t *dev, uint8_t reg, uint8_t value)
{
    uint8_t tx_data[2] = {
        (uint8_t)((reg << 1) & 0x7E),
        value,
    };

    spi_transaction_t transaction = {
        .length = 16,
        .tx_buffer = tx_data,
    };

    return spi_device_transmit(dev->spi, &transaction);
}

static esp_err_t mfrc522_read_reg(mfrc522_t *dev, uint8_t reg, uint8_t *value)
{
    uint8_t tx_data[2] = {
        (uint8_t)(((reg << 1) & 0x7E) | 0x80),
        0x00,
    };
    uint8_t rx_data[2] = {0};

    spi_transaction_t transaction = {
        .length = 16,
        .tx_buffer = tx_data,
        .rx_buffer = rx_data,
    };

    esp_err_t err = spi_device_transmit(dev->spi, &transaction);
    if (err != ESP_OK) {
        return err;
    }

    *value = rx_data[1];
    return ESP_OK;
}

static esp_err_t mfrc522_set_bit_mask(mfrc522_t *dev, uint8_t reg, uint8_t mask)
{
    uint8_t value = 0;
    esp_err_t err = mfrc522_read_reg(dev, reg, &value);
    if (err != ESP_OK) {
        return err;
    }

    return mfrc522_write_reg(dev, reg, value | mask);
}

static esp_err_t mfrc522_clear_bit_mask(mfrc522_t *dev, uint8_t reg, uint8_t mask)
{
    uint8_t value = 0;
    esp_err_t err = mfrc522_read_reg(dev, reg, &value);
    if (err != ESP_OK) {
        return err;
    }

    return mfrc522_write_reg(dev, reg, value & (uint8_t)(~mask));
}

static esp_err_t mfrc522_antenna_on(mfrc522_t *dev)
{
    uint8_t value = 0;
    esp_err_t err = mfrc522_read_reg(dev, MFRC522_REG_TX_CONTROL, &value);
    if (err != ESP_OK) {
        return err;
    }

    if ((value & 0x03) != 0x03) {
        return mfrc522_set_bit_mask(dev, MFRC522_REG_TX_CONTROL, 0x03);
    }

    return ESP_OK;
}

static esp_err_t mfrc522_transceive(mfrc522_t *dev,
                                    const uint8_t *send_data,
                                    size_t send_len,
                                    uint8_t valid_bits,
                                    uint8_t *back_data,
                                    size_t *back_len)
{
    ESP_RETURN_ON_ERROR(mfrc522_write_reg(dev, MFRC522_REG_COMMAND, MFRC522_CMD_IDLE), "MFRC522", "idle failed");
    ESP_RETURN_ON_ERROR(mfrc522_write_reg(dev, MFRC522_REG_COM_IRQ, 0x7F), "MFRC522", "clear irq failed");
    ESP_RETURN_ON_ERROR(mfrc522_write_reg(dev, MFRC522_REG_FIFO_LEVEL, MFRC522_FIFO_FLUSH), "MFRC522", "flush fifo failed");

    for (size_t i = 0; i < send_len; i++) {
        ESP_RETURN_ON_ERROR(mfrc522_write_reg(dev, MFRC522_REG_FIFO_DATA, send_data[i]), "MFRC522", "write fifo failed");
    }

    ESP_RETURN_ON_ERROR(mfrc522_write_reg(dev, MFRC522_REG_BIT_FRAMING, valid_bits), "MFRC522", "set bit framing failed");
    ESP_RETURN_ON_ERROR(mfrc522_write_reg(dev, MFRC522_REG_COMMAND, MFRC522_CMD_TRANSCEIVE), "MFRC522", "transceive failed");
    ESP_RETURN_ON_ERROR(mfrc522_set_bit_mask(dev, MFRC522_REG_BIT_FRAMING, MFRC522_START_SEND), "MFRC522", "start send failed");

    uint8_t irq = 0;
    for (int i = 0; i < MFRC522_TRANSCEIVE_TIMEOUT_MS; i++) {
        ESP_RETURN_ON_ERROR(mfrc522_read_reg(dev, MFRC522_REG_COM_IRQ, &irq), "MFRC522", "read irq failed");
        if ((irq & MFRC522_IRQ_RX_OR_IDLE) != 0) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    ESP_RETURN_ON_ERROR(mfrc522_clear_bit_mask(dev, MFRC522_REG_BIT_FRAMING, MFRC522_START_SEND), "MFRC522", "stop send failed");

    if ((irq & MFRC522_IRQ_RX_OR_IDLE) == 0) {
        return ESP_ERR_TIMEOUT;
    }

    uint8_t error = 0;
    ESP_RETURN_ON_ERROR(mfrc522_read_reg(dev, MFRC522_REG_ERROR, &error), "MFRC522", "read error failed");
    if ((error & MFRC522_ERROR_MASK) != 0) {
        return ESP_FAIL;
    }

    uint8_t fifo_level = 0;
    ESP_RETURN_ON_ERROR(mfrc522_read_reg(dev, MFRC522_REG_FIFO_LEVEL, &fifo_level), "MFRC522", "read fifo level failed");

    uint8_t control = 0;
    ESP_RETURN_ON_ERROR(mfrc522_read_reg(dev, MFRC522_REG_CONTROL, &control), "MFRC522", "read control failed");
    uint8_t last_bits = control & 0x07;

    if (last_bits != 0) {
        *back_len = ((fifo_level - 1) * 8U) + last_bits;
    } else {
        *back_len = fifo_level * 8U;
    }

    size_t bytes_to_read = fifo_level;
    if (bytes_to_read > MFRC522_MAX_UID_SIZE) {
        bytes_to_read = MFRC522_MAX_UID_SIZE;
    }

    for (size_t i = 0; i < bytes_to_read; i++) {
        ESP_RETURN_ON_ERROR(mfrc522_read_reg(dev, MFRC522_REG_FIFO_DATA, &back_data[i]), "MFRC522", "read fifo failed");
    }

    return ESP_OK;
}

esp_err_t mfrc522_init(mfrc522_t *dev, const mfrc522_config_t *config)
{
    if (dev == NULL || config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(dev, 0, sizeof(*dev));
    dev->rst_io = config->rst_io;

    spi_bus_config_t bus_config = {
        .mosi_io_num = config->mosi_io,
        .miso_io_num = config->miso_io,
        .sclk_io_num = config->sclk_io,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 32,
    };

    spi_device_interface_config_t device_config = {
        .clock_speed_hz = MFRC522_SPI_CLOCK_HZ,
        .mode = 0,
        .spics_io_num = config->cs_io,
        .queue_size = 1,
    };

    ESP_RETURN_ON_ERROR(spi_bus_initialize(config->host, &bus_config, SPI_DMA_DISABLED), "MFRC522", "spi bus init failed");
    ESP_RETURN_ON_ERROR(spi_bus_add_device(config->host, &device_config, &dev->spi), "MFRC522", "spi add device failed");

    gpio_config_t rst_config = {
        .pin_bit_mask = 1ULL << config->rst_io,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&rst_config), "MFRC522", "rst gpio config failed");

    gpio_set_level(config->rst_io, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(config->rst_io, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    ESP_RETURN_ON_ERROR(mfrc522_write_reg(dev, MFRC522_REG_COMMAND, MFRC522_CMD_SOFT_RESET), "MFRC522", "soft reset failed");
    vTaskDelay(pdMS_TO_TICKS(50));

    ESP_RETURN_ON_ERROR(mfrc522_write_reg(dev, MFRC522_REG_T_MODE, 0x8D), "MFRC522", "timer mode failed");
    ESP_RETURN_ON_ERROR(mfrc522_write_reg(dev, MFRC522_REG_T_PRESCALER, 0x3E), "MFRC522", "timer prescaler failed");
    ESP_RETURN_ON_ERROR(mfrc522_write_reg(dev, MFRC522_REG_T_RELOAD_L, 30), "MFRC522", "timer reload low failed");
    ESP_RETURN_ON_ERROR(mfrc522_write_reg(dev, MFRC522_REG_T_RELOAD_H, 0), "MFRC522", "timer reload high failed");
    ESP_RETURN_ON_ERROR(mfrc522_write_reg(dev, MFRC522_REG_TX_ASK, 0x40), "MFRC522", "tx ask failed");
    ESP_RETURN_ON_ERROR(mfrc522_write_reg(dev, MFRC522_REG_MODE, 0x3D), "MFRC522", "mode failed");
    ESP_RETURN_ON_ERROR(mfrc522_antenna_on(dev), "MFRC522", "antenna on failed");

    return ESP_OK;
}

bool mfrc522_is_card_present(mfrc522_t *dev)
{
    uint8_t request = PICC_CMD_REQA;
    uint8_t atqa[2] = {0};
    size_t atqa_bits = 0;

    esp_err_t err = mfrc522_transceive(dev, &request, 1, 7, atqa, &atqa_bits);
    return err == ESP_OK && atqa_bits == 16;
}

esp_err_t mfrc522_read_card_uid(mfrc522_t *dev, mfrc522_uid_t *uid)
{
    if (dev == NULL || uid == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t anticoll[] = {PICC_CMD_ANTICOLL_CL1, 0x20};
    uint8_t response[MFRC522_MAX_UID_SIZE] = {0};
    size_t response_bits = 0;

    esp_err_t err = mfrc522_transceive(dev, anticoll, sizeof(anticoll), 0, response, &response_bits);
    if (err != ESP_OK) {
        return err;
    }

    if (response_bits != 40) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint8_t bcc = response[0] ^ response[1] ^ response[2] ^ response[3];
    if (bcc != response[4]) {
        return ESP_ERR_INVALID_CRC;
    }

    memcpy(uid->bytes, response, 4);
    uid->size = 4;

    return ESP_OK;
}
