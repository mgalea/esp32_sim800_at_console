set(srcs "src/cmd_system.c"
                    )

idf_component_register(SRCS "${srcs}"
                    INCLUDE_DIRS include
                    REQUIRES console spi_flash)