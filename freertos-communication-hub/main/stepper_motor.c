#include "stepper_motor.h"

#include <string.h>

#define STEPPER_HALF_STEP_COUNT 8

static const uint8_t half_step_sequence[STEPPER_HALF_STEP_COUNT][4] = {
    {1, 0, 0, 0},
    {1, 1, 0, 0},
    {0, 1, 0, 0},
    {0, 1, 1, 0},
    {0, 0, 1, 0},
    {0, 0, 1, 1},
    {0, 0, 0, 1},
    {1, 0, 0, 1},
};

static void stepper_motor_write_outputs(stepper_motor_t *motor,
                                        uint8_t in1,
                                        uint8_t in2,
                                        uint8_t in3,
                                        uint8_t in4)
{
    gpio_set_level(motor->config.in1_io, in1);
    gpio_set_level(motor->config.in2_io, in2);
    gpio_set_level(motor->config.in3_io, in3);
    gpio_set_level(motor->config.in4_io, in4);
}

esp_err_t stepper_motor_init(stepper_motor_t *motor, const stepper_motor_config_t *config)
{
    if (motor == NULL || config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(motor, 0, sizeof(*motor));
    motor->config = *config;

    uint64_t pin_mask = (1ULL << config->in1_io) |
                        (1ULL << config->in2_io) |
                        (1ULL << config->in3_io) |
                        (1ULL << config->in4_io);

    gpio_config_t io_config = {
        .pin_bit_mask = pin_mask,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&io_config);
    if (err != ESP_OK) {
        return err;
    }

    stepper_motor_deenergize(motor);
    return ESP_OK;
}

void stepper_motor_write_half_step(stepper_motor_t *motor, uint8_t step_index)
{
    const uint8_t *step = half_step_sequence[step_index % STEPPER_HALF_STEP_COUNT];
    stepper_motor_write_outputs(motor, step[0], step[1], step[2], step[3]);
}

void stepper_motor_deenergize(stepper_motor_t *motor)
{
    stepper_motor_write_outputs(motor, 0, 0, 0, 0);
}
