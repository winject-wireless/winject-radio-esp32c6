idf_component_register(SRCS "sdio_link_slave.c"
                       INCLUDE_DIRS "include"
                       REQUIRES sdio_msg
                       PRIV_REQUIRES esp_driver_sdio freertos log heap)
