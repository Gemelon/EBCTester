idf_component_register(
    SRCS "mariadb_client.cpp" "mariadb_sync.cpp" "mariadb_sync_web.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES battery_database web_interface network_manager esp_http_server espressif__cjson
                  nvs_flash mbedtls lwip esp_timer
    EMBED_FILES "assets/mariadb.html" "assets/mariadb.js"
)
