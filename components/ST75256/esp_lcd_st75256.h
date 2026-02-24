/*
 * SPDX-FileCopyrightText: 2024 Your Name
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "esp_lcd_panel_dev.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief ST75256 configuration structure
 *
 * To be used as esp_lcd_panel_dev_config_t.vendor_config.
 * See esp_lcd_new_panel_st75256().
 */
typedef struct {
    /**
     * @brief Display orientation mode
     *        - 0: Landscape mode (256 columns x 128 rows)
     *        - 1: Portrait mode  (128 columns x 256 rows)
     *
     * This controls the scan direction via command 0xBC.
     * Default is 0 (256x128).
     */
    uint8_t orientation;
} esp_lcd_panel_st75256_config_t;

/**
 * @brief Create LCD panel for model ST75256
 *
 * @param[in] io LCD panel IO handle (I2C or SPI)
 * @param[in] panel_dev_config General panel device configuration
 * @param[out] ret_panel Returned LCD panel handle
 * @return
 *          - ESP_ERR_INVALID_ARG   if parameter is invalid
 *          - ESP_ERR_NO_MEM        if out of memory
 *          - ESP_OK                on success
 *
 * @note The default panel size is 256x128 (landscape).
 * @note Use esp_lcd_panel_st75256_config_t to set orientation to 128x256.
 *
 * Example usage:
 * @code {c}
 * esp_lcd_panel_st75256_config_t st75256_config = {
 *     .orientation = 0 // 256x128
 * };
 * esp_lcd_panel_dev_config_t panel_config = {
 *     .reset_gpio_num = GPIO_NUM_18,
 *     .color_space = ESP_LCD_COLOR_SPACE_MONOCHROME,
 *     .bits_per_pixel = 1,
 *     .vendor_config = &st75256_config,
 * };
 * esp_lcd_new_panel_st75256(io_handle, &panel_config, &panel_handle);
 * @endcode
 */
esp_err_t esp_lcd_new_panel_st75256(const esp_lcd_panel_io_handle_t io,
                                    const esp_lcd_panel_dev_config_t *panel_dev_config,
                                    esp_lcd_panel_handle_t *ret_panel);

#ifdef __cplusplus
}
#endif