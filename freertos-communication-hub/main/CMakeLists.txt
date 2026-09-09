idf_component_register(SRCS "freertos-communication-hub.c" "mfrc522.c" "ssd1306.c" "stepper_motor.c" "reaction_network.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_driver_gpio esp_driver_i2c esp_driver_spi esp_timer esp_wifi esp_event esp_netif nvs_flash esp_websocket_client)
