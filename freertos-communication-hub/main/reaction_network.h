#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "game_events.h"

#include <stdint.h>

typedef enum {
    NETWORK_EVENT_RFID_SCAN
} network_event_type_t;

typedef struct {
    network_event_type_t type;
    int64_t timestamp_ms;
} network_outgoing_event_t;

typedef struct {
    QueueHandle_t game_event_queue;
    QueueHandle_t outgoing_queue;
} reaction_network_config_t;

void reaction_network_task(void *pvParameters);
