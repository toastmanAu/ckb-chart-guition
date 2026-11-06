/*
 * lc_task.c - the on-device light client (see lc_task.h).
 *
 * A thin user of ckb-light-esp's ckb_session, modelled on that repository's
 * main/ckb_client_task.c and test_host/connect_live.c (--sched): the session
 * owns TCP framing, SecIO, yamux, the light-client protocol and every proof
 * check. This file owns what only the firmware has: the lwIP socket, PSRAM
 * buffers, the persisted checkpoint (NVS) and the reconnect policy.
 *
 * The dashboard needs the PROVEN TIP, not wallet data, so the session
 * watches a lock hash that cannot occur (all 0xff): filters sync (cheaply)
 * and never match except by a GCS false positive (~1 in 784,931 per block),
 * whose block is fetched, verified and discarded.
 */
#include "lc_task.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "app_config.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "sdkconfig.h"

#if CONFIG_CKB_LC_ENABLE
#include "ckb_identify.h"
#include "ckb_peerid.h"
#include "ckb_session.h"
#include "crypto_mbedtls.h"
#endif

static const char *TAG = "lc";

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static lc_status_t s_status;

void lc_task_status(lc_status_t *out)
{
    taskENTER_CRITICAL(&s_lock);
    *out = s_status;
    taskEXIT_CRITICAL(&s_lock);
}

static void set_state(lc_state_t state)
{
    taskENTER_CRITICAL(&s_lock);
    s_status.state = state;
    taskEXIT_CRITICAL(&s_lock);
}

#if !CONFIG_CKB_LC_ENABLE

esp_err_t lc_task_start(bool (*wifi_up)(void))
{
    (void)wifi_up;
    set_state(LC_STATE_DISABLED);
    return ESP_OK;
}

#else

static uint64_t now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

/*
 * Compiled-in checkpoints: number, hash, own total difficulty (little-endian
 * U256). The same values as ckb-light-esp test_host/connect_live.c, which
 * records how each was obtained and checked:
 *   mainnet 20,912,678: hash = parent of the 20,912,679 tip a mainnet node
 *     sent, equal to mainnet.ckb.dev get_header_by_number; TD = that tip's
 *     chain root, reproduced from the RPC's epochs.
 *   testnet 22,952,000: hash from testnet.ckb.dev; TD by the same epoch sum.
 * A checkpoint is the trust anchor: refresh at each release.
 */
static const ckb_session_checkpoint_t CP_MAINNET = { 20912678ULL,
    { 0xba,0x0f,0xae,0x0c,0xee,0x9f,0xaa,0xba,0x46,0xff,0xd8,0xf2,0x75,0xcd,0x48,0xea,
      0xad,0xab,0xb6,0x3b,0x7f,0x04,0x7c,0xdf,0xcb,0xf0,0x4d,0xa4,0xd1,0x53,0xb1,0xf2 },
    { 0xee,0x48,0xd0,0x18,0x00,0x91,0x2a,0x96,0x72,0xe3,0x1a } };
static const ckb_session_checkpoint_t CP_TESTNET = { 22952000ULL,
    { 0x4d,0x74,0xf1,0xe6,0xfe,0x72,0xbd,0x92,0x97,0xce,0xdf,0x92,0x3a,0x7b,0xde,0x18,
      0x08,0x4a,0x60,0xaa,0xa8,0xcc,0x8f,0xab,0x32,0x3f,0xa1,0x81,0x2f,0x98,0x86,0x42 },
    { 0x12,0x29,0xf9,0x89,0x00,0xa3,0x27 } };

/* A lock hash nobody can pay to: only the proven tip is wanted. */
static const ckb_hash_t NO_LOCK = {
    0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
    0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff };

/* ── buffers: PSRAM, allocated once ── */
typedef struct {
    uint8_t *rx;
    uint8_t *block;
    uint8_t *zbuf;
    ckb_hash_t *scratch;
    uint8_t *lsp;
    filter_ctx_t *fctx;
    fstream_ctx_t *fstream;
    hstream_ctx_t *hstream;
} lc_bufs_t;

static lc_bufs_t s_mem;
static uint8_t s_tx[CKB_SESSION_TX_MIN];
static uint64_t s_sess_mem[CKB_SESSION_MAX_SIZE / sizeof(uint64_t)];
static ckb_block_effect_t s_effect;
static secio_crypto_t s_crypto;

#define LC_RX_LEN CKB_SESSION_RX_MIN
#define LC_BLOCK_LEN CKB_SESSION_BLOCK_RECOMMENDED
#define LC_ZBUF_LEN (CKB_SESSION_BLOCK_RECOMMENDED + 64u)
#define LC_SCRATCH_N CKB_SESSION_SCRATCH_RECOMMENDED
#define LC_LSP_LEN (CKB_SESSION_LSP_MIN + 4096u)

static void *psram(size_t n)
{
    return heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static bool alloc_buffers(void)
{
    s_mem.rx = psram(LC_RX_LEN);
    s_mem.block = psram(LC_BLOCK_LEN);
    s_mem.zbuf = psram(LC_ZBUF_LEN);
    s_mem.scratch = psram((size_t)LC_SCRATCH_N * sizeof(ckb_hash_t));
    s_mem.lsp = psram(LC_LSP_LEN);
    s_mem.fctx = psram(sizeof(filter_ctx_t));
    s_mem.fstream = psram(sizeof(fstream_ctx_t));
    s_mem.hstream = psram(sizeof(hstream_ctx_t));
    size_t total = LC_RX_LEN + LC_BLOCK_LEN + LC_ZBUF_LEN + (size_t)LC_SCRATCH_N * sizeof(ckb_hash_t) +
                   LC_LSP_LEN + sizeof(filter_ctx_t) + sizeof(fstream_ctx_t) + sizeof(hstream_ctx_t);
    bool ok = s_mem.rx && s_mem.block && s_mem.zbuf && s_mem.scratch && s_mem.lsp && s_mem.fctx &&
              s_mem.fstream && s_mem.hstream;
    ESP_LOGI(TAG, "buffers %s: %u KiB PSRAM (rx %u, block %u, zbuf %u, scratch %u, lsp %u, filter %u); "
                  "PSRAM free after %u KiB",
             ok ? "ready" : "FAILED", (unsigned)(total / 1024), (unsigned)(LC_RX_LEN / 1024),
             (unsigned)(LC_BLOCK_LEN / 1024), (unsigned)(LC_ZBUF_LEN / 1024),
             (unsigned)(LC_SCRATCH_N * sizeof(ckb_hash_t) / 1024), (unsigned)(LC_LSP_LEN / 1024),
             (unsigned)(sizeof(filter_ctx_t) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    return ok;
}

/* ── the persisted checkpoint (NVS) ── */
#define LC_NVS_NS "ckb_lc"
#define LC_NVS_MAGIC 0x31504B43u   /* "CKP1" */
/* Rewrite at most this often: a checkpoint moves every block. */
#define LC_NVS_MIN_INTERVAL_MS (30u * 60u * 1000u)

typedef struct {
    uint32_t magic;
    uint32_t size;
    ckb_session_checkpoint_t cp;
} lc_nvs_record_t;

static bool is_mainnet(void)
{
    return strcmp(app_config()->chain, "mainnet") == 0;
}

static const char *nvs_key(void)
{
    return is_mainnet() ? "cp_mainnet" : "cp_testnet";
}

static const ckb_session_checkpoint_t *compiled_cp(void)
{
    return is_mainnet() ? &CP_MAINNET : &CP_TESTNET;
}

/* A stored checkpoint is used only if it is intact and not older than the
 * compiled one (an older one would only add work; a newer firmware with a
 * newer compiled checkpoint supersedes it). */
static bool nvs_load(ckb_session_checkpoint_t *out)
{
    nvs_handle_t h;
    if (nvs_open(LC_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    lc_nvs_record_t rec;
    size_t len = sizeof(rec);
    esp_err_t err = nvs_get_blob(h, nvs_key(), &rec, &len);
    nvs_close(h);
    if (err != ESP_OK || len != sizeof(rec) || rec.magic != LC_NVS_MAGIC || rec.size != sizeof(rec.cp) ||
        rec.cp.number <= compiled_cp()->number) {
        return false;
    }
    *out = rec.cp;
    return true;
}

static void nvs_save(const ckb_session_checkpoint_t *cp)
{
    nvs_handle_t h;
    if (nvs_open(LC_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "checkpoint not saved: NVS unavailable");
        return;
    }
    lc_nvs_record_t rec = { LC_NVS_MAGIC, sizeof(rec.cp), *cp };
    esp_err_t err = nvs_set_blob(h, nvs_key(), &rec, sizeof(rec));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "checkpoint %llu saved", (unsigned long long)cp->number);
    } else {
        ESP_LOGW(TAG, "checkpoint not saved: %s", esp_err_to_name(err));
    }
}

/* ── platform seam ── */
/* The session narrates every record and dumps the peer's identify message
 * at INFO: useful on a bench, a flood on a dashboard's serial log. */
static bool is_chatter(const char *msg)
{
    return strncmp(msg, "[rx]", 4) == 0 || strncmp(msg, "[tx]", 4) == 0 || strncmp(msg, "  ", 2) == 0 ||
           strncmp(msg, "[identify]", 10) == 0;
}

static void env_log(void *user, int level, const char *msg)
{
    (void)user;
    if (level == CKB_LOG_INFO && is_chatter(msg)) {
        level = CKB_LOG_DEBUG;
    }
    switch (level) {
    case CKB_LOG_ERROR: ESP_LOGE(TAG, "%s", msg); break;
    case CKB_LOG_WARN: ESP_LOGW(TAG, "%s", msg); break;
    case CKB_LOG_DEBUG: ESP_LOGD(TAG, "%s", msg); break;
    default: ESP_LOGI(TAG, "%s", msg); break;
    }
}

static uint64_t env_now_ms(void *user)
{
    (void)user;
    return now_ms();
}

static int env_tcp_connect(void *user, const char *host, uint16_t port, uint32_t timeout_ms)
{
    (void)user;
    (void)timeout_ms;
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    char service[8];
    snprintf(service, sizeof(service), "%u", (unsigned)port);
    if (getaddrinfo(host, service, &hints, &res) != 0 || !res) {
        ESP_LOGW(TAG, "cannot resolve %s", host);
        return -1;
    }
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        freeaddrinfo(res);
        return -1;
    }
    int rc = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (rc != 0) {
        ESP_LOGW(TAG, "connect %s:%u failed: errno %d", host, (unsigned)port, errno);
        close(fd);
        return -1;
    }
    /* Close hard: this client reconnects every few minutes, and TIME_WAIT
     * sockets hold lwIP buffers (ckb_client_task.c, measured 2026-08-08). */
    struct linger lg = { .l_onoff = 1, .l_linger = 0 };
    setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
    /* A record that has started arriving; the node can take ~17 s to build
     * a large reply. */
    struct timeval tv = { .tv_sec = 45, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return fd;
}

static void env_tcp_close(void *user, int fd)
{
    (void)user;
    close(fd);
}

static int hex_to_bytes(const char *hex, uint8_t *out, size_t n)
{
    if (strlen(hex) != 2 * n) {
        return -1;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        if (sscanf(hex + 2 * i, "%2x", &v) != 1) {
            return -1;
        }
        out[i] = (uint8_t)v;
    }
    return 0;
}

/* ── one session ── */

/* Carried between sessions this boot: the checkpoint to start the next
 * session from (16 below the last proven tip) and the filter cursor. */
static ckb_session_checkpoint_t s_carry;
static bool s_carry_valid;
static uint64_t s_cursor;
static ckb_hash_t s_cursor_hash;
static bool s_cursor_valid;
static uint64_t s_saved_number;
static uint64_t s_saved_ms;

#define LC_STEP_BUDGET_MS 3000u

/* Publish the tip the last last-state proof bound. Before the first proof
 * the session reports the checkpoint itself: that is the trust anchor, not
 * a proof of anything current, so it is not published. */
static void publish_tip(ckb_session_t *s, const ckb_session_checkpoint_t *cp)
{
    uint64_t vn = 0;
    uint64_t peer_tip = 0;
    ckb_hash_t vh;
    if (ckb_session_verified_tip(s, &vn, vh, NULL) != 0 || vn == 0 ||
        (vn == cp->number && memcmp(vh, cp->hash, 32) == 0)) {
        return;
    }
    ckb_session_cursor(s, NULL, NULL, &peer_tip);
    taskENTER_CRITICAL(&s_lock);
    bool moved = !s_status.has_tip || vn != s_status.tip_number || memcmp(vh, s_status.tip_hash, 32) != 0;
    s_status.has_tip = true;
    s_status.tip_number = vn;
    memcpy(s_status.tip_hash, vh, 32);
    s_status.state = LC_STATE_PROVEN;
    /* The proof vouches for "now" while it covers the tip the peer is
     * reporting; once the peer moves on, it ages until the next proof. */
    if (moved || peer_tip == vn) {
        s_status.tip_proven_ms = now_ms();
    }
    taskEXIT_CRITICAL(&s_lock);
    if (moved) {
        ESP_LOGI(TAG, "proven tip %llu 0x%02x%02x%02x%02x...", (unsigned long long)vn, vh[0], vh[1], vh[2],
                 vh[3]);
    }
}

static void maybe_persist(ckb_session_t *s, bool session_end)
{
    ckb_session_checkpoint_t cp;
    if (ckb_session_checkpoint_to_persist(s, &cp) != 0) {
        return;
    }
    s_carry = cp;
    s_carry_valid = true;
    uint64_t now = now_ms();
    bool due = s_saved_ms == 0 || now - s_saved_ms >= LC_NVS_MIN_INTERVAL_MS;
    if (cp.number > s_saved_number && (due || (session_end && now - s_saved_ms >= 60000u))) {
        nvs_save(&cp);
        s_saved_number = cp.number;
        s_saved_ms = now;
    }
}

static int run_session(void)
{
    static ckb_session_checkpoint_t stored;
    const ckb_session_checkpoint_t *compiled = compiled_cp();
    const ckb_session_checkpoint_t *cp = compiled;
    if (s_carry_valid) {
        cp = &s_carry;
    } else if (nvs_load(&stored)) {
        cp = &stored;
        s_saved_number = stored.number;
        ESP_LOGI(TAG, "resuming from saved checkpoint %llu", (unsigned long long)stored.number);
    }

    static uint8_t pin[CKB_PEER_ID_LEN];
    bool pinned = hex_to_bytes(CONFIG_CKB_LC_PEER_ID, pin, sizeof(pin)) == 0;
    const ckb_peer_t peer = { CONFIG_CKB_LC_HOST, (uint16_t)CONFIG_CKB_LC_PORT, pinned ? pin : NULL,
                              pinned ? CKB_PEER_ID_LEN : 0, is_mainnet() ? CKB_NET_MAINNET : CKB_NET_TESTNET };
    const ckb_session_env_t env = { env_log, env_now_ms, env_tcp_connect, env_tcp_close, NULL, &s_crypto, NULL };
    const ckb_session_bufs_t bufs = {
        .rx = s_mem.rx, .rx_len = LC_RX_LEN, .tx = s_tx, .tx_len = sizeof(s_tx),
        .fctx_applier = s_mem.fctx, .fctx_sentinel = NULL,
        .fstream = s_mem.fstream, .hstream = s_mem.hstream,
        .block = s_mem.block, .block_len = LC_BLOCK_LEN,
        .scratch = s_mem.scratch, .scratch_n = LC_SCRATCH_N,
        .cells = NULL,
        .zbuf = s_mem.zbuf, .zbuf_len = LC_ZBUF_LEN,
        .lsp = s_mem.lsp, .lsp_len = LC_LSP_LEN,
    };
    ckb_session_t *s = (ckb_session_t *)s_sess_mem;

    set_state(LC_STATE_CONNECTING);
    taskENTER_CRITICAL(&s_lock);
    s_status.sessions++;
    s_status.checkpoint = cp->number;
    taskEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "session to %s:%u (%s), checkpoint %llu%s", CONFIG_CKB_LC_HOST, (unsigned)CONFIG_CKB_LC_PORT,
             pinned ? "peer id pinned" : "peer id NOT pinned", (unsigned long long)cp->number,
             cp == compiled ? " (compiled)" : "");
    int st = ckb_session_open(s, &peer, &env, &bufs);
    if (st != CKB_SESSION_OK) {
        ESP_LOGW(TAG, "session open failed: %d", st);
        return st;
    }
    const ckb_session_watch_opts_t opts = {
        .require_signer_check = 0,
        .checkpoint = cp,
        .fallback_checkpoint = cp == compiled ? NULL : compiled,
    };
    /* Filters from the trusted start, or onward from where the last session
     * got to (its running hash chains the next range onto verified ones). */
    uint64_t from = s_cursor_valid ? s_cursor + 1 : cp->number;
    if (ckb_session_watch_ex(s, NO_LOCK, NULL, from, s_cursor_valid ? s_cursor_hash : NULL, &opts) != 0) {
        ESP_LOGE(TAG, "watch failed");
        ckb_session_close(s);
        return CKB_SESSION_ERROR;
    }
    set_state(LC_STATE_ANCHORING);

    for (;;) {
        uint64_t t0 = now_ms();
        st = ckb_session_step(s, LC_STEP_BUDGET_MS);
        uint64_t took = now_ms() - t0;
        if (took > LC_STEP_BUDGET_MS + 1000u) {
            ESP_LOGI(TAG, "step took %llu ms (CPU-bound work at idle priority)", (unsigned long long)took);
        }
        while (ckb_session_next_effect(s, &s_effect) == 1) {
            /* A GCS false positive for NO_LOCK: verified and discarded. */
        }
        if (st != CKB_SESSION_OK && st != CKB_SESSION_AT_TIP && st != CKB_SESSION_CAUGHT_UP) {
            break;
        }
        publish_tip(s, cp);
        maybe_persist(s, false);
    }
    if (st == CKB_SESSION_CHAIN_BREAK) {
        uint64_t at = 0;
        ckb_session_break_t which = CKB_BREAK_NONE;
        ckb_session_break_at(s, &at, &which);
        ESP_LOGW(TAG, "chain break at %llu (cursor %d): reconnecting", (unsigned long long)at, (int)which);
    }
    publish_tip(s, cp);
    maybe_persist(s, true);
    uint64_t n = 0;
    ckb_session_cursor(s, &n, s_cursor_hash, NULL);
    /* After a break the cursor's hash is not one the peer agrees with:
     * re-sync filters from the checkpoint rather than resume from it. */
    s_cursor_valid = n > 0 && st != CKB_SESSION_CHAIN_BREAK;
    s_cursor = n;
    ckb_session_close(s);
    ESP_LOGW(TAG, "session ended: status %d, filters at %llu", st, (unsigned long long)n);
    return st;
}

/* ── task ── */
#define LC_BACKOFF_MIN_MS 2000u
#define LC_BACKOFF_MAX_MS 60000u

static bool (*s_wifi_up)(void);

static void lc_task(void *arg)
{
    (void)arg;
    /* The protocol component and this file must agree on the header caps,
     * or every struct between them has two sizes (upstream once compiled
     * without sdkconfig.h and silently used 64/64). */
    if (lc_proof_headers_cap != MAX_SAMPLED_BLOCKS || lc_proof_items_cap != MAX_LSP_PROOF_ITEMS) {
        ESP_LOGE(TAG, "light client disabled: protocol built with caps %u/%u, firmware expects %u/%u",
                 (unsigned)lc_proof_headers_cap, (unsigned)lc_proof_items_cap, (unsigned)MAX_SAMPLED_BLOCKS,
                 (unsigned)MAX_LSP_PROOF_ITEMS);
        set_state(LC_STATE_DISABLED);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "proof caps %u headers / %u items (contiguous up to %u blocks)", (unsigned)lc_proof_headers_cap,
             (unsigned)lc_proof_items_cap, (unsigned)CKB_SESSION_LSP_LAST_N);
    if (!alloc_buffers()) {
        ESP_LOGE(TAG, "light client disabled: not enough PSRAM");
        set_state(LC_STATE_DISABLED);
        vTaskDelete(NULL);
        return;
    }
    crypto_mbedtls_init(&s_crypto);
    uint32_t backoff = LC_BACKOFF_MIN_MS;
    for (;;) {
        if (app_config()->demo) {
            set_state(LC_STATE_DISABLED);
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        if (!s_wifi_up()) {
            set_state(LC_STATE_WAITING);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        uint64_t before = s_status.tip_number;
        int st = run_session();
        taskENTER_CRITICAL(&s_lock);
        s_status.last_status = st;
        s_status.state = LC_STATE_RETRYING;
        bool progressed = s_status.tip_number > before;
        taskEXIT_CRITICAL(&s_lock);
        /* The node drops idle sessions every few minutes: normal. Back off
         * only when a session proved nothing new. */
        backoff = progressed ? LC_BACKOFF_MIN_MS
                             : (backoff >= LC_BACKOFF_MAX_MS / 2 ? LC_BACKOFF_MAX_MS : backoff * 2);
        uint32_t delay = backoff - backoff / 4 + esp_random() % (backoff / 2 + 1);
        ESP_LOGI(TAG, "reconnecting in %u ms (heap internal %u, stack hwm %u)", (unsigned)delay,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)uxTaskGetStackHighWaterMark(NULL));
        vTaskDelay(pdMS_TO_TICKS(delay));
    }
}

esp_err_t lc_task_start(bool (*wifi_up)(void))
{
    if (!wifi_up) {
        return ESP_ERR_INVALID_ARG;
    }
    s_wifi_up = wifi_up;
    set_state(LC_STATE_WAITING);
    /* Core 0 (Wi-Fi, the RPC worker), at IDLE priority: verifying a sampled
     * proof is seconds of pure CPU with no yield inside the protocol code.
     * At priority 3 it starved IDLE0 past the task watchdog while the RPC
     * task did TLS (2026-11-06). At priority 0 it time-slices with IDLE0
     * every tick, and every other task preempts it. LVGL keeps core 1.
     * 12 KiB: mbedtls ECC is stack-hungry (the hwm is logged per session). */
    return xTaskCreatePinnedToCore(lc_task, "ckb_lc", 12288, NULL, tskIDLE_PRIORITY, NULL, 0) == pdTRUE
               ? ESP_OK
               : ESP_ERR_NO_MEM;
}

#endif /* CONFIG_CKB_LC_ENABLE */
