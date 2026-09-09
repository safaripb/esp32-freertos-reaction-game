#pragma once

#include <stdint.h>

typedef enum {
    GAME_EVENT_START,
    GAME_EVENT_COLLISION,
    GAME_EVENT_RFID_SCAN,
    GAME_EVENT_ATTEMPT_SUCCESS,
    GAME_EVENT_ATTEMPT_FAILED,
    GAME_EVENT_WIN,
    GAME_EVENT_OVER
} game_event_type_t;

typedef struct {
    game_event_type_t type;
    int64_t timestamp_ms;
    int32_t reaction_ms;
    uint32_t score;
} game_event_t;
