idf_component_register(SRCS "elotto_ota.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_http_server app_update esp_partition nvs_flash
                                esp_app_format)
