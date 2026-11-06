#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define APP_SSID_LEN 32
#define APP_PASSWORD_LEN 64
#define APP_URL_LEN 160

typedef struct {
    char ssid[APP_SSID_LEN + 1];
    char password[APP_PASSWORD_LEN + 1];
    char rpc_url[APP_URL_LEN + 1];
    char chain[16];
    bool demo;
    bool public_rpc;
    uint8_t brightness;
} app_config_t;

esp_err_t app_config_init(void);
/* Pointer access is safe for fields that never change after init (chain) and
 * for single-byte flags. Other tasks must use app_config_copy() for strings,
 * because the UI task rewrites them while provisioning. */
app_config_t *app_config(void);
void app_config_copy(app_config_t *out);
esp_err_t app_config_set_demo(bool demo);
esp_err_t app_config_set_brightness(uint8_t percent);
esp_err_t app_config_set_network(const char *ssid, const char *password,
                                 const char *rpc_url, bool public_rpc);
const char *app_config_expected_genesis(void);
