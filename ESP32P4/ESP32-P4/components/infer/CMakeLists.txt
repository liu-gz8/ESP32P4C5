idf_component_register(SRCS "infer.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES espressif__pedestrian_detect espressif__esp-dl esp_timer)
