idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        nvs_flash
        esp_timer
        freertos
)
