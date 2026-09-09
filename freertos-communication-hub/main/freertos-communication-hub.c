#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "game_events.h"
#include "mfrc522.h"
#include "reaction_network.h"
#include "ssd1306.h"
#include "stepper_motor.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

#define GAME_EVENT_QUEUE_LENGTH 10
#define MONITOR_TASK_STACK_SIZE 3072
#define RFID_TASK_STACK_SIZE 4096
#define OLED_TASK_STACK_SIZE 4096
#define STEPPER_TASK_STACK_SIZE 3072
#define NETWORK_TASK_STACK_SIZE 6144
#define GAME_LOGIC_TASK_STACK_SIZE 4096
#define MONITOR_TASK_PRIORITY 1
#define RFID_TASK_PRIORITY 2
#define OLED_TASK_PRIORITY 2
#define STEPPER_TASK_PRIORITY 2
#define NETWORK_TASK_PRIORITY 2
#define GAME_LOGIC_TASK_PRIORITY 3
#define MONITOR_TASK_CORE 0
#define RFID_TASK_CORE 0
#define OLED_TASK_CORE 1
#define STEPPER_TASK_CORE 1
#define NETWORK_TASK_CORE 0
#define GAME_LOGIC_TASK_CORE 1
#define RFID_POLL_INTERVAL_MS 40
#define RFID_RELEASE_POLLS 3
#define OLED_STATE_QUEUE_LENGTH 1
#define STEPPER_COMMAND_QUEUE_LENGTH 4
#define NETWORK_OUTGOING_QUEUE_LENGTH 8
#define OLED_ANIMATION_INTERVAL_MS 250
#define OLED_SUCCESS_DURATION_MS 1000
#define STEPPER_STEP_DELAY_MS 3

#define RFID_SPI_HOST SPI3_HOST
#define RFID_PIN_CS GPIO_NUM_5
#define RFID_PIN_SCLK GPIO_NUM_18
#define RFID_PIN_MOSI GPIO_NUM_23
#define RFID_PIN_MISO GPIO_NUM_19
#define RFID_PIN_RST GPIO_NUM_27

#define OLED_I2C_PORT I2C_NUM_0
#define OLED_I2C_ADDRESS 0x3C
#define OLED_PIN_SDA GPIO_NUM_21
#define OLED_PIN_SCL GPIO_NUM_22

#define STEPPER_PIN_IN1 GPIO_NUM_25
#define STEPPER_PIN_IN2 GPIO_NUM_26
#define STEPPER_PIN_IN3 GPIO_NUM_32
#define STEPPER_PIN_IN4 GPIO_NUM_33

typedef enum {
    OLED_WAITING,
    OLED_SUCCESS,
    OLED_FAILED,
    OLED_WIN,
    OLED_GAME_OVER
} oled_state_t;

typedef enum {
    STEPPER_START_CW,
    STEPPER_STOP
} stepper_command_t;

static const char *TAG = "SYSTEM";
static const char *GAME_TAG = "GAME";
static const char *RFID_TAG = "RFID";
static const char *OLED_TAG = "OLED";
static const char *STEPPER_TAG = "STEPPER";
static QueueHandle_t game_event_queue = NULL;
static QueueHandle_t oled_state_queue = NULL;
static QueueHandle_t stepper_command_queue = NULL;
static QueueHandle_t network_outgoing_queue = NULL;
static TaskHandle_t monitor_task_handle = NULL;
static TaskHandle_t rfid_task_handle = NULL;
static TaskHandle_t oled_task_handle = NULL;
static TaskHandle_t stepper_task_handle = NULL;
static TaskHandle_t network_task_handle = NULL;
static TaskHandle_t game_logic_task_handle = NULL;
static mfrc522_t rfid_reader;
static ssd1306_t oled_display;
static stepper_motor_t stepper_motor;
static reaction_network_config_t network_task_config;

static void monitor_task(void *pvParameters);
static void rfid_task(void *pvParameters);
static void oled_task(void *pvParameters);
static void stepper_task(void *pvParameters);
static void game_logic_task(void *pvParameters);
static void send_stepper_command(stepper_command_t command);
static void send_network_rfid_scan(int64_t timestamp_ms);
static esp_err_t oled_show_waiting(ssd1306_t *display, int frame);
static esp_err_t oled_show_success(ssd1306_t *display);
static esp_err_t oled_show_failed(ssd1306_t *display);
static esp_err_t oled_show_win(ssd1306_t *display);
static esp_err_t oled_show_game_over(ssd1306_t *display);

static void send_stepper_command(stepper_command_t command)
{
    if (stepper_command_queue == NULL)
    {
        ESP_LOGE(STEPPER_TAG, "Stepper command queue is not ready");
        return;
    }

    if (xQueueSend(stepper_command_queue, &command, pdMS_TO_TICKS(10)) != pdTRUE)
    {
        ESP_LOGE(STEPPER_TAG, "Failed to send stepper command");
    }
}

static void send_network_rfid_scan(int64_t timestamp_ms)
{
    if (network_outgoing_queue == NULL)
    {
        ESP_LOGE(GAME_TAG, "Network outgoing queue is not ready");
        return;
    }

    network_outgoing_event_t outgoing_event = {
        .type = NETWORK_EVENT_RFID_SCAN,
        .timestamp_ms = timestamp_ms,
    };

    if (xQueueSend(network_outgoing_queue, &outgoing_event, pdMS_TO_TICKS(10)) != pdTRUE)
    {
        ESP_LOGE(GAME_TAG, "Failed to send RFID scan to network task");
    }
}

static void game_logic_task(void *pvParameters)
{
    (void)pvParameters;

    game_event_t event;
    uint32_t score = 0;

    while (1)
    {
        if (xQueueReceive(game_event_queue, &event, portMAX_DELAY) == pdTRUE)
        {
            switch (event.type)
            {
                case GAME_EVENT_START:
                    ESP_LOGI(GAME_TAG, "GAME_EVENT_START at %" PRId64 " ms", event.timestamp_ms);
                    send_stepper_command(STEPPER_START_CW);
                    break;

                case GAME_EVENT_COLLISION:
                    ESP_LOGI(GAME_TAG, "GAME_EVENT_COLLISION at %" PRId64 " ms", event.timestamp_ms);
                    break;

                case GAME_EVENT_RFID_SCAN:
                    ESP_LOGI(GAME_TAG, "GAME_EVENT_RFID_SCAN at %" PRId64 " ms", event.timestamp_ms);
                    send_network_rfid_scan(event.timestamp_ms);
                    break;

                case GAME_EVENT_ATTEMPT_SUCCESS:
                    score = event.score;
                    ESP_LOGI(GAME_TAG,
                             "GAME_EVENT_ATTEMPT_SUCCESS at %" PRId64 " ms, reaction %" PRId32 " ms, score %" PRIu32,
                             event.timestamp_ms,
                             event.reaction_ms,
                             score);

                    if (oled_state_queue != NULL)
                    {
                        oled_state_t oled_state = OLED_SUCCESS;
                        xQueueOverwrite(oled_state_queue, &oled_state);
                    }
                    break;

                case GAME_EVENT_ATTEMPT_FAILED:
                    ESP_LOGI(GAME_TAG, "GAME_EVENT_ATTEMPT_FAILED at %" PRId64 " ms", event.timestamp_ms);

                    if (oled_state_queue != NULL)
                    {
                        oled_state_t oled_state = OLED_FAILED;
                        xQueueOverwrite(oled_state_queue, &oled_state);
                    }
                    break;

                case GAME_EVENT_WIN:
                    ESP_LOGI(GAME_TAG, "GAME_EVENT_WIN at %" PRId64 " ms", event.timestamp_ms);
                    send_stepper_command(STEPPER_STOP);

                    if (oled_state_queue != NULL)
                    {
                        oled_state_t oled_state = OLED_WIN;
                        xQueueOverwrite(oled_state_queue, &oled_state);
                    }
                    break;

                case GAME_EVENT_OVER:
                    ESP_LOGI(GAME_TAG, "GAME_EVENT_OVER at %" PRId64 " ms", event.timestamp_ms);
                    send_stepper_command(STEPPER_STOP);

                    if (oled_state_queue != NULL)
                    {
                        oled_state_t oled_state = OLED_GAME_OVER;
                        xQueueOverwrite(oled_state_queue, &oled_state);
                    }
                    break;

                default:
                    ESP_LOGI(GAME_TAG, "Unknown game event at %" PRId64 " ms", event.timestamp_ms);
                    break;
            }
        }
    }
}

static void stepper_task(void *pvParameters)
{
    (void)pvParameters;

    const stepper_motor_config_t motor_config = {
        .in1_io = STEPPER_PIN_IN1,
        .in2_io = STEPPER_PIN_IN2,
        .in3_io = STEPPER_PIN_IN3,
        .in4_io = STEPPER_PIN_IN4,
    };

    ESP_ERROR_CHECK(stepper_motor_init(&stepper_motor, &motor_config));
    ESP_LOGI(STEPPER_TAG, "Stepper initialized");

    stepper_command_t command;
    bool running = false;
    uint8_t step_index = 0;
    TickType_t step_delay_ticks = pdMS_TO_TICKS(STEPPER_STEP_DELAY_MS);

    if (step_delay_ticks == 0)
    {
        ESP_LOGI(STEPPER_TAG,
                 "Configured step delay %d ms is less than one RTOS tick; using 1 tick",
                 STEPPER_STEP_DELAY_MS);
        step_delay_ticks = 1;
    }

    while (1)
    {
        if (!running)
        {
            if (xQueueReceive(stepper_command_queue, &command, portMAX_DELAY) == pdTRUE)
            {
                if (command == STEPPER_START_CW)
                {
                    ESP_LOGI(STEPPER_TAG, "Motor start clockwise");
                    running = true;
                }
                else if (command == STEPPER_STOP)
                {
                    stepper_motor_deenergize(&stepper_motor);
                    ESP_LOGI(STEPPER_TAG, "Motor stop");
                }
            }
        }
        else
        {
            if (xQueueReceive(stepper_command_queue, &command, step_delay_ticks) == pdTRUE)
            {
                if (command == STEPPER_STOP)
                {
                    running = false;
                    stepper_motor_deenergize(&stepper_motor);
                    ESP_LOGI(STEPPER_TAG, "Motor stop");
                    continue;
                }
                else if (command == STEPPER_START_CW)
                {
                    ESP_LOGI(STEPPER_TAG, "Motor already running clockwise");
                }
            }
            else
            {
                stepper_motor_write_half_step(&stepper_motor, step_index);
                step_index = (step_index + 1) % 8;
            }
        }
    }
}

static esp_err_t oled_show_waiting(ssd1306_t *display, int frame)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(ssd1306_clear(display));
    ssd1306_draw_text(display, 43, 2, "WAITING");

    switch (frame % 4)
    {
        case 0:
            ssd1306_draw_text(display, 61, 4, ".");
            break;
        case 1:
            ssd1306_draw_text(display, 58, 4, "..");
            break;
        case 2:
            ssd1306_draw_text(display, 55, 4, "...");
            break;
        default:
            break;
    }

    return ssd1306_show(display);
}

static esp_err_t oled_show_success(ssd1306_t *display)
{
    ESP_RETURN_ON_ERROR(ssd1306_clear(display), OLED_TAG, "clear failed");
    ssd1306_draw_text(display, 40, 2, "SUCCESS!");
    ssd1306_draw_text(display, 39, 4, "CONGRATS");
    return ssd1306_show(display);
}

static esp_err_t oled_show_failed(ssd1306_t *display)
{
    ESP_RETURN_ON_ERROR(ssd1306_clear(display), OLED_TAG, "clear failed");
    ssd1306_draw_text(display, 46, 3, "FAILED");
    return ssd1306_show(display);
}

static esp_err_t oled_show_win(ssd1306_t *display)
{
    ESP_RETURN_ON_ERROR(ssd1306_clear(display), OLED_TAG, "clear failed");
    ssd1306_draw_text(display, 40, 3, "YOU WIN!");
    return ssd1306_show(display);
}

static esp_err_t oled_show_game_over(ssd1306_t *display)
{
    ESP_RETURN_ON_ERROR(ssd1306_clear(display), OLED_TAG, "clear failed");
    ssd1306_draw_text(display, 34, 3, "GAME OVER");
    return ssd1306_show(display);
}

static void oled_task(void *pvParameters)
{
    (void)pvParameters;

    const ssd1306_config_t oled_config = {
        .port = OLED_I2C_PORT,
        .sda_io = OLED_PIN_SDA,
        .scl_io = OLED_PIN_SCL,
        .address = OLED_I2C_ADDRESS,
    };

    ESP_ERROR_CHECK(ssd1306_init(&oled_display, &oled_config));
    ESP_LOGI(OLED_TAG, "SSD1306 initialized on SDA GPIO %d, SCL GPIO %d", OLED_PIN_SDA, OLED_PIN_SCL);

    oled_state_t state = OLED_WAITING;
    int animation_frame = 0;

    while (1)
    {
        oled_state_t next_state;

        switch (state)
        {
            case OLED_WAITING:
                ESP_ERROR_CHECK_WITHOUT_ABORT(oled_show_waiting(&oled_display, animation_frame));
                animation_frame++;

                if (xQueueReceive(oled_state_queue, &next_state, pdMS_TO_TICKS(OLED_ANIMATION_INTERVAL_MS)) == pdTRUE)
                {
                    state = next_state;
                }
                break;

            case OLED_SUCCESS:
                ESP_ERROR_CHECK_WITHOUT_ABORT(oled_show_success(&oled_display));

                if (xQueueReceive(oled_state_queue, &next_state, pdMS_TO_TICKS(OLED_SUCCESS_DURATION_MS)) == pdTRUE)
                {
                    state = next_state;
                }
                else
                {
                    state = OLED_WAITING;
                }
                break;

            case OLED_FAILED:
                ESP_ERROR_CHECK_WITHOUT_ABORT(oled_show_failed(&oled_display));

                if (xQueueReceive(oled_state_queue, &next_state, pdMS_TO_TICKS(OLED_SUCCESS_DURATION_MS)) == pdTRUE)
                {
                    state = next_state;
                }
                else
                {
                    state = OLED_WAITING;
                }
                break;

            case OLED_WIN:
                ESP_ERROR_CHECK_WITHOUT_ABORT(oled_show_win(&oled_display));

                if (xQueueReceive(oled_state_queue, &next_state, pdMS_TO_TICKS(OLED_SUCCESS_DURATION_MS)) == pdTRUE)
                {
                    state = next_state;
                }
                else
                {
                    state = OLED_WAITING;
                }
                break;

            case OLED_GAME_OVER:
                ESP_ERROR_CHECK_WITHOUT_ABORT(oled_show_game_over(&oled_display));

                if (xQueueReceive(oled_state_queue, &next_state, pdMS_TO_TICKS(OLED_SUCCESS_DURATION_MS)) == pdTRUE)
                {
                    state = next_state;
                }
                break;

            default:
                state = OLED_WAITING;
                break;
        }
    }
}

static void rfid_task(void *pvParameters)
{
    (void)pvParameters;

    bool card_latched = false;
    int release_count = 0;

    while (1)
    {
        if (mfrc522_is_card_present(&rfid_reader))
        {
            release_count = 0;

            if (!card_latched)
            {
                mfrc522_uid_t uid = {0};
                esp_err_t err = mfrc522_read_card_uid(&rfid_reader, &uid);
                card_latched = true;

                if (err == ESP_OK)
                {
                    char uid_text[(MFRC522_MAX_UID_SIZE * 3) + 1] = {0};
                    size_t offset = 0;

                    for (size_t i = 0; i < uid.size; i++)
                    {
                        offset += snprintf(uid_text + offset, sizeof(uid_text) - offset, "%02X%s",
                                           uid.bytes[i], (i + 1 < uid.size) ? ":" : "");
                    }

                    ESP_LOGI(RFID_TAG, "Card UID %s", uid_text);

                    game_event_t event = {
                        .type = GAME_EVENT_RFID_SCAN,
                        .timestamp_ms = esp_timer_get_time() / 1000,
                    };

                    if (xQueueSend(game_event_queue, &event, pdMS_TO_TICKS(10)) != pdTRUE)
                    {
                        ESP_LOGE(RFID_TAG, "Failed to send RFID scan event");
                    }
                }
                else
                {
                    ESP_LOGE(RFID_TAG, "Card detected, but UID read failed: %s", esp_err_to_name(err));
                }
            }
        }
        else if (card_latched)
        {
            release_count++;
            if (release_count >= RFID_RELEASE_POLLS)
            {
                card_latched = false;
                release_count = 0;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(RFID_POLL_INTERVAL_MS));
    }
}

static void monitor_task(void *pvParameters)
{
    (void)pvParameters;

    while (1)
    {
        ESP_LOGI(TAG, "Communication hub is running");

        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

void app_main(void)
{
    game_event_queue = xQueueCreate(GAME_EVENT_QUEUE_LENGTH, sizeof(game_event_t));

    if (game_event_queue == NULL)
    {
        ESP_LOGE(GAME_TAG, "Failed to create game event queue");
        return;
    }

    oled_state_queue = xQueueCreate(OLED_STATE_QUEUE_LENGTH, sizeof(oled_state_t));

    if (oled_state_queue == NULL)
    {
        ESP_LOGE(OLED_TAG, "Failed to create OLED state queue");
        return;
    }

    stepper_command_queue = xQueueCreate(STEPPER_COMMAND_QUEUE_LENGTH, sizeof(stepper_command_t));

    if (stepper_command_queue == NULL)
    {
        ESP_LOGE(STEPPER_TAG, "Failed to create stepper command queue");
        return;
    }

    network_outgoing_queue = xQueueCreate(NETWORK_OUTGOING_QUEUE_LENGTH, sizeof(network_outgoing_event_t));

    if (network_outgoing_queue == NULL)
    {
        ESP_LOGE(GAME_TAG, "Failed to create network outgoing queue");
        return;
    }

    const mfrc522_config_t rfid_config = {
        .host = RFID_SPI_HOST,
        .miso_io = RFID_PIN_MISO,
        .mosi_io = RFID_PIN_MOSI,
        .sclk_io = RFID_PIN_SCLK,
        .cs_io = RFID_PIN_CS,
        .rst_io = RFID_PIN_RST,
    };

    ESP_ERROR_CHECK(mfrc522_init(&rfid_reader, &rfid_config));

    BaseType_t monitor_created = xTaskCreatePinnedToCore(
        monitor_task,
        "monitor_task",
        MONITOR_TASK_STACK_SIZE,
        NULL,
        MONITOR_TASK_PRIORITY,
        &monitor_task_handle,
        MONITOR_TASK_CORE
    );

    if (monitor_created != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create monitor task");
        return;
    }

    BaseType_t stepper_created = xTaskCreatePinnedToCore(
        stepper_task,
        "stepper_task",
        STEPPER_TASK_STACK_SIZE,
        NULL,
        STEPPER_TASK_PRIORITY,
        &stepper_task_handle,
        STEPPER_TASK_CORE
    );

    if (stepper_created != pdPASS)
    {
        ESP_LOGE(STEPPER_TAG, "Failed to create stepper task");
        return;
    }

    network_task_config.game_event_queue = game_event_queue;
    network_task_config.outgoing_queue = network_outgoing_queue;

    BaseType_t network_created = xTaskCreatePinnedToCore(
        reaction_network_task,
        "network_task",
        NETWORK_TASK_STACK_SIZE,
        &network_task_config,
        NETWORK_TASK_PRIORITY,
        &network_task_handle,
        NETWORK_TASK_CORE
    );

    if (network_created != pdPASS)
    {
        ESP_LOGE(GAME_TAG, "Failed to create network task");
        return;
    }

    BaseType_t oled_created = xTaskCreatePinnedToCore(
        oled_task,
        "oled_task",
        OLED_TASK_STACK_SIZE,
        NULL,
        OLED_TASK_PRIORITY,
        &oled_task_handle,
        OLED_TASK_CORE
    );

    if (oled_created != pdPASS)
    {
        ESP_LOGE(OLED_TAG, "Failed to create OLED task");
        return;
    }

    BaseType_t rfid_created = xTaskCreatePinnedToCore(
        rfid_task,
        "rfid_task",
        RFID_TASK_STACK_SIZE,
        NULL,
        RFID_TASK_PRIORITY,
        &rfid_task_handle,
        RFID_TASK_CORE
    );

    if (rfid_created != pdPASS)
    {
        ESP_LOGE(RFID_TAG, "Failed to create RFID task");
        return;
    }

    BaseType_t game_logic_created = xTaskCreatePinnedToCore(
        game_logic_task,
        "game_logic_task",
        GAME_LOGIC_TASK_STACK_SIZE,
        NULL,
        GAME_LOGIC_TASK_PRIORITY,
        &game_logic_task_handle,
        GAME_LOGIC_TASK_CORE
    );

    if (game_logic_created != pdPASS)
    {
        ESP_LOGE(GAME_TAG, "Failed to create game logic task");
        return;
    }
}
