idf_component_register(
    SRCS "main.c" "pa_layout.c" "pa_cover.c" "pa_tail.c" "pa_import.c" "pa_protocol.c" "${PROJECT_DIR}/assets/fonts/pa_font_16.c"
    INCLUDE_DIRS "."
    REQUIRES passport_anki bsp nvs_flash esp_timer esp_driver_gpio freertos lvgl esp_lvgl_port
    PRIV_REQUIRES esp_partition launcher_contract bootloader_support app_update
)
