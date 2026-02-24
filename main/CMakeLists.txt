cmake_minimum_required(VERSION 3.16)

# 项目配置
set(PROJECT_VER "1.0")
set(PROJECT_VER_NUMBER 1)

# LVGL 组件路径
set(LVGL_DIR "../managed_components/lvgl__lvgl")
set(LVGL_DEMOS_DIR "${LVGL_DIR}/demos")

idf_component_register(
    SRCS 
        # 主程序
        "i2c_st75256.c" 
        "lvgl_demo_ui.c"
        
        # LVGL Benchmark 源文件
        "${LVGL_DEMOS_DIR}/benchmark/lv_demo_benchmark.c"
        
    PRIV_REQUIRES 
        # 私有依赖
        spi_flash
        ST75256 
        esp_lcd 
        lvgl 
        esp_lvgl_port
        driver 
        freertos 
        esp_timer 
        MYUI
        
    INCLUDE_DIRS 
        # 当前目录
        ""
        # LVGL Demo 头文件路径
        "${LVGL_DEMOS_DIR}/benchmark"
        "${LVGL_DEMOS_DIR}"
)