idf_component_register(
    SRCS "battery_database.cpp" "battery_demo_data.cpp"
    INCLUDE_DIRS "include"
    REQUIRES sqlite3
    PRIV_REQUIRES sd_card_manager heap esp_app_format
    EMBED_TXTFILES "schema.sql" "resistance_schema.sql"
)
