idf_component_register(
    SRCS "battery_dashboard.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES battery_database web_interface esp_http_server espressif__cjson
    EMBED_FILES "assets/dashboard.html" "assets/dashboard.js" "assets/chart.umd.js"
)
