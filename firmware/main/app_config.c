#include "app_config.h"

#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

#ifdef CONFIG_CKB_DEMO_MODE
#define CKB_DEMO_DEFAULT true
#else
#define CKB_DEMO_DEFAULT false
#endif

static const char *TAG = "config";
static app_config_t s_config;
static SemaphoreHandle_t s_lock;

static void lock(void) { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_lock); }

static void copy_string(char *dst, size_t size, const char *src)
{
    snprintf(dst, size, "%s", src ? src : "");
}

static void load_defaults(void)
{
    memset(&s_config, 0, sizeof(s_config));
    copy_string(s_config.ssid, sizeof(s_config.ssid), CONFIG_CKB_WIFI_SSID);
    copy_string(s_config.password, sizeof(s_config.password), CONFIG_CKB_WIFI_PASSWORD);
    copy_string(s_config.rpc_url, sizeof(s_config.rpc_url), CONFIG_CKB_RPC_URL);
    copy_string(s_config.chain, sizeof(s_config.chain), CONFIG_CKB_CHAIN);
    s_config.demo = CKB_DEMO_DEFAULT;
    s_config.public_rpc = CONFIG_CKB_RPC_PUBLIC;
    s_config.brightness = 90;
}

esp_err_t app_config_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        return err;
    }
    load_defaults();
    nvs_handle_t nvs;
    err = nvs_open("ckb", NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    size_t len;
    uint8_t byte;
    if (nvs_get_str(nvs, "ssid", s_config.ssid, &(size_t){sizeof(s_config.ssid)}) == ESP_ERR_NVS_INVALID_LENGTH) {
        nvs_close(nvs);
        return ESP_ERR_INVALID_SIZE;
    }
    len = sizeof(s_config.password);
    nvs_get_str(nvs, "password", s_config.password, &len);
    len = sizeof(s_config.rpc_url);
    nvs_get_str(nvs, "rpc_url", s_config.rpc_url, &len);
    len = sizeof(s_config.chain);
    nvs_get_str(nvs, "chain", s_config.chain, &len);
    if (nvs_get_u8(nvs, "demo", &byte) == ESP_OK) {
        s_config.demo = byte != 0;
    } else {
        nvs_set_u8(nvs, "demo", s_config.demo);
    }
    if (nvs_get_u8(nvs, "brightness", &byte) == ESP_OK) {
        s_config.brightness = byte > 100 ? 100 : byte;
    }
    if (nvs_get_u8(nvs, "public_rpc", &byte) == ESP_OK) {
        s_config.public_rpc = byte != 0;
    }
    uint8_t config_version = 0;
    nvs_get_u8(nvs, "config_version", &config_version);
    if (config_version < 2) {
        s_config.demo = CKB_DEMO_DEFAULT;
        nvs_set_u8(nvs, "demo", s_config.demo);
        nvs_set_u8(nvs, "config_version", 2);
    }
    if (nvs_get_str(nvs, "ssid", NULL, &(size_t){0}) == ESP_ERR_NVS_NOT_FOUND) {
        nvs_set_str(nvs, "ssid", s_config.ssid);
        nvs_set_str(nvs, "password", s_config.password);
        nvs_set_str(nvs, "rpc_url", s_config.rpc_url);
        nvs_set_str(nvs, "chain", s_config.chain);
        nvs_set_u8(nvs, "public_rpc", s_config.public_rpc);
    }
    err = nvs_commit(nvs);
    nvs_close(nvs);
    if (strcmp(s_config.chain, "mainnet") != 0 && strcmp(s_config.chain, "testnet") != 0) {
        ESP_LOGE(TAG, "Invalid chain setting; refusing network input");
        copy_string(s_config.chain, sizeof(s_config.chain), "invalid");
    }
    return err;
}

app_config_t *app_config(void)
{
    return &s_config;
}

void app_config_copy(app_config_t *out)
{
    lock();
    *out = s_config;
    unlock();
}

esp_err_t app_config_set_demo(bool demo)
{
    s_config.demo = demo;
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("ckb", NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs, "demo", demo);
        if (err == ESP_OK) {
            err = nvs_commit(nvs);
        }
        nvs_close(nvs);
    }
    return err;
}

esp_err_t app_config_set_brightness(uint8_t percent)
{
    s_config.brightness = percent > 100 ? 100 : percent;
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("ckb", NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs, "brightness", s_config.brightness);
        if (err == ESP_OK) {
            err = nvs_commit(nvs);
        }
        nvs_close(nvs);
    }
    return err;
}

esp_err_t app_config_set_network(const char *ssid, const char *password,
                                 const char *rpc_url, bool public_rpc)
{
    if (!ssid || !password || !rpc_url || strlen(ssid) > APP_SSID_LEN ||
        strlen(password) > APP_PASSWORD_LEN || strlen(rpc_url) > APP_URL_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    copy_string(s_config.ssid, sizeof(s_config.ssid), ssid);
    copy_string(s_config.password, sizeof(s_config.password), password);
    copy_string(s_config.rpc_url, sizeof(s_config.rpc_url), rpc_url);
    s_config.public_rpc = public_rpc;
    s_config.demo = false;
    unlock();
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("ckb", NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        nvs_set_str(nvs, "ssid", s_config.ssid);
        nvs_set_str(nvs, "password", s_config.password);
        nvs_set_str(nvs, "rpc_url", s_config.rpc_url);
        nvs_set_u8(nvs, "public_rpc", public_rpc);
        nvs_set_u8(nvs, "demo", 0);
        nvs_set_u8(nvs, "config_version", 2);
        err = nvs_commit(nvs);
        nvs_close(nvs);
    }
    return err;
}

const char *app_config_expected_genesis(void)
{
    if (strcmp(s_config.chain, "mainnet") == 0) {
        return "0x92b197aa1fba0f63633922c61c92375c9c074a93e85963554f5499fe1450d0e5";
    }
    if (strcmp(s_config.chain, "testnet") == 0) {
        return "0x10639e0895502b5688a6be8cf69460d76541bfa4821629d86d62ba0aae3f9606";
    }
    return NULL;
}
