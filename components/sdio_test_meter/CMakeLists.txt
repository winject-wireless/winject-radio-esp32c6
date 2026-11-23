idf_component_register(SRCS "sdio_test_meter.c" "sdio_test_axi.c"
                       INCLUDE_DIRS "include"
                       REQUIRES sdio_msg
                       PRIV_REQUIRES esp_timer esp_mm hal efuse heap freertos log)
