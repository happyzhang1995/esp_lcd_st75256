#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_system.h"
#include "esp_check.h"
#include "esp_timer.h" 
#include "driver/i2c_master.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "ui.h"
#include "esp_lcd_st75256.h"

// 引入 benchmark 头文件
#include "lv_demo_benchmark.h"

extern void example_lvgl_demo_ui(lv_disp_t *disp);

// st75256配置参数
#define ST75256_PIN_NUM_RST -1
#define ST75256_I2C_ADDR    0x3C

#define LCD_H_RES 256
#define LCD_V_RES 128

#define I2C_MASTER_SCL_IO    5        // SCL 引脚
#define I2C_MASTER_SDA_IO    4        // SDA 引脚
#define I2C_MASTER_FREQ_HZ   800000   // 800 kHz
#define I2C_MASTER_TIMEOUT_MS 1000    // 超时时间
#define I2C_MASTER_PORT      I2C_NUM_0    // I2C 端口号

static const char *I2C_TAG = "I2C_BUS";              // 日志标签

// 全局变量
static i2c_master_bus_handle_t i2c_bus_handle;      // I2C 总线句柄

// 初始化I2C总线
static esp_err_t init_i2c_bus(void)
{
    // 初始化I2C主机配置
      i2c_master_bus_config_t i2c_mst_config = {
        .i2c_port = I2C_MASTER_PORT,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = false,  // 如有外部上拉，关闭内部上拉（开启后会影响ST75256 通信）
    };

    ESP_RETURN_ON_ERROR(
        i2c_new_master_bus(&i2c_mst_config, &i2c_bus_handle),
        I2C_TAG,
        "Failed to create I2C master bus"
    );

    ESP_LOGI(I2C_TAG, "I2C bus initialized successfully");
    return ESP_OK;
}

static esp_err_t install_st75256_panel(i2c_master_bus_handle_t i2c_bus,
                                       esp_lcd_panel_handle_t *panel_handle,
                                       esp_lcd_panel_io_handle_t *io_handle)
{
    assert(i2c_bus && panel_handle && io_handle);
    ESP_LOGI("ST75256", "Install ST75256 panel");

    // 创建 Panel IO
    esp_lcd_panel_io_i2c_config_t io_config = {
        .dev_addr = ST75256_I2C_ADDR,
        .scl_speed_hz = I2C_MASTER_FREQ_HZ,
        .control_phase_bytes = 1,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .dc_bit_offset = 6,
        .flags = {
            .disable_control_phase = 0, // ST75256: 0x00=CMD, 0x40=DATA
        },
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(i2c_bus, &io_config, io_handle), "ST75256", "install panel IO failed");

    // ST75256 专用配置（256x128 模式） （可选）
    esp_lcd_panel_st75256_config_t st75256_config = {
        .orientation = 0,  // 0 = 256 columns × 128 rows (landscape)
    };

    // 安装面板驱动（关键：传入 vendor_config）
    esp_lcd_panel_dev_config_t panel_config = {
        .bits_per_pixel = 1,
        .reset_gpio_num = ST75256_PIN_NUM_RST,
        .vendor_config = &st75256_config,  // 指向ST75256 专用配置
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st75256(*io_handle, &panel_config, panel_handle), "ST75256", "install ST75256 driver failed");

    // 初始化面板
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(*panel_handle), "ST75256", "panel reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(*panel_handle), "ST75256", "panel init failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(*panel_handle, true), "ST75256", "turn on display failed");

    return ESP_OK;
}

static lv_disp_t *initialize_lvgl_display(esp_lcd_panel_handle_t panel_handle,
                                          esp_lcd_panel_io_handle_t io_handle)
{
    ESP_LOGI("LVGL", "Initialize LVGL");

    // 初始化 LVGL 端口

    //使用默认配置
    //const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();

    // 自定义配置
    const lvgl_port_cfg_t lvgl_cfg = {
        .task_priority = 4,           // 设定优先级
        .task_stack = 7168,           // 设定栈大小
        .task_affinity = -1,          // 不绑定核心
        .task_max_sleep_ms = 500,     // 设定最大睡眠
        .task_stack_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DEFAULT,
        .timer_period_ms = 2,         // 从 默认 5ms 改为 2ms
    };
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    // 配置显示参数
    //当 ST75256 以256x128模式工作时: hres = LCD_H_RES（256），vres = LCD_V_RES（128），swap_xy=false
    //当 ST75256 以128x256模式工作时: hres = LCD_V_RES（128），vres = LCD_H_RES（256），swap_xy=true
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io_handle,
        .panel_handle = panel_handle,
        .buffer_size = LCD_H_RES * LCD_V_RES, // 1bpp
        .double_buffer = true,
        .hres = LCD_H_RES,
        .vres = LCD_V_RES,
        .monochrome = true,
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        }
    };

    lv_disp_t *disp = lvgl_port_add_disp(&disp_cfg);
    if (!disp) {
        ESP_LOGE("LVGL", "Failed to add display to LVGL");
        return NULL;
    }

    lv_disp_set_rotation(disp, LV_DISP_ROT_NONE);
    return disp;
}

void app_main(void)
{
    /* Print chip information */
    esp_chip_info_t chip_info;
    uint32_t flash_size;
    esp_chip_info(&chip_info);
    printf("This is %s chip with %d CPU core(s), %s%s%s%s, ",
           CONFIG_IDF_TARGET,
           chip_info.cores,
           (chip_info.features & CHIP_FEATURE_WIFI_BGN) ? "WiFi/" : "",
           (chip_info.features & CHIP_FEATURE_BT) ? "BT" : "",
           (chip_info.features & CHIP_FEATURE_BLE) ? "BLE" : "",
           (chip_info.features & CHIP_FEATURE_IEEE802154) ? ", 802.15.4 (Zigbee/Thread)" : "");

    unsigned major_rev = chip_info.revision / 100;
    unsigned minor_rev = chip_info.revision % 100;
    printf("silicon revision v%d.%d, ", major_rev, minor_rev);
    if(esp_flash_get_size(NULL, &flash_size) != ESP_OK) {
        printf("Get flash size failed");
        return;
    }

    printf("%" PRIu32 "MB %s flash\n", flash_size / (uint32_t)(1024 * 1024),
           (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "embedded" : "external");

    printf("Minimum free heap size: %" PRIu32 " bytes\n", esp_get_minimum_free_heap_size());

    /*user application code*/
    esp_err_t ret;

    // 初始化I2C总线
    ret = init_i2c_bus();
    if (ret != ESP_OK) {
        ESP_LOGE(I2C_TAG, "Failed to initialize I2C bus: %s", esp_err_to_name(ret));
        return;
    }

    // 安装 ST75256 面板（包含 IO 和驱动）
    esp_lcd_panel_handle_t panel_handle = NULL;
    esp_lcd_panel_io_handle_t io_handle = NULL;
    ESP_ERROR_CHECK(install_st75256_panel(i2c_bus_handle, &panel_handle, &io_handle));

    // 初始化 LVGL 并注册显示设备
    lv_disp_t *disp = initialize_lvgl_display(panel_handle, io_handle);
    if (!disp) {
        ESP_LOGE("LVGL", "Failed to initialize LVGL display");
        return;
    }
    
    // 启动 LVGL UI 示例
    ESP_LOGI("LVGL", "Start LVGL demo");
    
    if (lvgl_port_lock(0)) {
        //example_lvgl_demo_ui(disp);   // 运行官方示例
        lv_demo_benchmark();          // 运行基准测试
        //ui_init();                      // 运行squareline 自定义 UI
        lvgl_port_unlock();
    }  
}
