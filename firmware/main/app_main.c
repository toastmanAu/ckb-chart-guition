#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_core_dump.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "app_config.h"
#include "app_types.h"
#include "board.h"
#include "dashboard_ui.h"
#include "lc_task.h"
#include "network.h"

static const char *TAG = "app";

void app_main(void)
{
    ESP_LOGI(TAG, "CKB Chart for Guition ESP32-4848S040C; ESP-IDF %s", esp_get_idf_version());
    esp_reset_reason_t reason = esp_reset_reason();
    static const char *REASONS[] = {"unknown", "power-on", "external", "software", "panic",
                                    "interrupt WDT", "task WDT", "other WDT", "deep sleep",
                                    "brownout", "SDIO", "USB", "JTAG", "eFuse", "power glitch", "CPU lockup"};
    const char *reason_text = (unsigned)reason < sizeof(REASONS) / sizeof(REASONS[0]) ? REASONS[reason] : "?";
    if (reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT || reason == ESP_RST_TASK_WDT ||
        reason == ESP_RST_WDT || reason == ESP_RST_BROWNOUT) {
        ESP_LOGE(TAG, "Previous run ended abnormally: reset reason %d (%s)", (int)reason, reason_text);
    } else {
        ESP_LOGI(TAG, "Reset reason %d (%s)", (int)reason, reason_text);
    }
    if (esp_core_dump_image_check() == ESP_OK) {
        ESP_LOGE(TAG, "A crash core dump is stored in flash; read it with: "
                      "idf.py -C firmware coredump-info");
    }
    ESP_ERROR_CHECK(app_config_init());
    ESP_ERROR_CHECK(board_init());

    /* 8 x ~3.7 KiB snapshots: keep the storage in PSRAM so the ~30 KiB does
     * not compete with Wi-Fi and TLS for internal RAM. */
    QueueHandle_t snapshots = xQueueCreateWithCaps(8, sizeof(dashboard_snapshot_t),
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    QueueHandle_t controls = xQueueCreate(4, sizeof(control_event_t));
    if (!snapshots || !controls) {
        ESP_LOGE(TAG, "Cannot allocate bounded task queues");
        abort();
    }
    ESP_ERROR_CHECK(dashboard_ui_start(snapshots, controls));
    esp_err_t network = network_start(snapshots, controls);
    if (network != ESP_OK) {
        ESP_LOGE(TAG, "Network worker unavailable: %s", esp_err_to_name(network));
    } else {
        esp_err_t lc = lc_task_start(network_wifi_up);
        if (lc != ESP_OK) {
            ESP_LOGE(TAG, "Light client unavailable: %s", esp_err_to_name(lc));
        }
    }
    ESP_LOGI(TAG, "Startup complete; mode=%s chain=%s", app_config()->demo ? "DEMO" : "RPC",
             app_config()->chain);
    /* Soft-start the backlight once the Wi-Fi bring-up peak (~1.3-1.6 s after
     * reset) has passed, so the two current peaks never overlap. */
    vTaskDelay(pdMS_TO_TICKS(2500));
    uint8_t target = app_config()->brightness;
    for (uint8_t level = BOARD_BOOT_BACKLIGHT; level < target; level += 5) {
        board_set_backlight(level);
        vTaskDelay(pdMS_TO_TICKS(40));
    }
    board_set_backlight(target);

#if CONFIG_CKB_DEBUG_PANIC_TEST
    /* Bench test of the crash-recovery path. Only fires after a power-on or
     * external reset, so the reboot that follows (reason = panic) runs
     * normally instead of looping. A panic reset does not reset the RGB LCD
     * peripheral or the panel, unlike the EN-pin reset that serial tools do. */
    if (reason == ESP_RST_POWERON || reason == ESP_RST_EXT) {
        ESP_LOGW(TAG, "DEBUG: deliberate panic in 60 s to test crash recovery");
        vTaskDelay(pdMS_TO_TICKS(60000));
        abort();
    }
#endif
}
