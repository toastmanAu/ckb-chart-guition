#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

esp_err_t dashboard_ui_start(QueueHandle_t snapshots, QueueHandle_t controls);
