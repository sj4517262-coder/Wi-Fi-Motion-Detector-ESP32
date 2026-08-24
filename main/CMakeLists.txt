idf_component_register(
    SRCS
        "app_main.c"
        "wifi_conn.c"
        "csi_capture.c"
        "traffic_gen.c"
        "motion.c"
        "web_server.c"
        "led.c"
    INCLUDE_DIRS "."
    # The dashboard page is baked into the firmware image, so there is no
    # filesystem partition to build, flash or keep in sync.
    EMBED_FILES "www/dashboard.html"
    REQUIRES
        esp_wifi
        esp_netif
        esp_event
        esp_http_server
        nvs_flash
        esp_timer
        driver
        lwip
)
