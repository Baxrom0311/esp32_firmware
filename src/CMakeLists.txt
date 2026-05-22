idf_component_register(
    SRCS "main.c" "wifi_manager.c" "nvs_storage.c" "mqtt_client.c"
         "bell_controller.c" "schedule_manager.c" "ota_manager.c"
         "device_registration.c" "status_reporter.c"
    INCLUDE_DIRS "../include" "."
    REQUIRES nvs_flash esp_wifi esp_netif esp_event driver mqtt
             esp_http_client esp_https_ota json app_update esp_timer
)
