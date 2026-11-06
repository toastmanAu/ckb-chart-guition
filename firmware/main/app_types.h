#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "metric_core.h"

#define RPC_BLOCK_BATCH 20
#define HASH_TEXT_LEN 67
#define SCAN_MAX_RESULTS 8

typedef struct {
    uint64_t height;
    uint64_t timestamp_ms;
    uint32_t tx_count;
    uint32_t compact_target;
    char hash[HASH_TEXT_LEN];
    char parent_hash[HASH_TEXT_LEN];
    bool valid;
} rpc_block_summary_t;

typedef struct {
    bool online;
    bool rpc_ok;
    bool stale;
    bool demo;
    bool chain_mismatch;
    bool reorg;
    /* Wi-Fi scan state is stamped onto every snapshot by the network task.
     * scan_seq increments once per finished scan (success or failure), so the
     * UI can tell a fresh result from a stale one without a sticky flag. */
    uint32_t scan_seq;
    bool scanning;
    bool scan_ok;
    uint8_t scan_count;
    char scan_ssids[SCAN_MAX_RESULTS][33];
    int8_t scan_rssi[SCAN_MAX_RESULTS];
    bool has_tip;
    bool has_epoch;
    bool has_pool;
    bool has_fee;
    bool has_peers;
    bool has_difficulty;
    uint64_t tip_height;
    uint64_t tip_timestamp_ms;
    uint64_t capture_ms;
    uint64_t epoch_number;
    uint64_t epoch_start;
    uint64_t epoch_length;
    uint64_t pool_pending;
    uint64_t pool_bytes;
    uint64_t peers;
    double fee_rate;
    double difficulty;
    metric_source_t source;
    char chain[16];
    char status[96];
    char node_version[32];
    rpc_block_summary_t blocks[RPC_BLOCK_BATCH];
    uint32_t block_count;
    /* On-device light client, stamped by the network task on every post
     * (lc_task.h / lc_badge.h). lc_badge is an lc_badge_t. */
    uint8_t lc_badge;
    uint8_t lc_state;        /* lc_state_t */
    uint8_t lc_check;        /* lc_check_t: RPC vs proven hash at lc_height */
    bool lc_has_tip;
    uint64_t lc_height;      /* proven tip height */
    uint32_t lc_age_s;       /* seconds since the proof covered the peer's tip */
    uint64_t lc_checkpoint;  /* trusted start of the current session */
    uint32_t lc_sessions;
    char lc_hash[HASH_TEXT_LEN];
} dashboard_snapshot_t;

typedef enum {
    CONTROL_RECONNECT = 0,
    CONTROL_DEMO_CHANGED,
    CONTROL_CONFIG_CHANGED,
    CONTROL_SCAN_WIFI,
} control_type_t;

typedef struct {
    control_type_t type;
} control_event_t;
