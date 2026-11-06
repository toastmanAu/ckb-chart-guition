#include "board.h"

#include <string.h>
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_io_additions.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_st7701.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_log.h"

static const char *TAG = "board";
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_touch_handle_t s_touch;

static const st7701_lcd_init_cmd_t s_guition_init[] = {
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x10}, 5, 0},
    {0xC0, (uint8_t[]){0x3B, 0x00}, 2, 0},
    {0xC1, (uint8_t[]){0x0D, 0x02}, 2, 0},
    {0xC2, (uint8_t[]){0x31, 0x05}, 2, 0},
    {0xCD, (uint8_t[]){0x00}, 1, 0},
    {0xB0, (uint8_t[]){0x00, 0x11, 0x18, 0x0E, 0x11, 0x06, 0x07, 0x08,
                        0x07, 0x22, 0x04, 0x12, 0x0F, 0xAA, 0x31, 0x18}, 16, 0},
    {0xB1, (uint8_t[]){0x00, 0x11, 0x19, 0x0E, 0x12, 0x07, 0x08, 0x08,
                        0x08, 0x22, 0x04, 0x11, 0x11, 0xA9, 0x32, 0x18}, 16, 0},
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x11}, 5, 0},
    {0xB0, (uint8_t[]){0x60}, 1, 0},
    {0xB1, (uint8_t[]){0x32}, 1, 0},
    {0xB2, (uint8_t[]){0x07}, 1, 0},
    {0xB3, (uint8_t[]){0x80}, 1, 0},
    {0xB5, (uint8_t[]){0x49}, 1, 0},
    {0xB7, (uint8_t[]){0x85}, 1, 0},
    {0xB8, (uint8_t[]){0x21}, 1, 0},
    {0xC1, (uint8_t[]){0x78}, 1, 0},
    {0xC2, (uint8_t[]){0x78}, 1, 0},
    {0xE0, (uint8_t[]){0x00, 0x1B, 0x02}, 3, 0},
    {0xE1, (uint8_t[]){0x08, 0xA0, 0x00, 0x00, 0x07, 0xA0,
                        0x00, 0x00, 0x00, 0x44, 0x44}, 11, 0},
    {0xE2, (uint8_t[]){0x11, 0x11, 0x44, 0x44, 0xED, 0xA0,
                        0x00, 0x00, 0xEC, 0xA0, 0x00, 0x00}, 12, 0},
    {0xE3, (uint8_t[]){0x00, 0x00, 0x11, 0x11}, 4, 0},
    {0xE4, (uint8_t[]){0x44, 0x44}, 2, 0},
    {0xE5, (uint8_t[]){0x0A, 0xE9, 0xD8, 0xA0, 0x0C, 0xEB, 0xD8, 0xA0,
                        0x0E, 0xED, 0xD8, 0xA0, 0x10, 0xEF, 0xD8, 0xA0}, 16, 0},
    {0xE6, (uint8_t[]){0x00, 0x00, 0x11, 0x11}, 4, 0},
    {0xE7, (uint8_t[]){0x44, 0x44}, 2, 0},
    {0xE8, (uint8_t[]){0x09, 0xE8, 0xD8, 0xA0, 0x0B, 0xEA, 0xD8, 0xA0,
                        0x0D, 0xEC, 0xD8, 0xA0, 0x0F, 0xEE, 0xD8, 0xA0}, 16, 0},
    {0xEB, (uint8_t[]){0x02, 0x00, 0xE4, 0xE4, 0x88, 0x00, 0x40}, 7, 0},
    {0xEC, (uint8_t[]){0x3C, 0x00}, 2, 0},
    {0xED, (uint8_t[]){0xAB, 0x89, 0x76, 0x54, 0x02, 0xFF, 0xFF, 0xFF,
                        0xFF, 0xFF, 0xFF, 0x20, 0x45, 0x67, 0x98, 0xBA}, 16, 0},
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x13}, 5, 0},
    {0xE5, (uint8_t[]){0xE4}, 1, 0},
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x00}, 5, 0},
    {0x3A, (uint8_t[]){0x60}, 1, 10},
    {0x11, NULL, 0, 120},
    {0x29, NULL, 0, 0},
};

static esp_err_t backlight_init(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 1000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "backlight timer");
    const ledc_channel_config_t channel = {
        .gpio_num = 38,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
    };
    return ledc_channel_config(&channel);
}

esp_err_t board_set_backlight(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }
    uint32_t duty = (uint32_t)percent * 1023 / 100;
    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty), TAG, "backlight duty");
    return ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static esp_err_t display_init(void)
{
    spi_line_config_t line = {
        .cs_io_type = IO_TYPE_GPIO,
        .cs_gpio_num = 39,
        .scl_io_type = IO_TYPE_GPIO,
        .scl_gpio_num = 48,
        .sda_io_type = IO_TYPE_GPIO,
        .sda_gpio_num = 47,
        .io_expander = NULL,
    };
    esp_lcd_panel_io_3wire_spi_config_t io_config = ST7701_PANEL_IO_3WIRE_SPI_CONFIG(line, 0);
    io_config.flags.del_keep_cs_inactive = 1;
    esp_lcd_panel_io_handle_t io = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_3wire_spi(&io_config, &io), TAG, "ST7701 IO");

    esp_lcd_rgb_panel_config_t rgb = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            /* 548 x 518 clocks per frame: 6 MHz gave ~21 Hz refresh, which
             * flickers visibly. 12 MHz ~= 42 Hz; bounce buffers keep PSRAM
             * reads ahead of the panel. */
            .pclk_hz = 12000000,
            .h_res = BOARD_HRES,
            .v_res = BOARD_VRES,
            /* The panel runs in DE mode, so porches do not move the image
             * (verified on-device: 10/10/20 and 10/73/20 looked identical).
             * Kept at the original, longest-tested values. */
            .hsync_pulse_width = 8,
            .hsync_back_porch = 50,
            .hsync_front_porch = 10,
            .vsync_pulse_width = 8,
            .vsync_back_porch = 20,
            .vsync_front_porch = 10,
            .flags.pclk_active_neg = false,
        },
        .data_width = 16,
        .bits_per_pixel = 16,
        .num_fbs = 2,
        .bounce_buffer_size_px = 4800,
        .dma_burst_size = 64,
        .hsync_gpio_num = 16,
        .vsync_gpio_num = 17,
        .de_gpio_num = 18,
        .pclk_gpio_num = 21,
        .disp_gpio_num = -1,
        .data_gpio_nums = {4, 5, 6, 7, 15, 8, 20, 3, 46, 9, 10, 11, 12, 13, 14, 0},
        .flags.fb_in_psram = true,
    };
    st7701_vendor_config_t vendor = {
        .rgb_config = &rgb,
        .init_cmds = s_guition_init,
        .init_cmds_size = sizeof(s_guition_init) / sizeof(s_guition_init[0]),
        .flags = {
            .mirror_by_cmd = 1,
            .enable_io_multiplex = 0,
        },
    };
    const esp_lcd_panel_dev_config_t panel = {
        .reset_gpio_num = -1,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 18,
        .vendor_config = &vendor,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7701(io, &panel, &s_panel), TAG, "ST7701 panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "panel reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "panel on");
    return ESP_OK;
}

static esp_err_t touch_at_address(uint8_t address)
{
    esp_lcd_panel_io_i2c_config_t io_config = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    io_config.dev_addr = address;
    io_config.scl_speed_hz = 0;
    esp_lcd_panel_io_handle_t io = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c_v1(I2C_NUM_0, &io_config, &io), TAG, "GT911 IO");
    esp_lcd_touch_io_gt911_config_t gt911 = {
        .dev_addr = address,
    };
    esp_lcd_touch_config_t config = {
        .x_max = BOARD_HRES,
        .y_max = BOARD_VRES,
        .rst_gpio_num = -1,
        .int_gpio_num = -1,
        .levels = {.reset = 0, .interrupt = 0},
        .flags = {.swap_xy = 0, .mirror_x = 0, .mirror_y = 0},
        .driver_data = &gt911,
    };
    esp_err_t err = esp_lcd_touch_new_i2c_gt911(io, &config, &s_touch);
    if (err != ESP_OK) {
        esp_lcd_panel_io_del(io);
        return err;
    }
    return ESP_OK;
}

static esp_err_t touch_init(void)
{
    const i2c_config_t config = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = 19,
        .scl_io_num = 45,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_param_config(I2C_NUM_0, &config), TAG, "touch I2C config");
    ESP_RETURN_ON_ERROR(i2c_driver_install(I2C_NUM_0, I2C_MODE_MASTER, 0, 0, 0), TAG, "touch I2C driver");
    esp_err_t err = touch_at_address(0x5d);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "GT911 not found at 0x5d; trying 0x14");
        err = touch_at_address(0x14);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GT911 initialization failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t board_init(void)
{
    ESP_RETURN_ON_ERROR(backlight_init(), TAG, "backlight init");
    ESP_RETURN_ON_ERROR(display_init(), TAG, "display init");
    esp_err_t touch = touch_init();
    if (touch != ESP_OK) {
        ESP_LOGW(TAG, "Continuing without touch; use serial logs for diagnosis");
    }
    /* Dim until Wi-Fi has finished its RF calibration burst; app_main ramps
     * to the saved brightness. Full backlight + calibration browned out the
     * board on a weak USB supply. */
    ESP_RETURN_ON_ERROR(board_set_backlight(BOARD_BOOT_BACKLIGHT), TAG, "backlight on");
    ESP_LOGI(TAG, "ST7701 RGB565 480x480, Guition init, PCLK=12MHz, two PSRAM frames, GT911 %s",
             s_touch ? "ready" : "unavailable");
    return ESP_OK;
}

esp_lcd_panel_handle_t board_panel(void) { return s_panel; }
esp_lcd_touch_handle_t board_touch(void) { return s_touch; }
