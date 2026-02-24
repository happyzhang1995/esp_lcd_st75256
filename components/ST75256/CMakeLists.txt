
# components/st75256/CMakeLists.txt
idf_component_register(
    SRCS "esp_lcd_st75256.c"
    INCLUDE_DIRS "."
    REQUIRES esp_lcd driver esp_lvgl_port
)
