idf_component_register(SRCS "ota_main.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_eth esp_netif esp_event esp_http_server
                                app_update esp_partition spi_flash nvs_flash
                                esp_hw_support esp_timer)
