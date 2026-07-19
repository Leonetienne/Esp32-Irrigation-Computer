idf_component_register(SRCS "main.cpp"
                        INCLUDE_DIRS "."
                        REQUIRES nvs_flash esp_wifi esp_netif esp_event mqtt esp_driver_gpio esp_timer)
