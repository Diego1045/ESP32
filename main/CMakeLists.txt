idf_component_register(SRCS "main.c" "storage.c" "security.c" "access.c" "access_log.c" "actuator.c" "console_cli.c"
                    INCLUDE_DIRS "."
                    PRIV_INCLUDE_DIRS "../managed_components/abobija__rc522/internal"
                    REQUIRES abobija__rc522 rc522_diag nvs_flash esp_driver_gpio esp_driver_spi)
