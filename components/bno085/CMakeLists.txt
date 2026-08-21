idf_component_register(SRCS "bno085.c"
                        INCLUDE_DIRS "."
                        REQUIRES driver
                        PRIV_REQUIRES sh2 esp_timer log)
