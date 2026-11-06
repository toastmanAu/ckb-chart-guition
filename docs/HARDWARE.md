# Guition ESP32-4848S040C hardware bring-up

Manufacturer lists ESP32-S3R8, **480×480**, 8MB PSRAM, 16MB flash, capacitive touch. Cross-reference installed board/revision; do not blindly assume suffix is interchangeable. Guition component registry identifies `esp32_4848s040` (ST7701 + 3-wire init and RGB + GT911/none).

Reference implementation reported: RGB interface 16-bit ST7701, init CS=39, SCK=48, MOSI=47; DE=18, VSYNC=17, HSYNC=16, PCLK=21. RGB red GPIO 11,12,13,14,0; green 8,20,3,46,9,10; blue 4,5,6,7,15. Backlight GPIO38. GT911 touch SDA=19, SCL=45, address 0x5D or 0x14. **These are reference candidates, verify against exact board's Guition driver and PCB before coding.**

Critical: GPIO0 / GPIO45/46 are strapping-sensitive; do not repurpose. 9-bit ST7701 init command format/sequence and GPIO timing must match panel. Several field reports mention 8–10MHz pixel clock to mitigate instability with Wi-Fi/PSRAM; prefer vendor-tested initial timing and then benchmark. GT911 may need touch axes adjustment or calibration.

Recommended ESP-IDF path: use Guition official registry board component when supported; otherwise `esp_lcd` RGB + verified ST7701 init + GT911 driver. LVGL 9 draw buffer strategy, RGB flush synchronization and PSRAM DMA constraints require actual hardware test.

**Acceptance**: boot splash correct colors no inversion, animated chart no significant flicker, five-point touch calibration, Wi-Fi under full-display refresh, 30min power/touch run, reboot resume. Record internal free heap, PSRAM heap and minimum free heap after UI startup, during network polling and backfill.

References:
- https://www.guition.com/esp32-display-module/4-inch-esp32s3-display-module
- https://components.espressif.com/components/guition/guition_boards
- https://github.com/jantielens/esp32-macropad/blob/main/docs/dev/esp32-4848s040.md
- https://lcdwizard.com/boards/guition-4848s040
