#pragma once

#include "freertos/FreeRTOS.h"
#include <stdbool.h>
#include "freertos/queue.h"

esp_err_t network_start(QueueHandle_t snapshots, QueueHandle_t controls);

/* True while the station holds an IP (any task). */
bool network_wifi_up(void);
