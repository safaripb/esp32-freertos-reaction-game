#include "ssd1306.h"

#include "esp_check.h"

#include <stddef.h>
#include <string.h>

#define SSD1306_I2C_SPEED_HZ 400000
#define SSD1306_I2C_TIMEOUT_MS 100
#define SSD1306_CONTROL_COMMAND 0x00
#define SSD1306_CONTROL_DATA 0x40
#define SSD1306_PAGE_COUNT 8
#define SSD1306_DATA_CHUNK_SIZE 16

static const char *TAG = "SSD1306";

static esp_err_t ssd1306_write_command(ssd1306_t *display, uint8_t command)
{
    uint8_t data[] = {SSD1306_CONTROL_COMMAND, command};
    return i2c_master_transmit(display->dev, data, sizeof(data), SSD1306_I2C_TIMEOUT_MS);
}

static esp_err_t ssd1306_write_commands(ssd1306_t *display, const uint8_t *commands, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        ESP_RETURN_ON_ERROR(ssd1306_write_command(display, commands[i]), TAG, "command write failed");
    }

    return ESP_OK;
}

static esp_err_t ssd1306_write_data(ssd1306_t *display, const uint8_t *data, size_t count)
{
    uint8_t packet[SSD1306_DATA_CHUNK_SIZE + 1] = {SSD1306_CONTROL_DATA};
    size_t sent = 0;

    while (sent < count) {
        size_t chunk = count - sent;
        if (chunk > SSD1306_DATA_CHUNK_SIZE) {
            chunk = SSD1306_DATA_CHUNK_SIZE;
        }

        memcpy(&packet[1], &data[sent], chunk);
        ESP_RETURN_ON_ERROR(i2c_master_transmit(display->dev, packet, chunk + 1, SSD1306_I2C_TIMEOUT_MS),
                            TAG, "data write failed");
        sent += chunk;
    }

    return ESP_OK;
}

static void ssd1306_get_glyph(char c, uint8_t glyph[5])
{
    memset(glyph, 0, 5);

    switch (c) {
        case '!': {
            const uint8_t v[] = {0x00, 0x00, 0x5F, 0x00, 0x00};
            memcpy(glyph, v, 5);
            break;
        }
        case '.': {
            const uint8_t v[] = {0x00, 0x60, 0x60, 0x00, 0x00};
            memcpy(glyph, v, 5);
            break;
        }
        case 'A': {
            const uint8_t v[] = {0x7E, 0x11, 0x11, 0x11, 0x7E};
            memcpy(glyph, v, 5);
            break;
        }
        case 'C': {
            const uint8_t v[] = {0x3E, 0x41, 0x41, 0x41, 0x22};
            memcpy(glyph, v, 5);
            break;
        }
        case 'D': {
            const uint8_t v[] = {0x7F, 0x41, 0x41, 0x22, 0x1C};
            memcpy(glyph, v, 5);
            break;
        }
        case 'E': {
            const uint8_t v[] = {0x7F, 0x49, 0x49, 0x49, 0x41};
            memcpy(glyph, v, 5);
            break;
        }
        case 'F': {
            const uint8_t v[] = {0x7F, 0x09, 0x09, 0x09, 0x01};
            memcpy(glyph, v, 5);
            break;
        }
        case 'G': {
            const uint8_t v[] = {0x3E, 0x41, 0x49, 0x49, 0x7A};
            memcpy(glyph, v, 5);
            break;
        }
        case 'I': {
            const uint8_t v[] = {0x00, 0x41, 0x7F, 0x41, 0x00};
            memcpy(glyph, v, 5);
            break;
        }
        case 'L': {
            const uint8_t v[] = {0x7F, 0x40, 0x40, 0x40, 0x40};
            memcpy(glyph, v, 5);
            break;
        }
        case 'N': {
            const uint8_t v[] = {0x7F, 0x02, 0x04, 0x08, 0x7F};
            memcpy(glyph, v, 5);
            break;
        }
        case 'S': {
            const uint8_t v[] = {0x46, 0x49, 0x49, 0x49, 0x31};
            memcpy(glyph, v, 5);
            break;
        }
        case 'T': {
            const uint8_t v[] = {0x01, 0x01, 0x7F, 0x01, 0x01};
            memcpy(glyph, v, 5);
            break;
        }
        case 'U': {
            const uint8_t v[] = {0x3F, 0x40, 0x40, 0x40, 0x3F};
            memcpy(glyph, v, 5);
            break;
        }
        case 'W': {
            const uint8_t v[] = {0x7F, 0x20, 0x18, 0x20, 0x7F};
            memcpy(glyph, v, 5);
            break;
        }
        default:
            break;
    }
}

esp_err_t ssd1306_init(ssd1306_t *display, const ssd1306_config_t *config)
{
    if (display == NULL || config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(display, 0, sizeof(*display));

    i2c_master_bus_config_t bus_config = {
        .i2c_port = config->port,
        .sda_io_num = config->sda_io,
        .scl_io_num = config->scl_io,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = config->address,
        .scl_speed_hz = SSD1306_I2C_SPEED_HZ,
    };

    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_config, &display->bus), TAG, "i2c bus init failed");
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(display->bus, &device_config, &display->dev),
                        TAG, "i2c device add failed");

    const uint8_t init_commands[] = {
        0xAE, 0x20, 0x00, 0xB0, 0xC8, 0x00, 0x10, 0x40,
        0x81, 0x7F, 0xA1, 0xA6, 0xA8, 0x3F, 0xA4, 0xD3,
        0x00, 0xD5, 0x80, 0xD9, 0xF1, 0xDA, 0x12, 0xDB,
        0x40, 0x8D, 0x14, 0xAF,
    };

    ESP_RETURN_ON_ERROR(ssd1306_write_commands(display, init_commands, sizeof(init_commands)),
                        TAG, "display init failed");
    ESP_RETURN_ON_ERROR(ssd1306_clear(display), TAG, "clear failed");
    ESP_RETURN_ON_ERROR(ssd1306_show(display), TAG, "initial show failed");

    return ESP_OK;
}

esp_err_t ssd1306_clear(ssd1306_t *display)
{
    if (display == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(display->buffer, 0, sizeof(display->buffer));
    return ESP_OK;
}

void ssd1306_draw_text(ssd1306_t *display, uint8_t x, uint8_t page, const char *text)
{
    if (display == NULL || text == NULL || page >= SSD1306_PAGE_COUNT) {
        return;
    }

    size_t index = (page * SSD1306_WIDTH) + x;

    while (*text != '\0' && index + 6 < SSD1306_BUFFER_SIZE) {
        uint8_t glyph[5] = {0};
        ssd1306_get_glyph(*text, glyph);

        for (size_t i = 0; i < 5 && index < SSD1306_BUFFER_SIZE; i++) {
            display->buffer[index++] = glyph[i];
        }

        if (index < SSD1306_BUFFER_SIZE) {
            display->buffer[index++] = 0x00;
        }

        text++;
    }
}

esp_err_t ssd1306_show(ssd1306_t *display)
{
    if (display == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t address_window[] = {
        0x21, 0x00, SSD1306_WIDTH - 1,
        0x22, 0x00, SSD1306_PAGE_COUNT - 1,
    };

    ESP_RETURN_ON_ERROR(ssd1306_write_commands(display, address_window, sizeof(address_window)),
                        TAG, "set address window failed");
    return ssd1306_write_data(display, display->buffer, sizeof(display->buffer));
}
