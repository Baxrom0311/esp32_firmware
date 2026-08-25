idf_component_register(
    SRCS "main.c" "wifi_manager.c" "nvs_storage.c" "mqtt_client.c"
         "bell_controller.c" "schedule_manager.c" "ota_manager.c"
         "device_registration.c" "status_reporter.c" "holiday_manager.c"
         "rtc_driver.c" "ap_provisioning.c"
         "captive_dns.c" "rtc_diagnostics.c"
         "entitlement_manager.c"
    INCLUDE_DIRS "../include" "."
    REQUIRES nvs_flash esp_wifi esp_netif esp_event driver mqtt
             esp_http_client esp_https_ota json app_update esp_timer
             bt esp_http_server
             mbedtls
)
