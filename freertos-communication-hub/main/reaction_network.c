#include "reaction_network.h"

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_websocket_client.h"
#include "freertos/event_groups.h"
#include "network_config.h"
#include "nvs_flash.h"

#include <inttypes.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIFI_CONNECTED_BIT BIT0
#define NETWORK_RECONNECT_DELAY_MS 2000
#define NETWORK_SEND_TIMEOUT_MS 1000
#define NETWORK_INCOMING_QUEUE_TIMEOUT_MS 10
#define NETWORK_WEBSOCKET_BUFFER_SIZE 512

static const char *TAG = "NETWORK";
static EventGroupHandle_t wifi_event_group = NULL;
static reaction_network_config_t network_config;
static bool websocket_connected = false;

static esp_err_t initialize_nvs(void)
{
    esp_err_t err = nvs_flash_init();

    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "erase NVS failed");
        err = nvs_flash_init();
    }

    return err;
}

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    (void)arg;
    (void)event_data;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "Wi-Fi disconnected, reconnecting");
        esp_wifi_connect();
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Wi-Fi connected, IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t wifi_station_init(void)
{
    wifi_event_group = xEventGroupCreate();
    if (wifi_event_group == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    ESP_RETURN_ON_ERROR(initialize_nvs(), TAG, "NVS init failed");
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init failed");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop init failed");
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_config), TAG, "Wi-Fi init failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL),
                        TAG,
                        "Wi-Fi event handler register failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL),
                        TAG,
                        "IP event handler register failed");

    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.sta.ssid, REACTION_WIFI_SSID, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, REACTION_WIFI_PASSWORD, sizeof(wifi_config.sta.password));

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set Wi-Fi mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), TAG, "set Wi-Fi config failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start Wi-Fi failed");

    return ESP_OK;
}

static bool extract_json_type(const char *message, char *type, size_t type_size)
{
    const char *type_key = strstr(message, "\"type\"");

    if (type_key == NULL)
    {
        return false;
    }

    const char *colon = strchr(type_key, ':');
    if (colon == NULL)
    {
        return false;
    }

    const char *value = colon + 1;
    while (*value != '\0' && isspace((unsigned char)*value))
    {
        value++;
    }

    if (*value != '"')
    {
        return false;
    }

    value++;
    size_t index = 0;
    while (*value != '\0' && *value != '"' && index + 1 < type_size)
    {
        type[index++] = *value++;
    }

    type[index] = '\0';
    return index > 0 && *value == '"';
}

static bool extract_json_int32(const char *message, const char *field_name, int32_t *value)
{
    char key[32] = {0};
    snprintf(key, sizeof(key), "\"%s\"", field_name);

    const char *field = strstr(message, key);
    if (field == NULL)
    {
        return false;
    }

    const char *colon = strchr(field, ':');
    if (colon == NULL)
    {
        return false;
    }

    char *end = NULL;
    long parsed = strtol(colon + 1, &end, 10);
    if (end == colon + 1)
    {
        return false;
    }

    *value = (int32_t)parsed;
    return true;
}

static void send_game_event(game_event_type_t type, int32_t reaction_ms, uint32_t score)
{
    game_event_t event = {
        .type = type,
        .timestamp_ms = esp_timer_get_time() / 1000,
        .reaction_ms = reaction_ms,
        .score = score,
    };

    if (xQueueSend(network_config.game_event_queue,
                   &event,
                   pdMS_TO_TICKS(NETWORK_INCOMING_QUEUE_TIMEOUT_MS)) != pdTRUE)
    {
        ESP_LOGE(TAG, "Failed to send incoming command to game event queue");
    }
}

static void handle_incoming_text(const char *data, int data_len)
{
    char message[128] = {0};
    int copy_len = data_len;

    if (copy_len >= (int)sizeof(message))
    {
        copy_len = sizeof(message) - 1;
    }

    memcpy(message, data, copy_len);
    ESP_LOGI(TAG, "Incoming game command: %s", message);

    char type[32] = {0};
    if (!extract_json_type(message, type, sizeof(type)))
    {
        ESP_LOGI(TAG, "Ignoring message without JSON type field");
        return;
    }

    if (strcmp(type, "GAME_START") == 0)
    {
        send_game_event(GAME_EVENT_START, 0, 0);
    }
    else if (strcmp(type, "COLLISION") == 0)
    {
        send_game_event(GAME_EVENT_COLLISION, 0, 0);
    }
    else if (strcmp(type, "ATTEMPT_SUCCESS") == 0)
    {
        int32_t reaction_ms = 0;
        int32_t score = 0;
        extract_json_int32(message, "reaction_ms", &reaction_ms);
        extract_json_int32(message, "score", &score);
        send_game_event(GAME_EVENT_ATTEMPT_SUCCESS, reaction_ms, (uint32_t)score);
    }
    else if (strcmp(type, "ATTEMPT_FAILED") == 0)
    {
        send_game_event(GAME_EVENT_ATTEMPT_FAILED, 0, 0);
    }
    else if (strcmp(type, "GAME_WIN") == 0)
    {
        send_game_event(GAME_EVENT_WIN, 0, 0);
    }
    else if (strcmp(type, "GAME_OVER") == 0)
    {
        send_game_event(GAME_EVENT_OVER, 0, 0);
    }
    else
    {
        ESP_LOGI(TAG, "Ignoring unknown command");
    }
}

static void websocket_event_handler(void *handler_args,
                                    esp_event_base_t base,
                                    int32_t event_id,
                                    void *event_data)
{
    (void)handler_args;
    (void)base;

    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id)
    {
        case WEBSOCKET_EVENT_CONNECTED:
            websocket_connected = true;
            ESP_LOGI(TAG, "WebSocket connected");
            break;

        case WEBSOCKET_EVENT_DISCONNECTED:
            websocket_connected = false;
            ESP_LOGI(TAG, "WebSocket disconnected");
            break;

        case WEBSOCKET_EVENT_DATA:
            if (data->data_len > 0 && data->payload_offset == 0)
            {
                handle_incoming_text(data->data_ptr, data->data_len);
            }
            break;

        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGE(TAG, "WebSocket error");
            break;

        default:
            break;
    }
}

static esp_websocket_client_handle_t websocket_start(void)
{
    const esp_websocket_client_config_t websocket_config = {
        .uri = REACTION_WEBSOCKET_URI,
        .buffer_size = NETWORK_WEBSOCKET_BUFFER_SIZE,
        .reconnect_timeout_ms = NETWORK_RECONNECT_DELAY_MS,
    };

    esp_websocket_client_handle_t client = esp_websocket_client_init(&websocket_config);
    if (client == NULL)
    {
        ESP_LOGE(TAG, "Failed to create WebSocket client");
        return NULL;
    }

    ESP_ERROR_CHECK(esp_websocket_register_events(client,
                                                  WEBSOCKET_EVENT_ANY,
                                                  websocket_event_handler,
                                                  NULL));

    esp_err_t err = esp_websocket_client_start(client);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to start WebSocket client: %s", esp_err_to_name(err));
        esp_websocket_client_destroy(client);
        return NULL;
    }

    return client;
}

static void send_outgoing_event(esp_websocket_client_handle_t client,
                                const network_outgoing_event_t *event)
{
    if (!websocket_connected || !esp_websocket_client_is_connected(client))
    {
        ESP_LOGE(TAG, "WebSocket is not connected; dropping outgoing event");
        return;
    }

    if (event->type == NETWORK_EVENT_RFID_SCAN)
    {
        char message[128] = {0};
        int len = snprintf(message,
                           sizeof(message),
                           "{\"type\":\"RFID_SCAN\",\"timestamp_ms\":%" PRId64 "}",
                           event->timestamp_ms);

        if (len < 0 || len >= (int)sizeof(message))
        {
            ESP_LOGE(TAG, "Outgoing RFID message was too long");
            return;
        }

        int sent = esp_websocket_client_send_text(client,
                                                  message,
                                                  len,
                                                  pdMS_TO_TICKS(NETWORK_SEND_TIMEOUT_MS));
        if (sent < 0)
        {
            ESP_LOGE(TAG, "Failed to send outgoing RFID event");
            return;
        }

        ESP_LOGI(TAG, "Outgoing RFID event: %s", message);
    }
}

void reaction_network_task(void *pvParameters)
{
    reaction_network_config_t *config = (reaction_network_config_t *)pvParameters;

    if (config == NULL || config->game_event_queue == NULL || config->outgoing_queue == NULL)
    {
        ESP_LOGE(TAG, "Network task config is invalid");
        vTaskDelete(NULL);
        return;
    }

    network_config = *config;
    ESP_ERROR_CHECK(wifi_station_init());

    esp_websocket_client_handle_t client = NULL;
    network_outgoing_event_t outgoing_event;

    while (1)
    {
        xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

        if (client == NULL)
        {
            client = websocket_start();
            if (client == NULL)
            {
                vTaskDelay(pdMS_TO_TICKS(NETWORK_RECONNECT_DELAY_MS));
                continue;
            }
        }

        if (xQueueReceive(network_config.outgoing_queue, &outgoing_event, pdMS_TO_TICKS(500)) == pdTRUE)
        {
            send_outgoing_event(client, &outgoing_event);
        }

        if (!websocket_connected && !esp_websocket_client_is_connected(client))
        {
            vTaskDelay(pdMS_TO_TICKS(NETWORK_RECONNECT_DELAY_MS));
        }
    }
}
