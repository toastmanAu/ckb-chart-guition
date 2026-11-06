#include "rpc_client.h"

#include <stdio.h>
#include <string.h>
#include "app_config.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "metric_core.h"

static char *s_body;
static size_t s_body_len;
static bool s_overflow;
static char s_error[96];
static uint32_t s_request_id;

static void *json_psram_malloc(size_t size)
{
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static esp_err_t on_http(esp_http_client_event_t *event)
{
    if (event->event_id == HTTP_EVENT_ON_DATA) {
        if (!event->user_data || event->data_len < 0) {
            return ESP_FAIL;
        }
        if (s_body_len + (size_t)event->data_len >= RPC_RESPONSE_CAP) {
            s_overflow = true;
            return ESP_FAIL;
        }
        memcpy(s_body + s_body_len, event->data, (size_t)event->data_len);
        s_body_len += (size_t)event->data_len;
    }
    return ESP_OK;
}

esp_err_t rpc_client_init(void)
{
    if (s_body) {
        return ESP_OK;
    }
    s_body = heap_caps_malloc(RPC_RESPONSE_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_body) {
        snprintf(s_error, sizeof(s_error), "Cannot allocate RPC response buffer");
        return ESP_ERR_NO_MEM;
    }
    cJSON_Hooks hooks = {
        .malloc_fn = json_psram_malloc,
        .free_fn = heap_caps_free,
    };
    cJSON_InitHooks(&hooks);
    return ESP_OK;
}

const char *rpc_last_error(void)
{
    return s_error;
}

cJSON *rpc_call(const char *method, const char *params_json)
{
    if (!s_body || !method || !params_json) {
        return NULL;
    }
    s_body_len = 0;
    s_overflow = false;
    s_error[0] = '\0';
    char request[512];
    int request_len = snprintf(request, sizeof(request),
        "{\"jsonrpc\":\"2.0\",\"id\":%lu,\"method\":\"%s\",\"params\":%s}",
        (unsigned long)++s_request_id, method, params_json);
    if (request_len <= 0 || request_len >= (int)sizeof(request)) {
        snprintf(s_error, sizeof(s_error), "RPC request too large");
        return NULL;
    }

    /* Copy under the config lock: the UI task may rewrite the URL mid-call. */
    app_config_t cfg;
    app_config_copy(&cfg);
    memset(cfg.password, 0, sizeof(cfg.password));
    esp_http_client_config_t config = {
        .url = cfg.rpc_url,
        .event_handler = on_http,
        /* on_http() rejects data when user_data is NULL; without this every
         * response body was dropped and each call failed as "HTTP 200". */
        .user_data = s_body,
        .timeout_ms = 8000,
        .method = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        snprintf(s_error, sizeof(s_error), "HTTP client init failed");
        return NULL;
    }
    esp_err_t err = esp_http_client_set_header(client, "Content-Type", "application/json");
    if (err == ESP_OK) {
        err = esp_http_client_set_header(client, "Accept", "application/json");
    }
    if (err == ESP_OK) {
        err = esp_http_client_set_header(client, "User-Agent", "ckb-chart-esp32/1.0");
    }
    if (err == ESP_OK) {
        err = esp_http_client_set_post_field(client, request, request_len);
    }
    if (err == ESP_OK) {
        err = esp_http_client_perform(client);
    }
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || status != 200 || s_overflow || s_body_len == 0) {
        snprintf(s_error, sizeof(s_error), "%s (HTTP %d%s)",
                 esp_err_to_name(err), status, s_overflow ? ", body cap" : "");
        return NULL;
    }
    s_body[s_body_len] = '\0';
    cJSON *root = cJSON_ParseWithLength(s_body, s_body_len);
    if (!root) {
        snprintf(s_error, sizeof(s_error), "Malformed JSON");
        return NULL;
    }
    const cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
    if (cJSON_IsObject(error)) {
        const cJSON *message = cJSON_GetObjectItemCaseSensitive(error, "message");
        snprintf(s_error, sizeof(s_error), "RPC: %.70s",
                 cJSON_IsString(message) ? message->valuestring : "request rejected");
        cJSON_Delete(root);
        return NULL;
    }
    if (!cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(root, "result"))) {
        const cJSON *result = cJSON_GetObjectItemCaseSensitive(root, "result");
        if (!result || (!cJSON_IsString(result) && !cJSON_IsArray(result))) {
            snprintf(s_error, sizeof(s_error), "Missing RPC result");
            cJSON_Delete(root);
            return NULL;
        }
    }
    return root;
}

const cJSON *rpc_result(const cJSON *root)
{
    return root ? cJSON_GetObjectItemCaseSensitive(root, "result") : NULL;
}

bool rpc_json_u64(const cJSON *object, const char *key, uint64_t *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(item) && ckb_parse_hex_u64(item->valuestring, out);
}

bool rpc_json_string(const cJSON *object, const char *key, char *out, size_t size)
{
    if (!out || size == 0) {
        return false;
    }
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsString(item)) {
        out[0] = '\0';
        return false;
    }
    snprintf(out, size, "%s", item->valuestring);
    return true;
}
