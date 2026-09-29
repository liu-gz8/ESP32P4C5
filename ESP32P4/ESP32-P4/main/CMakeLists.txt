idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES cam infer esp_timer heap esp_lcd_st7102 disp)
