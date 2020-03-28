set(srcs "src/esp_modem.c"
        "src/esp_modem_dce_service"
        "src/sim800.c"
        "src/bg96.c"
        "src/cmd_modem.c")

idf_component_register(SRCS "${srcs}"
                    INCLUDE_DIRS include
                    REQUIRES driver mqtt console
                    )