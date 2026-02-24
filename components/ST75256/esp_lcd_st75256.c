#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_st75256.h"
#include "esp_log.h"
#include "esp_lcd_panel_ops.h"
#include "esp_compiler.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_check.h"               // 提供 ESP_RETURN_ON_ERROR 等
#include "esp_lcd_panel_vendor.h"
#include <stdint.h>
#include <sys/cdefs.h>
#include "sdkconfig.h"
#if CONFIG_LCD_ENABLE_DEBUG_LOG
// The local log level must be defined before including esp_log.h
#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
#endif

static const char *TAG = "lcd_panel.st75256";

// ST75256 Commands (Command Set 1, entered by sending 0x30)
#define ST75256_CMD_SET_COLUMN_RANGE      0x15  // Followed by 2 byte
#define ST75256_CMD_SET_PAGE_RANGE        0x75  // Followed by 2 byte
#define ST75256_CMD_WRITE_RAM             0x5C  // Write data to GRAM
#define ST75256_CMD_DISP_OFF              0xAE  // Display OFF
#define ST75256_CMD_DISP_ON               0xAF  // Display ON
#define ST75256_CMD_INVERT_OFF            0xA6  // Normal display
#define ST75256_CMD_INVERT_ON             0xA7  // Inverse display
#define ST75256_CMD_POWER_SAVE_ON         0x95  // Enter power save
#define ST75256_CMD_POWER_SAVE_OFF        0x94  // Exit power save
#define ST75256_CMD_ALL_PIXEL_OFF         0x22  // turn off all pixels
#define ST75256_CMD_ALL_PIXEL_ON          0x23  // turn on all pixels
#define ST75256_CMD_SET_DATA_MSB          0x08  // MSB first
#define ST75256_CMD_SET_DATA_LSB          0x0C  // LSB first
#define ST75256_CMD_DISPLAY_CONTROL       0xCA  // Followed by 3 byte
#define ST75256_CMD_SET_CONTRAST          0x81  // Followed by 2 byte
#define ST75256_CMD_SET_POWER_CONTROL     0x20  // Followed by 1 byte
#define ST75256_CMD_SET_DISPLAY_MODE      0xF0  // Followed by 1 byte
#define ST75256_CMD_SET_SCAN_DIRECTION    0xBC  // Followed by 1 byte: 0x00~0x07

// ST75256 Commands (Command Set 2, entered by sending 0x31)
#define ST75256_CMD_SET_GRAYSCALE_TABLE   0x20  // Followed by 16 bytes
#define ST75256_CMD_DISABLE_AUTO_READ     0xD7  // Disable OPT auto read
#define ST75256_CMD_ANALOG_CIRCUIT_SET    0x32  // Followed by 3 byte

// Command set selectors
#define ST75256_CMD_SET_1                 0x30  // Switch to Command Set 1
#define ST75256_CMD_SET_2                 0x31  // Switch to Command Set 2

// ST75256 Physical Coordinates
#define ST75256_TOTAL_PAGES               0x14  // Total 21 pages 

// Predefined grayscale table (16 levels)
static const uint8_t grayscale_table[16] = {
    0x01, 0x03, 0x05, 0x07, 0x09, 0x0B, 0x0D, 0x10,
    0x11, 0x13, 0x15, 0x17, 0x19, 0x1B, 0x1D, 0x1F
};

// Panel private data
typedef struct {
    esp_lcd_panel_t base;
    esp_lcd_panel_io_handle_t io;
    uint8_t height;           // Physical height in pixels (128 or 256)
    uint8_t width;            // Physical width in pixels (256 or 128)
    int reset_gpio_num;
    int x_gap;
    int y_gap;
    unsigned int bits_per_pixel;
    bool reset_level;
    bool swap_axes;           // true = 128x256 mode, false = 256x128 mode
    bool y_mirror;           // true = Y mirror mode, false = Y normal mode
} st75256_panel_t;

static void st75256_remap_swapped_frame(uint8_t *src, uint8_t *dst);
static inline void st75256_apply_mirror(int *start, int *end);

// Helper: switch to Command Set 1
static inline esp_err_t st75256_set_cmd_set_1(esp_lcd_panel_io_handle_t io)
{
    return esp_lcd_panel_io_tx_param(io, ST75256_CMD_SET_1, NULL, 0);
}

// Helper: switch to Command Set 2
static inline esp_err_t st75256_set_cmd_set_2(esp_lcd_panel_io_handle_t io)
{
    return esp_lcd_panel_io_tx_param(io, ST75256_CMD_SET_2, NULL, 0);
}

// Helper: send scan direction command (0xBC + value)
static esp_err_t st75256_set_scan_direction(st75256_panel_t *st75256, uint8_t dir)
{
    ESP_RETURN_ON_ERROR(st75256_set_cmd_set_1(st75256->io), TAG, "switch to cmd set 1 failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(st75256->io, ST75256_CMD_SET_SCAN_DIRECTION, NULL, 0), TAG, "send scan dir cmd failed");
    return esp_lcd_panel_io_tx_color(st75256->io, -1, &dir, 1);
}

static esp_err_t panel_st75256_del(esp_lcd_panel_t *panel);
static esp_err_t panel_st75256_reset(esp_lcd_panel_t *panel);
static esp_err_t panel_st75256_init(esp_lcd_panel_t *panel);
static esp_err_t panel_st75256_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end, const void *color_data);
static esp_err_t panel_st75256_invert_color(esp_lcd_panel_t *panel, bool invert_color_data);
static esp_err_t panel_st75256_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y);
static esp_err_t panel_st75256_swap_xy(esp_lcd_panel_t *panel, bool swap_axes);
static esp_err_t panel_st75256_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap);
static esp_err_t panel_st75256_disp_on_off(esp_lcd_panel_t *panel, bool off);

esp_err_t esp_lcd_new_panel_st75256(const esp_lcd_panel_io_handle_t io,
                                    const esp_lcd_panel_dev_config_t *panel_dev_config,
                                    esp_lcd_panel_handle_t *ret_panel)
{
#if CONFIG_LCD_ENABLE_DEBUG_LOG
    esp_log_level_set(TAG, ESP_LOG_DEBUG);
#endif
    esp_err_t ret = ESP_OK;
    st75256_panel_t *st75256 = NULL;
    ESP_GOTO_ON_FALSE(io && panel_dev_config && ret_panel, ESP_ERR_INVALID_ARG, err, TAG, "invalid argument");
    ESP_GOTO_ON_FALSE(panel_dev_config->bits_per_pixel == 1, ESP_ERR_INVALID_ARG, err, TAG, "bpp must be 1");

    esp_lcd_panel_st75256_config_t *st75256_spec_config = (esp_lcd_panel_st75256_config_t *)panel_dev_config->vendor_config;
    bool swap_axes = st75256_spec_config ? (st75256_spec_config->orientation != 0) : false;

    // Determine physical dimensions based on orientation
    uint8_t width = swap_axes ? 128 : 256;
    uint8_t height = swap_axes ? 256 : 128;

    ESP_COMPILER_DIAGNOSTIC_PUSH_IGNORE("-Wanalyzer-malloc-leak")
    st75256 = calloc(1, sizeof(st75256_panel_t));
    ESP_GOTO_ON_FALSE(st75256, ESP_ERR_NO_MEM, err, TAG, "no mem for st75256 panel");

    if (panel_dev_config->reset_gpio_num >= 0) {
        gpio_config_t io_conf = {
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = 1ULL << panel_dev_config->reset_gpio_num,
        };
        ESP_GOTO_ON_ERROR(gpio_config(&io_conf), err, TAG, "configure GPIO for RST line failed");
    }

    st75256->io = io;
    st75256->bits_per_pixel = panel_dev_config->bits_per_pixel;
    st75256->reset_gpio_num = panel_dev_config->reset_gpio_num;
    st75256->reset_level = panel_dev_config->flags.reset_active_high;
    st75256->width = width;
    st75256->height = height;
    st75256->swap_axes = swap_axes;
    st75256->base.del = panel_st75256_del;
    st75256->base.reset = panel_st75256_reset;
    st75256->base.init = panel_st75256_init;
    st75256->base.draw_bitmap = panel_st75256_draw_bitmap;
    st75256->base.invert_color = panel_st75256_invert_color;
    st75256->base.set_gap = panel_st75256_set_gap;
    st75256->base.mirror = panel_st75256_mirror;
    st75256->base.swap_xy = panel_st75256_swap_xy;
    st75256->base.disp_on_off = panel_st75256_disp_on_off;
    *ret_panel = &(st75256->base);
    ESP_LOGD(TAG, "new st75256 panel @%p, %ux%u", st75256, width, height);

    return ESP_OK;

err:
    if (st75256) {
        if (panel_dev_config->reset_gpio_num >= 0) {
            gpio_reset_pin(panel_dev_config->reset_gpio_num);
        }
        free(st75256);
    }
    return ret;
    ESP_COMPILER_DIAGNOSTIC_POP("-Wanalyzer-malloc-leak")
}

static esp_err_t panel_st75256_del(esp_lcd_panel_t *panel)
{
    st75256_panel_t *st75256 = __containerof(panel, st75256_panel_t, base);
    if (st75256->reset_gpio_num >= 0) {
        gpio_reset_pin(st75256->reset_gpio_num);
    }
    ESP_LOGD(TAG, "del st75256 panel @%p", st75256);
    free(st75256);
    return ESP_OK;
}

static esp_err_t panel_st75256_reset(esp_lcd_panel_t *panel)
{
    st75256_panel_t *st75256 = __containerof(panel, st75256_panel_t, base);
    if (st75256->reset_gpio_num >= 0) {
        gpio_set_level(st75256->reset_gpio_num, st75256->reset_level);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(st75256->reset_gpio_num, !st75256->reset_level);
        vTaskDelay(pdMS_TO_TICKS(120)); // ST75256 requires >100ms after reset
    }
    return ESP_OK;
}

static esp_err_t panel_st75256_init(esp_lcd_panel_t *panel)
{
    st75256_panel_t *st75256 = __containerof(panel, st75256_panel_t, base);
    esp_lcd_panel_io_handle_t io = st75256->io;

    // Step 1: Enter Command Set 1 and turn display OFF
    ESP_RETURN_ON_ERROR(st75256_set_cmd_set_1(io), TAG, "enter cmd set 1 failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, ST75256_CMD_DISP_OFF, NULL, 0), TAG, "display off failed");

    // Step 2: Exit power save mode
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, ST75256_CMD_POWER_SAVE_OFF, NULL, 0), TAG, "power save off failed");

    // Step 3: Set data format (MSB first)
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, ST75256_CMD_SET_DATA_MSB, NULL, 0), TAG, "set data format failed");

    // Step 4: Enter Command Set 2 for advanced config
    ESP_RETURN_ON_ERROR(st75256_set_cmd_set_2(io), TAG, "enter cmd set 2 failed");

    // Step 5: Disable auto-read
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, ST75256_CMD_DISABLE_AUTO_READ, NULL, 0), TAG, "disable auto-read cmd failed");
    uint8_t disable_auto_read_val = 0x9F;
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_color(io, -1, &disable_auto_read_val, 1), TAG, "disable auto-read param failed");

    // Step 6: Analog circuit setting
    uint8_t analog_cfg[3] = {0x00, 0x01, 0x00};
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, ST75256_CMD_ANALOG_CIRCUIT_SET, NULL, 0), TAG, "analog circuit cmd failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_color(io, -1, analog_cfg, 3), TAG, "analog circuit param failed");

    // Step 7: Gray scale table
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, ST75256_CMD_SET_GRAYSCALE_TABLE, NULL, 0), TAG, "gray scale cmd failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_color(io, -1, grayscale_table, 16), TAG, "gray scale data failed");

    // Step 8: Back to Command Set 1 for contrast and power
    ESP_RETURN_ON_ERROR(st75256_set_cmd_set_1(io), TAG, "back to cmd set 1 failed");

    // Step 9: Contrast setting (0x81 + 2 bytes)
    uint8_t contrast_val[2] = {0x1E, 0x05};
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, ST75256_CMD_SET_CONTRAST, NULL, 0), TAG, "contrast cmd failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_color(io, -1, contrast_val, 2), TAG, "contrast param failed");

    // Step 10: Power control (simplified)
    uint8_t power_val = 0x0B;
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, ST75256_CMD_SET_POWER_CONTROL, NULL, 0), TAG, "power ctrl cmd failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_color(io, -1, &power_val, 1), TAG, "power ctrl param failed");

    // Step 11: Display control (0xCA + 3 bytes)
    uint8_t display_ctrl[3] = {0x00, 0x7F, 0x20}; // 典型值：设置CL驱动频率=0, 占空比=128, 帧周期=0x20
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, ST75256_CMD_DISPLAY_CONTROL, NULL, 0), TAG, "display control cmd failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_color(io, -1, display_ctrl, 3), TAG, "display control param failed");

    // Step 12: Display mode (monochrome)
    uint8_t display_mode = 0x10; // 0x10 = monochrome（单色）, 0x11 = grayscale（四级灰度）
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, ST75256_CMD_SET_DISPLAY_MODE, NULL, 0), TAG, "display mode cmd failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_color(io, -1, &display_mode, 1), TAG, "display mode param failed");

    // Step 13: Normal display mode
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, ST75256_CMD_INVERT_OFF, NULL, 0), TAG, "normal display failed");

    // Step 14: Clear display RAM
    {
        // Switch to Command Set 1
        ESP_RETURN_ON_ERROR(st75256_set_cmd_set_1(io), TAG, "enter cmd set 1 failed");
        // 设置列地址范围: 0 ~ 255
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, ST75256_CMD_SET_COLUMN_RANGE, NULL, 0), TAG, "set column range cmd failed");
        uint8_t col_range[2] = {0, 255};
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_color(io, -1, col_range, 2), TAG, "set column range param failed");

        // 设置页地址范围: 0 ~ 14 
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, ST75256_CMD_SET_PAGE_RANGE, NULL, 0), TAG, "set page range cmd failed");
        uint8_t page_range[2] = {0, 40};
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_color(io, -1, page_range, 2), TAG, "set page range param failed");

        // 开始写显存
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, ST75256_CMD_WRITE_RAM, NULL, 0), TAG, "write ram cmd failed");

        // 发送 4096 字节的 0x00
        static const uint8_t zero_byte = 0x00;
        const size_t total_bytes = 256 * 16; // width * num_pages
        for (size_t i = 0; i < total_bytes; i++) {
            ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_color(io, -1, &zero_byte, 1), TAG, "clear ddram failed");
        }
    }

    // Display remains OFF until disp_on_off(true) is called
    return ESP_OK;
}

static esp_err_t panel_st75256_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end, const void *color_data)
{
    st75256_panel_t *st75256 = __containerof(panel, st75256_panel_t, base);
    esp_lcd_panel_io_handle_t io = st75256->io;
    void *color_data_local = NULL;

    // >>> 调试：打印原始坐标 <<<
    ESP_LOGD(TAG, "Draw bitmap: input rect = (%d, %d) -> (%d, %d)", x_start, y_start, x_end, y_end);

    // Apply gap offset (for panels with non-zero start address)
    x_start += st75256->x_gap;
    x_end += st75256->x_gap;
    y_start += st75256->y_gap;
    y_end += st75256->y_gap;

    // >>> 调试：打印 gap 后坐标 <<<
    ESP_LOGD(TAG, "After gap: (%d, %d) -> (%d, %d), swap_axes=%s",
             x_start, y_start, x_end, y_end,
             st75256->swap_axes ? "true" : "false");

    // Handle coordinate swap if enabled
    if (st75256->swap_axes) {    
        if (st75256->y_mirror) {
            // If Y is mirrored, adjust X coordinates accordingly
            st75256_apply_mirror(&x_start, &x_end);
            // After applying Y mirror, the coordinates are still in swapped orientation, so we will swap them later together with X/Y
        }
        
        //设置竖向扫描（128x256 模式）后，坐标系变为 Y 轴向下，X 轴向左，但物理内存布局仍是按行（水平）扫描的，因此需要交换 X/Y 坐标并重新排列像素数据
        int tmp = x_start;
        x_start = y_start;
        y_start = tmp;
        tmp = x_end;
        x_end = y_end;
        y_end = tmp;

        // >>> 调试：打印交换后坐标 <<<
        ESP_LOGD(TAG, "After swap_xy: (%d, %d) -> (%d, %d)", x_start, y_start, x_end, y_end);

        static uint8_t s_remap_buffer[16 * 256];     // 交换坐标后需要重新排列像素数据，暂存缓冲区（最大支持全屏交换）,注：分辨率改动后需要修改大小
        memset(s_remap_buffer, 0, sizeof(s_remap_buffer));  // 清空缓冲区
        st75256_remap_swapped_frame((uint8_t *)color_data, s_remap_buffer);
        color_data_local = (void*)s_remap_buffer;
    }
    else {
        if (st75256->y_mirror) {
            // If Y is mirrored, adjust Y coordinates accordingly
            st75256_apply_mirror(&y_start, &y_end); //
        }
        color_data_local = (void*)color_data;
    }

    // ST75256 organizes memory in pages (8 rows per page)
    uint8_t page_start = y_start / 8;
    uint8_t page_end = (y_end - 1) / 8;
    uint8_t num_pages = page_end - page_start + 1;

    // Width in pixels (columns)
    int width = x_end - x_start;

    // >>> 调试：打印页和宽度 <<<
    ESP_LOGD(TAG, "Page range: %u -> %u (num=%u), width=%d", page_start, page_end, num_pages, width);

    // Switch to Command Set 1
    ESP_RETURN_ON_ERROR(st75256_set_cmd_set_1(io), TAG, "enter cmd set 1 failed");

    // Set column address range [x_start, x_end)
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, ST75256_CMD_SET_COLUMN_RANGE, NULL, 0), TAG, "set column range cmd failed");
    uint8_t col_param[2] = {(uint8_t)x_start, (uint8_t)(x_end - 1)};
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_color(io, -1, col_param, 2), TAG, "set column range param failed");

    // Set page address range [page_start, page_end]
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, ST75256_CMD_SET_PAGE_RANGE, NULL, 0), TAG, "set page range cmd failed");
    uint8_t page_param[2] = {page_start, page_end};
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_color(io, -1, page_param, 2), TAG, "set page range param failed");

    // Start writing RAM
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, ST75256_CMD_WRITE_RAM, NULL, 0), TAG, "write ram cmd failed");

    // Calculate correct data size: pages × width (each page has 'width' bytes)
    size_t data_size = num_pages * width;
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_color(io, -1, color_data_local, data_size), TAG, "send pixel data failed");

    return ESP_OK;
}

static esp_err_t panel_st75256_invert_color(esp_lcd_panel_t *panel, bool invert_color_data)
{
    st75256_panel_t *st75256 = __containerof(panel, st75256_panel_t, base);
    esp_lcd_panel_io_handle_t io = st75256->io;
    uint8_t cmd = invert_color_data ? ST75256_CMD_INVERT_ON : ST75256_CMD_INVERT_OFF;
    ESP_RETURN_ON_ERROR(st75256_set_cmd_set_1(io), TAG, "enter cmd set 1 failed");
    return esp_lcd_panel_io_tx_param(io, cmd, NULL, 0);
}

static esp_err_t panel_st75256_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y)
{
    st75256_panel_t *st75256 = __containerof(panel, st75256_panel_t, base);
    esp_lcd_panel_io_handle_t io = st75256->io;
    uint8_t dir = 0x00;

    // Base direction from swap_axes
    if (st75256->swap_axes) {
        dir = 0x04; // 128x256 base
    } else {
        dir = 0x00; // 256x128 base
    }

    // Apply mirroring bits (bit1: X, bit0: Y)
    if (mirror_x) dir |= 0x02;
    if (mirror_y) 
    {
        dir |= 0x01;
        st75256->y_mirror = true;
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, ST75256_CMD_SET_DATA_MSB, NULL, 0), TAG, "set data format failed");
    }
    else
    {
        st75256->y_mirror = false;
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, ST75256_CMD_SET_DATA_LSB, NULL, 0), TAG, "set data format failed");
    }

    return st75256_set_scan_direction(st75256, dir);
}

static esp_err_t panel_st75256_swap_xy(esp_lcd_panel_t *panel, bool swap_axes)
{
    st75256_panel_t *st75256 = __containerof(panel, st75256_panel_t, base);
    st75256->swap_axes = swap_axes;
    // Update scan direction to match new orientation
    uint8_t dir = swap_axes ? 0x04 : 0x00;
    return st75256_set_scan_direction(st75256, dir);
}

static esp_err_t panel_st75256_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap)
{
    st75256_panel_t *st75256 = __containerof(panel, st75256_panel_t, base);
    st75256->x_gap = x_gap;
    st75256->y_gap = y_gap;
    return ESP_OK;
}

static esp_err_t panel_st75256_disp_on_off(esp_lcd_panel_t *panel, bool on_off)
{
    st75256_panel_t *st75256 = __containerof(panel, st75256_panel_t, base);
    esp_lcd_panel_io_handle_t io = st75256->io;
    uint8_t cmd = on_off ? ST75256_CMD_DISP_ON : ST75256_CMD_DISP_OFF;
    ESP_RETURN_ON_ERROR(st75256_set_cmd_set_1(io), TAG, "enter cmd set 1 failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, cmd, NULL, 0), TAG, "disp on/off failed");
    // Optional delay if needed by panel
    if (on_off) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return ESP_OK;
}

/**
 * 镜像坐标变换（通用）
 * 
 * 变换公式：new_start = total_height - old_end, new_end = total_height - old_start
 * 
 * @param start [in/out] 起始坐标
 * @param end   [in/out] 结束坐标
 */
static inline void st75256_apply_mirror(int *start, int *end)
{
    const int total_height = (ST75256_TOTAL_PAGES + 1) * 8;
    int tmp_start = total_height - *end;
    int tmp_end   = total_height - *start;
    *start = tmp_start;
    *end   = tmp_end;
}

/**
 * 将 LVGL 的 swap_xy 显存格式转换为 ST75256 硬件页格式
 *
 * @param src  LVGL 显存指针，4096 字节 (32 页 × 128 字节)
 * @param dst  ST75256 显存指针，4096 字节 (16 页 × 256 字节)
 * 
 * @note 边界检查：dst_byte 范围 0~4095
 *       最大值：LVGL_Y=255, LVGL_X=127 → dst_byte = 255*16 + 15 = 4095 
 */
static void st75256_remap_swapped_frame(uint8_t *src, uint8_t *dst)
{
    if (!src || !dst) {
        ESP_LOGE(TAG, "Invalid pointer: src=%p, dst=%p", src, dst);
        return;
    }

    for(int page = 0; page < 32; page++)
    {
        for(int x = 0; x < 128; x++)
        {
            uint8_t src_byte = src[x + page * 128];
            if (src_byte == 0) continue;

            for(int bit = 0; bit < 8; bit++)
            {
                if(src_byte & (1 << bit))
                {
                    // 计算像素位置
                    int LVGL_Y = page * 8 + bit;  
                    int LVGL_X = x;

                    // 计算目标位置
                    int dst_byte = LVGL_Y * 16 + LVGL_X / 8;
                    int dst_bit = LVGL_X % 8;

                    // 边界检查
                    if (dst_byte < 0 || dst_byte >= 16 * 256) continue;

                    dst[dst_byte] |= (1 << dst_bit);
                }
            }
        }
    }
}

