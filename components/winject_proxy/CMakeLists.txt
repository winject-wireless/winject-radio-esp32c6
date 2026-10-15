idf_component_register(SRCS "winject_proxy.c"
                       INCLUDE_DIRS "include"
                       REQUIRES sdio_link_host
                       PRIV_REQUIRES lwip freertos log)
