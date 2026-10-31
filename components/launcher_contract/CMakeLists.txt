idf_component_register(
    SRCS "launcher_contract.c"
    INCLUDE_DIRS "include"
    REQUIRES app_update bootloader_support esp_partition esp_system log
)
