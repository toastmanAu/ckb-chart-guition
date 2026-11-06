#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define METRIC_HISTORY_CAP 480
#define METRIC_VIEW_CAP 240

typedef enum {
    METRIC_BLOCK_INTERVAL = 0,
    METRIC_TX_PER_BLOCK,
    METRIC_POOL_PENDING,
    METRIC_POOL_BYTES,
    METRIC_FEE_RATE,
    METRIC_PEERS,
    METRIC_DIFFICULTY,
    METRIC_EPOCH_PROGRESS,
    METRIC_OCCUPIED_CKB,    /* header dao u, in CKB */
    METRIC_COUNT
} metric_id_t;

typedef enum {
    SOURCE_LIGHT_VERIFIED = 0,
    SOURCE_EXTERNAL_LIGHT_CLIENT,
    SOURCE_FULL_NODE_RPC,
    SOURCE_PUBLIC_RPC,
    SOURCE_DERIVED,
    SOURCE_DEMO
} metric_source_t;

typedef enum {
    QUALITY_LIVE = 0,
    QUALITY_STALE,
    QUALITY_PARTIAL,
    QUALITY_ESTIMATE,
    QUALITY_UNAVAILABLE
} metric_quality_t;

typedef struct {
    double value;
    uint64_t timestamp_ms;
    uint32_t block_height;
    uint8_t source;
    uint8_t quality;
    uint8_t valid;
} metric_point_t;

typedef struct {
    metric_point_t points[METRIC_HISTORY_CAP];
    uint16_t next;
    uint16_t count;
} metric_ring_t;

typedef struct {
    metric_ring_t series[METRIC_COUNT];
} metric_store_t;

bool ckb_parse_hex_u64(const char *text, uint64_t *out);

/* A header's `dao` field (RFC 0023): four little-endian u64, in shannons
 * except ar. c = total issued capacity, ar = NervosDAO accumulated rate
 * (10^16 at genesis), s = secondary-issuance bookkeeping, u = total
 * occupied capacity. */
typedef struct {
    uint64_t c;
    uint64_t ar;
    uint64_t s;
    uint64_t u;
} ckb_dao_t;

/* Decode "0x" + exactly 64 hex digits. On failure *out is left untouched. */
bool ckb_parse_dao(const char *text, ckb_dao_t *out);
/* Decimal with ',' every three digits counted from the right (20,919,340).
 * Falls back to plain digits when `size` cannot hold the separators. */
void ckb_format_grouped_u64(char *out, size_t size, uint64_t value);
bool ckb_parse_hex_double(const char *text, double *out);
double ckb_epoch_progress(uint64_t tip, uint64_t start, uint64_t length);
double ckb_compact_target_difficulty(uint32_t compact_target);
size_t ckb_downsample_mean(const double *input, size_t input_count,
                           double *output, size_t output_cap);

void metric_store_init(metric_store_t *store);
void metric_store_clear(metric_store_t *store);
void metric_store_drop_from_height(metric_store_t *store, uint32_t height);
bool metric_ring_add(metric_ring_t *ring, const metric_point_t *point);
size_t metric_ring_read(const metric_ring_t *ring, uint64_t oldest_ms,
                        metric_point_t *out, size_t out_cap);
double metric_ring_coverage(const metric_ring_t *ring, uint64_t now_ms,
                            uint64_t window_ms);
