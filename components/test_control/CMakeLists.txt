idf_component_register(
    SRCS "test_control.cpp" "test_control_web.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES ebc_link test_store resistance_test battery_database web_interface esp_http_server espressif__cjson
                  nvs_flash esp_timer
    EMBED_FILES "assets/battery.html" "assets/battery.js" "assets/manual.html" "assets/manual.js"
                "assets/auto.html" "assets/auto.js" "assets/resistance.html" "assets/resistance.js"
)
