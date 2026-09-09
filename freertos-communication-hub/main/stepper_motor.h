#pragma once

#include "driver/gpio.h"
#include "esp_err.h"

#include <stdint.h>

typedef struct {
    gpio_num_t in1_io;
    gpio_num_t in2_io;
    gpio_num_t in3_io;
    gpio_num_t in4_io;
} stepper_motor_config_t;

typedef struct {
    stepper_motor_config_t config;
} stepper_motor_t;

esp_err_t stepper_motor_init(stepper_motor_t *motor, const stepper_motor_config_t *config);
void stepper_motor_write_half_step(stepper_motor_t *motor, uint8_t step_index);
void stepper_motor_deenergize(stepper_motor_t *motor);
