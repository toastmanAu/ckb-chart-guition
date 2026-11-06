#pragma once

#define BOARD_BOOT_BACKLIGHT 15

#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"

#define BOARD_HRES 480
#define BOARD_VRES 480

esp_err_t board_init(void);
esp_lcd_panel_handle_t board_panel(void);
esp_lcd_touch_handle_t board_touch(void);
esp_err_t board_set_backlight(uint8_t percent);
