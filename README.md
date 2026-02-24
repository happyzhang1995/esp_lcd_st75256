# esp_lcd_st75256
ESP32-C3 LVGL port for ST75256 monochrome LCD via I2C. Features optimized frame buffer remapping, full-screen/partial refresh support, and benchmark demos. Built with esp_lvgl_port and ESP-IDF.

基于 **ESP32-C3** 和 **ESP-IDF** 框架，将 **LVGL v8** 图形库移植到 **ST75256** 单色 LCD 屏幕（I2C 接口）的开源项目。

本项目解决了 ST75256 垂直显示的显存排列（水平 8 像素/字节）与 LVGL 默认格式（垂直 8 像素/字节）不匹配的问题，实现了屏幕旋转显示，并包含了性能基准测试（Benchmark）。

## ✨ 主要特性 (Features)

- 🖥️ **硬件支持**: ESP32-C3 + ST75256 (256x128, 1bpp 单色，品牌：晶联讯，型号：JLX256128G-978-PN)
- 🔌 **通信接口**: I2C (支持 800kHz)
- 🎨 **LVGL 集成**: 基于 `esp_lvgl_port` 组件，支持 LVGL v8 
- ⚡ **显存调整**: 
  - 自定义 `st75256_remap_swapped_frame` 实现位图重排 (Bit Remapping)
  - 解决 LVGL 垂直像素排列 vs ST75256 水平页式排列的冲突
  - 支持ST75256水平、垂直、XY镜像翻转显示

## 📸 演示效果 (Demo)

| Benchmark 测试帧数：9-12FPS | 普通场景帧数：13-15FPS |
使用I2C总线虽然速度不快，但比SPI少2用两个引脚，可适当调高SCL频率，C3最高800kHz。

## 🛠️ 硬件连接 (Hardware Wiring)

| ST75256 Pin | ESP32-C3 Pin | 说明              |

| VCC         | 3.3V         | 电源              |

| GND         | GND          | 接地              |

| SCL         | GPIO_5       | I2C 时钟 (需上拉) |

| SDA         | GPIO_4       | I2C 数据 (需上拉) |

| RES         | GPIO_X       | 复位 (可选)       |

> **注意**: I2C 总线需要外接 4.7kΩ 上拉电阻以获得最佳速度。

## 🚀 快速开始 (Quick Start)

### 1. 环境要求
- ESP-IDF v5.0 或更高版本
- CMake 3.16+
- Python 3.8+

### 2. 克隆项目
```bash
git clone https://github.com/happyzhang1995/esp_lcd_st75256.git
cd esp_lcd_st75256
idf.py set-target esp32c3
