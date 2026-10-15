idf_component_register(
    SRCS
        "config_types.cpp"
        "main.cpp"
        "sdio_console.cpp"
        "sdio_ota_server.cpp"
        "settings.cpp"
        "settings_blob.cpp"
        "uart_console.cpp"
        "diag/test_run.cpp"
        "diag/wifi_tx_test.cpp"
        "endpoint/sdio_endpoint.cpp"
        "mplane/device_backend_esp.cpp"
        "mplane/mplane_args.cpp"
        "mplane/mplane_commands.cpp"
        "mplane/mplane_reply.cpp"
        "mplane/mplane_req_id.cpp"
        "mplane/mplane_version.cpp"
        "mplane/radio_backend_esp.cpp"
        "mplane/test_backend_esp.cpp"
        "radio/fcs.cpp"
        "radio/frame.cpp"
        "radio/packet.cpp"
        "radio/wifi.cpp"
        "radio/wifi_rx.cpp"
        "radio/wifi_tx.cpp"
    INCLUDE_DIRS
        "."
        "diag"
        "endpoint"
        "mplane"
        "radio"
    REQUIRES
        sdio_link_slave
        app_update
        esp_driver_gpio
        esp_driver_uart
        vfs
        esp_event
        esp_netif
        esp_rom
        esp_timer
        esp_wifi
        nvs_flash
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
# wifi_tx.cpp overrides ieee80211_raw_frame_sanity_check from libnet80211 so
# any frame type can be injected; the first (our) definition wins.
target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,-z,muldefs")
