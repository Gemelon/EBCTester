idf_component_register(
    SRCS "test_store.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES battery_database esp_app_format esp_timer
)
