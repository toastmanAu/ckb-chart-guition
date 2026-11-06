#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "cJSON.h"
#include "esp_err.h"

#define RPC_RESPONSE_CAP (768 * 1024)

esp_err_t rpc_client_init(void);
cJSON *rpc_call(const char *method, const char *params_json);
const cJSON *rpc_result(const cJSON *root);
bool rpc_json_u64(const cJSON *object, const char *key, uint64_t *out);
bool rpc_json_string(const cJSON *object, const char *key, char *out, size_t size);
const char *rpc_last_error(void);
