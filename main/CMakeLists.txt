idf_component_register(SRCS "main.cpp" "config_store.cpp" "net_setup.cpp" "web_server.cpp" "leds.cpp"
                        INCLUDE_DIRS "."
                        EMBED_TXTFILES "web/setup.html" "web/index.html" "web/settings.html" "web/advanced.html" "web/style.css"
                        REQUIRES nvs_flash esp_wifi esp_netif esp_event mqtt esp_driver_gpio esp_timer esp_http_server mdns lwip)
