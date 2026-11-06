#include "network.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include "app_config.h"
#include "app_types.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lc_badge.h"
#include "lc_task.h"
#include "rpc_client.h"

#define WIFI_CONNECTED BIT0
#define WIFI_SCAN_DONE BIT1
#define WIFI_DISCONNECTED BIT2
#define DEMO_BASE_MS 1793650000000ULL
#define SCAN_RECORD_CAP 20
#define SCAN_TIMEOUT_MS 15000
#define CONNECT_TIMEOUT_MS 20000
#define BACKOFF_MIN_MS 1000
#define BACKOFF_MAX_MS 30000

static const char *TAG = "network";
static EventGroupHandle_t s_wifi_events;
static QueueHandle_t s_snapshots;
static QueueHandle_t s_controls;
static dashboard_snapshot_t s_last;
static char s_last_tip_hash[HASH_TEXT_LEN];

/* Working buffers. Every snapshot is ~3.7 KiB; as stack locals they were
 * inlined into network_task and produced an 8.3 KiB frame inside a 12 KiB
 * stack, leaving too little for the TLS handshake. They are only touched by
 * the network task, so static storage is safe. */
static dashboard_snapshot_t s_work;
static dashboard_snapshot_t s_status_snapshot;
static dashboard_snapshot_t s_discard;
static wifi_ap_record_t s_records[SCAN_RECORD_CAP];

/* Wi-Fi bring-up stages, so a failed ensure_wifi() can be retried without
 * re-creating the netif or re-registering handlers. */
static bool s_netif_ready;
static bool s_wifi_inited;
static bool s_wifi_started;

/* Station connection state, owned by the network task. */
static bool s_connecting;
static uint64_t s_connect_started_ms;
static uint64_t s_next_connect_ms;
static uint32_t s_backoff_ms = BACKOFF_MIN_MS;
static volatile uint8_t s_disconnect_reason;
static char s_wifi_problem[40];

/* Scan state, stamped onto every posted snapshot. */
static bool s_scanning;
static uint64_t s_scan_started_ms;
static uint32_t s_scan_seq;
static bool s_scan_ok;
static uint8_t s_scan_count;
static char s_scan_ssids[SCAN_MAX_RESULTS][33];
static int8_t s_scan_rssi[SCAN_MAX_RESULTS];

static uint64_t monotonic_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

/* Cross-check of the RPC endpoint against the light client's proven tip,
 * owned by the network task. */
static lc_check_t s_lc_check;
static uint32_t s_lc_differ_streak;
static uint64_t s_lc_checked_height;

bool network_wifi_up(void)
{
    return s_wifi_events && (xEventGroupGetBits(s_wifi_events) & WIFI_CONNECTED);
}

static void hash_hex(const uint8_t hash[32], char out[HASH_TEXT_LEN])
{
    out[0] = '0';
    out[1] = 'x';
    for (int i = 0; i < 32; ++i) {
        snprintf(out + 2 + 2 * i, 3, "%02x", hash[i]);
    }
}

/* Stamp the light client's state and the trust badge onto a snapshot. */
static void stamp_light_client(dashboard_snapshot_t *snapshot)
{
    lc_status_t lc;
    lc_task_status(&lc);
    uint64_t now = monotonic_ms();
    uint64_t age = lc.has_tip && now >= lc.tip_proven_ms ? now - lc.tip_proven_ms : 0;
    const lc_badge_input_t in = {
        .enabled = lc.state != LC_STATE_DISABLED && !snapshot->demo,
        .connected = lc.state == LC_STATE_ANCHORING || lc.state == LC_STATE_PROVEN,
        .has_proof = lc.has_tip,
        .proof_age_ms = age,
        .check = s_lc_check,
        .differ_streak = s_lc_differ_streak,
    };
    snapshot->lc_badge = (uint8_t)lc_badge_decide(&in);
    snapshot->lc_state = (uint8_t)lc.state;
    snapshot->lc_check = (uint8_t)s_lc_check;
    snapshot->lc_has_tip = lc.has_tip;
    snapshot->lc_height = lc.tip_number;
    snapshot->lc_age_s = (uint32_t)(age / 1000);
    snapshot->lc_checkpoint = lc.checkpoint;
    snapshot->lc_sessions = lc.sessions;
    if (lc.has_tip) {
        hash_hex(lc.tip_hash, snapshot->lc_hash);
    } else {
        snapshot->lc_hash[0] = '\0';
    }
}

/* Does the RPC endpoint report the proven block at the proven height? The
 * recent window usually holds it; otherwise one get_block_hash. An RPC tip
 * below the proven height cannot answer (UNKNOWN), and neither can a failed
 * call: only a hash that DIFFERS counts against the endpoint. */
static void cross_check(const dashboard_snapshot_t *snapshot)
{
    lc_status_t lc;
    lc_task_status(&lc);
    if (!lc.has_tip || lc.tip_number > snapshot->tip_height) {
        s_lc_check = LC_CHECK_UNKNOWN;
        return;
    }
    char proven[HASH_TEXT_LEN];
    hash_hex(lc.tip_hash, proven);
    const char *rpc_hash = NULL;
    for (uint32_t i = 0; i < snapshot->block_count; ++i) {
        if (snapshot->blocks[i].valid && snapshot->blocks[i].height == lc.tip_number) {
            rpc_hash = snapshot->blocks[i].hash;
            break;
        }
    }
    char fetched[HASH_TEXT_LEN];
    if (!rpc_hash) {
        char params[32];
        snprintf(params, sizeof(params), "[\"0x%llx\"]", (unsigned long long)lc.tip_number);
        cJSON *root = rpc_call("get_block_hash", params);
        const cJSON *result = root ? rpc_result(root) : NULL;
        if (cJSON_IsString(result) && strlen(result->valuestring) == 66) {
            snprintf(fetched, sizeof(fetched), "%s", result->valuestring);
            rpc_hash = fetched;
        }
        if (root) {
            cJSON_Delete(root);
        }
    }
    if (!rpc_hash) {
        s_lc_check = LC_CHECK_UNKNOWN;
        return;
    }
    if (strcasecmp(rpc_hash, proven) == 0) {
        s_lc_check = LC_CHECK_MATCH;
        s_lc_differ_streak = 0;
    } else {
        s_lc_check = LC_CHECK_DIFFER;
        /* One comparison per proven height: a poll that sees the same pair
         * again is not new evidence. */
        if (lc.tip_number != s_lc_checked_height || s_lc_differ_streak == 0) {
            s_lc_differ_streak++;
        }
        ESP_LOGW(TAG, "RPC hash at proven height %llu differs: rpc %.18s... proven %.18s... (%u in a row)",
                 (unsigned long long)lc.tip_number, rpc_hash, proven, (unsigned)s_lc_differ_streak);
    }
    s_lc_checked_height = lc.tip_number;
}

static void post_snapshot(dashboard_snapshot_t *snapshot)
{
    stamp_light_client(snapshot);
    snapshot->scan_seq = s_scan_seq;
    snapshot->scanning = s_scanning;
    snapshot->scan_ok = s_scan_ok;
    snapshot->scan_count = s_scan_count;
    memcpy(snapshot->scan_ssids, s_scan_ssids, sizeof(s_scan_ssids));
    memcpy(snapshot->scan_rssi, s_scan_rssi, sizeof(s_scan_rssi));
    if (xQueueSend(s_snapshots, snapshot, 0) != pdTRUE) {
        xQueueReceive(s_snapshots, &s_discard, 0);
        xQueueSend(s_snapshots, snapshot, 0);
    }
}

/* Post the last good data with a new status line, so the UI keeps showing
 * the previous chain data (marked stale) while Wi-Fi is being sorted out. */
static void post_status(const char *status, bool online)
{
    s_status_snapshot = s_last;
    s_status_snapshot.online = online;
    s_status_snapshot.stale = s_last.has_tip;
    s_status_snapshot.source = app_config()->public_rpc ? SOURCE_PUBLIC_RPC : SOURCE_FULL_NODE_RPC;
    s_status_snapshot.capture_ms = s_last.has_tip ? s_last.capture_ms : monotonic_ms();
    snprintf(s_status_snapshot.status, sizeof(s_status_snapshot.status), "%s", status);
    post_snapshot(&s_status_snapshot);
}

static const char *disconnect_reason_text(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_NO_AP_FOUND:
        return "network not found";
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_MIC_FAILURE:
        return "wrong password?";
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        return "unsupported security mode";
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        return "signal too weak";
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_ASSOC_FAIL:
    case WIFI_REASON_BEACON_TIMEOUT:
    case WIFI_REASON_CONNECTION_FAIL:
        return "access point not responding";
    default:
        return "disconnected";
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event = data;
        s_disconnect_reason = event ? event->reason : 0;
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED);
        xEventGroupSetBits(s_wifi_events, WIFI_DISCONNECTED);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        xEventGroupSetBits(s_wifi_events, WIFI_SCAN_DONE);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = data;
        ESP_LOGI(TAG, "Wi-Fi connected, IP " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) {
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED);
    }
}

static esp_err_t apply_sta_config(void)
{
    app_config_t cfg;
    app_config_copy(&cfg);
    wifi_config_t config = {0};
    memcpy(config.sta.ssid, cfg.ssid, strnlen(cfg.ssid, sizeof(config.sta.ssid)));
    memcpy(config.sta.password, cfg.password, strnlen(cfg.password, sizeof(config.sta.password)));
    /* WPA2 is the minimum when a password is given (WPA3 is accepted too);
     * an empty password means the user picked an open network. */
    config.sta.threshold.authmode = cfg.password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    config.sta.pmf_cfg.capable = true;
    config.sta.pmf_cfg.required = false;
    config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &config);
    memset(&config, 0, sizeof(config));
    memset(cfg.password, 0, sizeof(cfg.password));
    return err;
}

static esp_err_t ensure_wifi(void)
{
    if (s_wifi_started) {
        return ESP_OK;
    }
    if (!s_netif_ready) {
        ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
        esp_err_t event_loop = esp_event_loop_create_default();
        if (event_loop != ESP_OK && event_loop != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "event loop: %s", esp_err_to_name(event_loop));
            return event_loop;
        }
        if (!esp_netif_create_default_wifi_sta()) {
            return ESP_FAIL;
        }
        s_netif_ready = true;
    }
    if (!s_wifi_inited) {
        wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
        ESP_RETURN_ON_ERROR(esp_wifi_init(&init), TAG, "Wi-Fi init");
        /* Credentials live in our own NVS namespace. Keeping the driver's copy
         * in RAM stops esp_wifi_set_config() from writing flash, which stalls
         * the PSRAM-backed RGB panel and can resurrect stale credentials. */
        ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "Wi-Fi storage");
        ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        wifi_event_handler, NULL), TAG, "Wi-Fi events");
        ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                                        wifi_event_handler, NULL), TAG, "IP events");
        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "Wi-Fi mode");
        s_wifi_inited = true;
    }
    ESP_RETURN_ON_ERROR(apply_sta_config(), TAG, "Wi-Fi config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "Wi-Fi start");
    s_wifi_started = true;
    ESP_LOGI(TAG, "Wi-Fi started; provisioning scan available");
    return ESP_OK;
}

/* Abort an in-flight connection attempt. The resulting DISCONNECTED event
 * (reason ASSOC_LEAVE) is treated as benign by wifi_service(). */
static void stop_connecting(void)
{
    if (s_wifi_started) {
        esp_wifi_disconnect();
    }
    s_connecting = false;
}

static void reset_backoff(void)
{
    s_backoff_ms = BACKOFF_MIN_MS;
    s_next_connect_ms = 0;
}

/* Drive the station connection from events instead of calling
 * esp_wifi_connect() on every loop pass, which aborted handshakes that
 * needed longer than the first backoff step. Returns true once an IP is held. */
static bool wifi_service(void)
{
    uint64_t now = monotonic_ms();
    EventBits_t bits = xEventGroupGetBits(s_wifi_events);
    if (bits & WIFI_DISCONNECTED) {
        xEventGroupClearBits(s_wifi_events, WIFI_DISCONNECTED);
        uint8_t reason = s_disconnect_reason;
        s_connecting = false;
        if (reason != WIFI_REASON_ASSOC_LEAVE) {
            snprintf(s_wifi_problem, sizeof(s_wifi_problem), "%s", disconnect_reason_text(reason));
            ESP_LOGW(TAG, "Wi-Fi disconnected, reason %u (%s); retry in %lu ms",
                     reason, s_wifi_problem, (unsigned long)s_backoff_ms);
            s_next_connect_ms = now + s_backoff_ms + esp_random() % 500;
            s_backoff_ms = s_backoff_ms < BACKOFF_MAX_MS / 2 ? s_backoff_ms * 2 : BACKOFF_MAX_MS;
        }
    }
    if (bits & WIFI_CONNECTED) {
        s_connecting = false;
        s_wifi_problem[0] = '\0';
        reset_backoff();
        return true;
    }
    if (s_scanning || app_config()->ssid[0] == '\0') {
        return false;
    }
    if (s_connecting && now - s_connect_started_ms > CONNECT_TIMEOUT_MS) {
        ESP_LOGW(TAG, "Wi-Fi connect attempt timed out");
        snprintf(s_wifi_problem, sizeof(s_wifi_problem), "connect timed out");
        stop_connecting();
        s_next_connect_ms = now + s_backoff_ms;
        return false;
    }
    if (!s_connecting && now >= s_next_connect_ms) {
        esp_err_t err = esp_wifi_connect();
        if (err == ESP_OK) {
            s_connecting = true;
            s_connect_started_ms = now;
        } else {
            ESP_LOGW(TAG, "esp_wifi_connect: %s", esp_err_to_name(err));
            s_next_connect_ms = now + s_backoff_ms;
        }
    }
    return false;
}

static bool parse_tip(const cJSON *root, dashboard_snapshot_t *snapshot)
{
    const cJSON *tip = rpc_result(root);
    if (!cJSON_IsObject(tip) ||
        !rpc_json_u64(tip, "number", &snapshot->tip_height) ||
        !rpc_json_u64(tip, "timestamp", &snapshot->tip_timestamp_ms)) {
        return false;
    }
    char hash[HASH_TEXT_LEN];
    if (!rpc_json_string(tip, "hash", hash, sizeof(hash)) || strlen(hash) != 66) {
        return false;
    }
    uint64_t compact;
    if (rpc_json_u64(tip, "compact_target", &compact) && compact <= UINT32_MAX) {
        snapshot->difficulty = ckb_compact_target_difficulty(compact);
        snapshot->has_difficulty = isfinite(snapshot->difficulty);
    }
    /* Into the NEW snapshot: writing s_last here made every poll look like a
     * tip change and every new block look like a reorg. */
    memcpy(snapshot->blocks[0].hash, hash, sizeof(hash));
    snapshot->has_tip = true;
    return true;
}

static bool verify_chain(dashboard_snapshot_t *snapshot)
{
    cJSON *root = rpc_call("get_blockchain_info", "[]");
    if (!root) {
        return false;
    }
    const cJSON *info = rpc_result(root);
    char rpc_chain[16];
    bool ok = rpc_json_string(info, "chain", rpc_chain, sizeof(rpc_chain));
    cJSON_Delete(root);
    if (!ok) {
        return false;
    }
    const char *expected_rpc_chain = strcmp(app_config()->chain, "mainnet") == 0 ? "ckb" : "ckb_testnet";
    if (strcmp(app_config()->chain, "mainnet") != 0 && strcmp(app_config()->chain, "testnet") != 0) {
        snapshot->chain_mismatch = true;
        return false;
    }
    if (strcmp(rpc_chain, expected_rpc_chain) != 0) {
        snapshot->chain_mismatch = true;
        snprintf(snapshot->status, sizeof(snapshot->status), "Chain mismatch: endpoint %.12s", rpc_chain);
        return false;
    }
    snprintf(snapshot->chain, sizeof(snapshot->chain), "%s", app_config()->chain);
    const char *expected_genesis = app_config_expected_genesis();
    if (expected_genesis) {
        root = rpc_call("get_block_hash", "[\"0x0\"]");
        if (!root) {
            return false;
        }
        const cJSON *hash = rpc_result(root);
        ok = cJSON_IsString(hash) && strcasecmp(hash->valuestring, expected_genesis) == 0;
        cJSON_Delete(root);
        if (!ok) {
            snapshot->chain_mismatch = true;
            snprintf(snapshot->status, sizeof(snapshot->status), "Genesis hash mismatch");
            return false;
        }
    }
    return true;
}

static bool fetch_block(uint64_t height, rpc_block_summary_t *summary)
{
    char params[32];
    snprintf(params, sizeof(params), "[\"0x%llx\"]", (unsigned long long)height);
    cJSON *root = rpc_call("get_block_by_number", params);
    if (!root) {
        return false;
    }
    const cJSON *block = rpc_result(root);
    const cJSON *header = cJSON_GetObjectItemCaseSensitive(block, "header");
    const cJSON *transactions = cJSON_GetObjectItemCaseSensitive(block, "transactions");
    uint64_t compact;
    bool ok = cJSON_IsObject(header) && cJSON_IsArray(transactions) &&
              rpc_json_u64(header, "number", &summary->height) &&
              rpc_json_u64(header, "timestamp", &summary->timestamp_ms) &&
              rpc_json_string(header, "hash", summary->hash, sizeof(summary->hash)) &&
              rpc_json_string(header, "parent_hash", summary->parent_hash, sizeof(summary->parent_hash));
    if (ok && summary->height != height) {
        ok = false;
    }
    if (ok) {
        int count = cJSON_GetArraySize(transactions);
        if (count < 0) {
            ok = false;
        } else {
            summary->tx_count = (uint32_t)count;
        }
    }
    if (ok && rpc_json_u64(header, "compact_target", &compact) && compact <= UINT32_MAX) {
        summary->compact_target = (uint32_t)compact;
    }
    summary->valid = ok;
    cJSON_Delete(root);
    return ok;
}

static void backfill(dashboard_snapshot_t *snapshot)
{
    snapshot->block_count = 0;
    uint64_t start = snapshot->tip_height;
    for (uint32_t i = 0; i < RPC_BLOCK_BATCH && start >= i; ++i) {
        rpc_block_summary_t *block = &snapshot->blocks[snapshot->block_count];
        if (uxQueueMessagesWaiting(s_controls) > 0) {
            /* A full block over TLS can take seconds; let a pending scan or
             * settings change run instead of finishing a 20-block backfill. */
            ESP_LOGI(TAG, "Backfill paused for pending control");
            break;
        }
        if (!fetch_block(start - i, block)) {
            ESP_LOGW(TAG, "Backfill stopped at height %llu: %s",
                     (unsigned long long)(start - i), rpc_last_error());
            break;
        }
        snapshot->block_count++;
    }
}

typedef enum {
    EXTEND_OK,
    EXTEND_NOT_APPLICABLE,
    EXTEND_FAILED,
    EXTEND_DIVERGED,
} extend_result_t;

/* Fetch only the blocks newer than the last poll and splice them onto the
 * previous window, instead of re-downloading all RPC_BLOCK_BATCH full blocks
 * (each a separate TLS session) whenever the tip moves. */
static extend_result_t extend_from_last(dashboard_snapshot_t *snapshot)
{
    if (s_last.block_count == 0 || !s_last.blocks[0].valid ||
        snapshot->tip_height <= s_last.tip_height ||
        snapshot->tip_height - s_last.tip_height >= RPC_BLOCK_BATCH) {
        return EXTEND_NOT_APPLICABLE;
    }
    uint32_t gap = (uint32_t)(snapshot->tip_height - s_last.tip_height);
    for (uint32_t i = 0; i < gap; ++i) {
        if (!fetch_block(snapshot->tip_height - i, &snapshot->blocks[i])) {
            ESP_LOGW(TAG, "Incremental fetch stopped at height %llu: %s",
                     (unsigned long long)(snapshot->tip_height - i), rpc_last_error());
            return EXTEND_FAILED;
        }
    }
    if (strcmp(snapshot->blocks[gap - 1].parent_hash, s_last.blocks[0].hash) != 0) {
        return EXTEND_DIVERGED;
    }
    uint32_t keep = RPC_BLOCK_BATCH - gap;
    if (keep > s_last.block_count) {
        keep = s_last.block_count;
    }
    memcpy(&snapshot->blocks[gap], s_last.blocks, keep * sizeof(snapshot->blocks[0]));
    snapshot->block_count = gap + keep;
    return EXTEND_OK;
}

static void fetch_epoch(dashboard_snapshot_t *snapshot)
{
    cJSON *root = rpc_call("get_current_epoch", "[]");
    if (!root) {
        return;
    }
    const cJSON *epoch = rpc_result(root);
    snapshot->has_epoch = rpc_json_u64(epoch, "number", &snapshot->epoch_number) &&
                          rpc_json_u64(epoch, "start_number", &snapshot->epoch_start) &&
                          rpc_json_u64(epoch, "length", &snapshot->epoch_length) &&
                          snapshot->epoch_length > 0;
    cJSON_Delete(root);
}

static void fetch_pool(dashboard_snapshot_t *snapshot)
{
    cJSON *root = rpc_call("tx_pool_info", "[]");
    if (root) {
        const cJSON *pool = rpc_result(root);
        snapshot->has_pool = rpc_json_u64(pool, "pending", &snapshot->pool_pending) &&
                             rpc_json_u64(pool, "total_tx_size", &snapshot->pool_bytes);
        cJSON_Delete(root);
    }
    root = rpc_call("get_fee_rate_statistics", "[]");
    if (root) {
        const cJSON *fee = rpc_result(root);
        uint64_t median;
        if (rpc_json_u64(fee, "median", &median)) {
            snapshot->fee_rate = (double)median;
            snapshot->has_fee = true;
        }
        cJSON_Delete(root);
    }
}

static void fetch_node(dashboard_snapshot_t *snapshot)
{
    cJSON *root = rpc_call("local_node_info", "[]");
    if (!root) {
        return;
    }
    const cJSON *node = rpc_result(root);
    snapshot->has_peers = rpc_json_u64(node, "connections", &snapshot->peers);
    rpc_json_string(node, "version", snapshot->node_version, sizeof(snapshot->node_version));
    cJSON_Delete(root);
}

static void poll_rpc(void)
{
    memset(&s_work, 0, sizeof(s_work));
    dashboard_snapshot_t *const snap = &s_work;
    snap->online = true;
    snap->source = app_config()->public_rpc ? SOURCE_PUBLIC_RPC : SOURCE_FULL_NODE_RPC;
    snap->capture_ms = monotonic_ms();
    if (!verify_chain(snap)) {
        snap->rpc_ok = false;
        snap->stale = s_last.has_tip;
        if (!snap->chain_mismatch) {
            snprintf(snap->status, sizeof(snap->status), "RPC unavailable: %.55s", rpc_last_error());
        }
        s_last.rpc_ok = false;
        s_last.stale = true;
        snprintf(s_last.status, sizeof(s_last.status), "%s", snap->status);
        s_last.capture_ms = monotonic_ms();
        post_snapshot(&s_last);
        return;
    }
    cJSON *root = rpc_call("get_tip_header", "[]");
    if (!root || !parse_tip(root, snap)) {
        if (root) {
            cJSON_Delete(root);
        }
        s_last.rpc_ok = false;
        s_last.stale = s_last.has_tip;
        s_last.capture_ms = monotonic_ms();
        snprintf(s_last.status, sizeof(s_last.status), "Tip RPC failed: %.60s", rpc_last_error());
        post_snapshot(&s_last);
        return;
    }
    cJSON_Delete(root);

    bool changed = strcmp(s_last_tip_hash, snap->blocks[0].hash) != 0;
    bool reorg = s_last_tip_hash[0] && snap->tip_height <= s_last.tip_height && changed;
    if (!changed && s_last.block_count > 0) {
        memcpy(snap->blocks, s_last.blocks, sizeof(snap->blocks));
        snap->block_count = s_last.block_count;
    } else {
        extend_result_t extended = reorg ? EXTEND_NOT_APPLICABLE : extend_from_last(snap);
        if (extended == EXTEND_DIVERGED) {
            reorg = true;
        }
        if (extended != EXTEND_OK) {
            backfill(snap);
        }
    }
    snap->reorg = reorg;
    if (reorg) {
        ESP_LOGW(TAG, "Canonical hash history changed; invalidating block-derived history");
    }
    static uint64_t epoch_polled_ms;
    static uint64_t pool_polled_ms;
    static uint64_t node_polled_ms;
    uint64_t now = monotonic_ms();
    if (changed || epoch_polled_ms == 0 || now - epoch_polled_ms >= 60000) {
        fetch_epoch(snap);
        epoch_polled_ms = now;
    } else {
        snap->has_epoch = s_last.has_epoch;
        snap->epoch_number = s_last.epoch_number;
        snap->epoch_start = s_last.epoch_start;
        snap->epoch_length = s_last.epoch_length;
    }
    if (pool_polled_ms == 0 || now - pool_polled_ms >= 30000) {
        fetch_pool(snap);
        pool_polled_ms = now;
    } else {
        snap->has_pool = s_last.has_pool;
        snap->has_fee = s_last.has_fee;
        snap->pool_pending = s_last.pool_pending;
        snap->pool_bytes = s_last.pool_bytes;
        snap->fee_rate = s_last.fee_rate;
    }
    if (node_polled_ms == 0 || now - node_polled_ms >= 30000) {
        fetch_node(snap);
        node_polled_ms = now;
    } else {
        snap->has_peers = s_last.has_peers;
        snap->peers = s_last.peers;
        memcpy(snap->node_version, s_last.node_version, sizeof(snap->node_version));
    }
    snap->rpc_ok = snap->has_tip;
    snap->stale = false;
    snap->capture_ms = monotonic_ms();
    cross_check(snap);
    snprintf(snap->status, sizeof(snap->status), "%s RPC; endpoint data, not light-client verified",
             app_config()->public_rpc ? "Public" : "Full-node");
    if (s_lc_check == LC_CHECK_MATCH) {
        snprintf(snap->status, sizeof(snap->status), "%s RPC; block %llu matches the light-client proof",
                 app_config()->public_rpc ? "Public" : "Full-node", (unsigned long long)s_lc_checked_height);
    }
    s_last = *snap;
    snprintf(s_last_tip_hash, sizeof(s_last_tip_hash), "%s", snap->blocks[0].hash);
    post_snapshot(snap);
}

static void poll_demo(void)
{
    memset(&s_work, 0, sizeof(s_work));
    dashboard_snapshot_t *const snap = &s_work;
    snap->demo = true;
    snap->rpc_ok = true;
    snap->has_tip = true;
    snap->has_epoch = true;
    snap->has_pool = true;
    snap->has_fee = true;
    snap->has_peers = true;
    snap->has_difficulty = true;
    snap->source = SOURCE_DEMO;
    snap->tip_height = 17000119;
    snap->tip_timestamp_ms = DEMO_BASE_MS + 119 * 10000;
    snap->capture_ms = monotonic_ms();
    snap->epoch_number = 12000;
    snap->epoch_start = 16999000;
    snap->epoch_length = 1800;
    snap->pool_pending = 37;
    snap->pool_bytes = 18000;
    snap->peers = 8;
    snap->fee_rate = 1100;
    snap->difficulty = 1.25e15;
    snprintf(snap->chain, sizeof(snap->chain), "mainnet");
    snprintf(snap->status, sizeof(snap->status), "Deterministic fixtures; not live chain data");
    snprintf(snap->node_version, sizeof(snap->node_version), "demo");
    snap->block_count = RPC_BLOCK_BATCH;
    for (uint32_t i = 0; i < RPC_BLOCK_BATCH; ++i) {
        rpc_block_summary_t *b = &snap->blocks[i];
        b->height = snap->tip_height - i;
        b->timestamp_ms = snap->tip_timestamp_ms - i * 10000;
        b->tx_count = 3 + (119 - i) % 7;
        b->compact_target = 0;
        b->valid = true;
        snprintf(b->hash, sizeof(b->hash), "0xde%062x", (unsigned)b->height);
        snprintf(b->parent_hash, sizeof(b->parent_hash), "0xde%062x", (unsigned)(b->height - 1));
    }
    s_last = *snap;
    post_snapshot(snap);
}

static void finish_scan(bool ok)
{
    s_scanning = false;
    s_scan_ok = ok;
    s_scan_seq++;
    /* Resume connecting promptly; the scan may have aborted an attempt. */
    s_next_connect_ms = 0;
    post_status(ok ? "Wi-Fi scan complete; select a network" : "Wi-Fi scan failed; tap Scan to retry",
                (xEventGroupGetBits(s_wifi_events) & WIFI_CONNECTED) != 0);
}

static void scan_wifi(void)
{
    ESP_LOGI(TAG, "Wi-Fi scan requested");
    if (s_scanning) {
        return; /* The in-flight scan will answer this request too. */
    }
    esp_err_t err = ensure_wifi();
    if (err == ESP_OK) {
        /* The driver refuses to scan while a connection attempt is in progress
         * (ESP_ERR_WIFI_STATE), which made re-scanning impossible after a bad
         * password. Abort the attempt; wifi_service() resumes it afterwards. */
        if (!(xEventGroupGetBits(s_wifi_events) & WIFI_CONNECTED) && s_connecting) {
            stop_connecting();
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        wifi_scan_config_t scan = {
            .show_hidden = false,
            .scan_type = WIFI_SCAN_TYPE_ACTIVE,
            .scan_time.active = {.min = 80, .max = 240},
        };
        err = esp_wifi_scan_start(&scan, false);
        if (err == ESP_ERR_WIFI_STATE) {
            stop_connecting();
            vTaskDelay(pdMS_TO_TICKS(300));
            err = esp_wifi_scan_start(&scan, false);
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi scan start: %s", esp_err_to_name(err));
        s_scan_count = 0;
        finish_scan(false);
        return;
    }
    xEventGroupClearBits(s_wifi_events, WIFI_SCAN_DONE);
    s_scanning = true;
    s_scan_started_ms = monotonic_ms();
    post_status("Scanning for Wi-Fi networks...", (xEventGroupGetBits(s_wifi_events) & WIFI_CONNECTED) != 0);
}

static bool ssid_seen(uint8_t count, const char *ssid)
{
    for (uint8_t i = 0; i < count; ++i) {
        if (strcmp(s_scan_ssids[i], ssid) == 0) {
            return true;
        }
    }
    return false;
}

static void scan_wifi_complete(void)
{
    uint16_t count = SCAN_RECORD_CAP;
    /* get_ap_records also frees the driver's scan list, even when it holds
     * more entries than requested. */
    esp_err_t err = esp_wifi_scan_get_ap_records(&count, s_records);
    if (err != ESP_OK) {
        esp_wifi_clear_ap_list();
        count = 0;
    }
    if (count > SCAN_RECORD_CAP) {
        count = SCAN_RECORD_CAP;
    }
    for (uint16_t i = 0; i < count; ++i) {
        for (uint16_t j = i + 1; j < count; ++j) {
            if (s_records[j].rssi > s_records[i].rssi) {
                wifi_ap_record_t swap = s_records[i];
                s_records[i] = s_records[j];
                s_records[j] = swap;
            }
        }
    }
    /* Strongest first, one entry per SSID, hidden networks skipped. */
    uint8_t kept = 0;
    for (uint16_t i = 0; i < count && kept < SCAN_MAX_RESULTS; ++i) {
        char ssid[33];
        snprintf(ssid, sizeof(ssid), "%.32s", (const char *)s_records[i].ssid);
        if (ssid[0] == '\0' || ssid_seen(kept, ssid)) {
            continue;
        }
        memcpy(s_scan_ssids[kept], ssid, sizeof(ssid));
        s_scan_rssi[kept] = s_records[i].rssi;
        kept++;
    }
    s_scan_count = kept;
    ESP_LOGI(TAG, "Wi-Fi scan complete: %u APs, %u listed, status %s",
             (unsigned)count, (unsigned)kept, esp_err_to_name(err));
    finish_scan(err == ESP_OK);
}

static void service_scan(void)
{
    if (!s_wifi_started) {
        return;
    }
    if (xEventGroupGetBits(s_wifi_events) & WIFI_SCAN_DONE) {
        xEventGroupClearBits(s_wifi_events, WIFI_SCAN_DONE);
        if (s_scanning) {
            scan_wifi_complete();
        } else {
            esp_wifi_clear_ap_list(); /* Late event from a stopped scan. */
        }
    } else if (s_scanning && monotonic_ms() - s_scan_started_ms > SCAN_TIMEOUT_MS) {
        ESP_LOGW(TAG, "Wi-Fi scan timed out");
        esp_wifi_scan_stop();
        esp_wifi_clear_ap_list();
        s_scan_count = 0;
        finish_scan(false);
    }
}

static void handle_control(const control_event_t *control)
{
    ESP_LOGI(TAG, "Control %d received", (int)control->type);
    switch (control->type) {
    case CONTROL_RECONNECT:
        stop_connecting();
        reset_backoff();
        break;
    case CONTROL_CONFIG_CHANGED:
        if (s_wifi_started) {
            if (s_scanning) {
                esp_wifi_scan_stop();
                s_scanning = false;
            }
            stop_connecting();
            esp_err_t err = apply_sta_config();
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Wi-Fi config: %s", esp_err_to_name(err));
            }
        }
        reset_backoff();
        s_wifi_problem[0] = '\0';
        memset(&s_last, 0, sizeof(s_last));
        s_last_tip_hash[0] = '\0';
        break;
    case CONTROL_SCAN_WIFI:
        scan_wifi();
        break;
    case CONTROL_DEMO_CHANGED:
    default:
        break;
    }
}

static void process_controls(void)
{
    control_event_t control;
    while (xQueueReceive(s_controls, &control, 0) == pdTRUE) {
        handle_control(&control);
    }
}

/* Sleep up to `ms`, but return early when the UI sends a control or Wi-Fi
 * raises an event. A plain vTaskDelay() here left scan requests unanswered
 * for up to 30 s during reconnect backoff. */
static void wait_or_wake(uint32_t ms)
{
    const uint32_t slice_ms = 50;
    for (uint32_t waited = 0; waited < ms; waited += slice_ms) {
        if (uxQueueMessagesWaiting(s_controls) > 0) {
            return;
        }
        if (s_wifi_started) {
            EventBits_t bits = xEventGroupWaitBits(s_wifi_events, WIFI_SCAN_DONE | WIFI_DISCONNECTED,
                                                   pdFALSE, pdFALSE, pdMS_TO_TICKS(slice_ms));
            if (bits & (WIFI_SCAN_DONE | WIFI_DISCONNECTED)) {
                return;
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(slice_ms));
        }
    }
}

static void post_wifi_progress(void)
{
    char status[96];
    char ssid[APP_SSID_LEN + 1];
    app_config_t cfg;
    app_config_copy(&cfg);
    memset(cfg.password, 0, sizeof(cfg.password));
    snprintf(ssid, sizeof(ssid), "%s", cfg.ssid);
    if (s_scanning) {
        snprintf(status, sizeof(status), "Scanning for Wi-Fi networks...");
    } else if (s_wifi_problem[0]) {
        snprintf(status, sizeof(status), "Wi-Fi %.24s: %.39s; retrying", ssid, s_wifi_problem);
    } else {
        snprintf(status, sizeof(status), "Connecting to Wi-Fi %.32s...", ssid);
    }
    post_status(status, false);
}

static void network_task(void *arg)
{
    (void)arg;
    bool was_demo = !app_config()->demo;
    uint32_t poll_count = 0;
#if CONFIG_CKB_DEBUG_BOOT_SCAN
    scan_wifi();
#endif
    while (true) {
        process_controls();
        service_scan();
        if (app_config()->demo) {
            poll_demo();
            was_demo = true;
            wait_or_wake(1000);
            continue;
        }
        if (was_demo) {
            memset(&s_last, 0, sizeof(s_last));
            s_last_tip_hash[0] = '\0';
            was_demo = false;
        }
        if (ensure_wifi() != ESP_OK) {
            post_status("Wi-Fi unavailable; open Settings > Wi-Fi / node", false);
            wait_or_wake(5000);
            continue;
        }
        if (app_config()->ssid[0] == '\0') {
            if (!s_scanning) {
                post_status("Settings > Wi-Fi / node: choose a scanned network", false);
            }
            wait_or_wake(2000);
            continue;
        }
        if (!wifi_service()) {
            post_wifi_progress();
            wait_or_wake(1000);
            continue;
        }
        poll_rpc();
        if ((++poll_count % 8) == 0) {
            ESP_LOGI(TAG, "rpc_ok=%d tip=%llu blocks=%u status=\"%s\"", (int)s_last.rpc_ok,
                     (unsigned long long)s_last.tip_height, (unsigned)s_last.block_count, s_last.status);
            ESP_LOGI(TAG, "heap internal=%u min=%u psram=%u rpc_stack_hwm=%u",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
        }
        wait_or_wake(8000);
    }
}

esp_err_t network_start(QueueHandle_t snapshots, QueueHandle_t controls)
{
    if (!snapshots || !controls) {
        return ESP_ERR_INVALID_ARG;
    }
    s_snapshots = snapshots;
    s_controls = controls;
    s_wifi_events = xEventGroupCreate();
    if (!s_wifi_events) {
        return ESP_ERR_NO_MEM;
    }
    if (rpc_client_init() != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }
    return xTaskCreatePinnedToCore(network_task, "ckb_rpc", 12288, NULL, 4, NULL, 0) == pdTRUE
               ? ESP_OK : ESP_ERR_NO_MEM;
}
