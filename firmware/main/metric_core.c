#include "metric_core.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

bool ckb_parse_hex_u64(const char *text, uint64_t *out)
{
    if (!text || !out || text[0] != '0' || text[1] != 'x' || text[2] == '\0') {
        return false;
    }
    errno = 0;
    char *end = NULL;
    unsigned long long value = strtoull(text + 2, &end, 16);
    if (errno == ERANGE || end == text + 2 || *end != '\0') {
        return false;
    }
    *out = (uint64_t)value;
    return true;
}

static int hex_nibble(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

bool ckb_parse_dao(const char *text, ckb_dao_t *out)
{
    if (!text || !out || text[0] != '0' || text[1] != 'x') {
        return false;
    }
    uint64_t words[4] = {0};
    const char *hex = text + 2;
    for (size_t byte = 0; byte < 32; ++byte) {
        int hi = hex_nibble(hex[2 * byte]);
        int lo = hi < 0 ? -1 : hex_nibble(hex[2 * byte + 1]);
        if (lo < 0) {
            return false; /* also catches an early '\0' */
        }
        /* Little-endian: byte k of a word is bits 8k..8k+7. */
        words[byte / 8] |= (uint64_t)((hi << 4) | lo) << (8 * (byte % 8));
    }
    if (hex[64] != '\0') {
        return false;
    }
    out->c = words[0];
    out->ar = words[1];
    out->s = words[2];
    out->u = words[3];
    return true;
}

bool ckb_parse_hex_double(const char *text, double *out)
{
    uint64_t value;
    if (!out || !ckb_parse_hex_u64(text, &value)) {
        return false;
    }
    *out = (double)value;
    return true;
}

double ckb_epoch_progress(uint64_t tip, uint64_t start, uint64_t length)
{
    if (length == 0 || tip < start) {
        return NAN;
    }
    uint64_t completed = tip - start + 1;
    double progress = (double)completed / (double)length;
    if (progress < 0.0) {
        return 0.0;
    }
    return progress > 1.0 ? 1.0 : progress;
}

double ckb_compact_target_difficulty(uint32_t compact)
{
    uint32_t exponent = compact >> 24;
    uint32_t mantissa = compact & 0x007fffffu;
    if ((compact & 0x00800000u) || mantissa == 0 || exponent == 0) {
        return NAN;
    }
    int power = 256 - 8 * ((int)exponent - 3);
    return ldexp(1.0 / (double)mantissa, power);
}

size_t ckb_downsample_mean(const double *input, size_t input_count,
                           double *output, size_t output_cap)
{
    if (!input || !output || output_cap == 0 || input_count == 0) {
        return 0;
    }
    if (input_count <= output_cap) {
        memcpy(output, input, input_count * sizeof(*input));
        return input_count;
    }
    size_t used = input_count < output_cap ? input_count : output_cap;
    for (size_t bucket = 0; bucket < used; ++bucket) {
        size_t begin = bucket * input_count / used;
        size_t end = (bucket + 1) * input_count / used;
        double sum = 0.0;
        for (size_t i = begin; i < end; ++i) {
            sum += input[i];
        }
        output[bucket] = sum / (double)(end - begin);
    }
    return used;
}

void metric_store_init(metric_store_t *store)
{
    if (store) {
        memset(store, 0, sizeof(*store));
    }
}

void metric_store_clear(metric_store_t *store)
{
    metric_store_init(store);
}

bool metric_ring_add(metric_ring_t *ring, const metric_point_t *point)
{
    if (!ring || !point || !point->valid || !isfinite(point->value)) {
        return false;
    }
    for (uint16_t i = 0; i < ring->count; ++i) {
        uint16_t index = (ring->next + METRIC_HISTORY_CAP - ring->count + i) % METRIC_HISTORY_CAP;
        metric_point_t *old = &ring->points[index];
        if (point->block_height != 0 && old->block_height == point->block_height) {
            *old = *point;
            return true;
        }
    }
    ring->points[ring->next] = *point;
    ring->next = (ring->next + 1) % METRIC_HISTORY_CAP;
    if (ring->count < METRIC_HISTORY_CAP) {
        ring->count++;
    }
    return true;
}

static uint16_t chronological_index(const metric_ring_t *ring, uint16_t offset)
{
    return (ring->next + METRIC_HISTORY_CAP - ring->count + offset) % METRIC_HISTORY_CAP;
}

size_t metric_ring_read(const metric_ring_t *ring, uint64_t oldest_ms,
                        metric_point_t *out, size_t out_cap)
{
    if (!ring || !out || out_cap == 0 || ring->count == 0) {
        return 0;
    }
    size_t matching = 0;
    for (uint16_t i = 0; i < ring->count; ++i) {
        const metric_point_t *p = &ring->points[chronological_index(ring, i)];
        if (p->valid && p->timestamp_ms >= oldest_ms) {
            matching++;
        }
    }
    if (matching == 0) {
        return 0;
    }
    size_t used = matching < out_cap ? matching : out_cap;
    size_t matching_index = 0;
    size_t slot = 0;
    for (uint16_t i = 0; i < ring->count && slot < used; ++i) {
        const metric_point_t *p = &ring->points[chronological_index(ring, i)];
        if (!p->valid || p->timestamp_ms < oldest_ms) {
            continue;
        }
        size_t wanted = used == 1 ? matching - 1 : slot * (matching - 1) / (used - 1);
        if (matching_index == wanted) {
            out[slot++] = *p;
        }
        matching_index++;
    }
    return slot;
}

double metric_ring_coverage(const metric_ring_t *ring, uint64_t now_ms,
                            uint64_t window_ms)
{
    if (!ring || ring->count == 0 || window_ms == 0) {
        return 0.0;
    }
    uint64_t oldest = UINT64_MAX;
    uint64_t newest = 0;
    for (uint16_t i = 0; i < ring->count; ++i) {
        const metric_point_t *p = &ring->points[i];
        if (!p->valid || p->timestamp_ms > now_ms || p->timestamp_ms + window_ms < now_ms) {
            continue;
        }
        if (p->timestamp_ms < oldest) {
            oldest = p->timestamp_ms;
        }
        if (p->timestamp_ms > newest) {
            newest = p->timestamp_ms;
        }
    }
    if (oldest == UINT64_MAX) {
        return 0.0;
    }
    double ratio = (double)(newest - oldest) / (double)window_ms;
    return ratio > 1.0 ? 1.0 : ratio;
}

void metric_store_drop_from_height(metric_store_t *store, uint32_t height)
{
    if (!store) {
        return;
    }
    for (size_t s = 0; s < METRIC_COUNT; ++s) {
        metric_ring_t old = store->series[s];
        memset(&store->series[s], 0, sizeof(store->series[s]));
        for (uint16_t i = 0; i < old.count; ++i) {
            const metric_point_t *p = &old.points[chronological_index(&old, i)];
            if (p->block_height == 0 || p->block_height < height) {
                metric_ring_add(&store->series[s], p);
            }
        }
    }
}

void ckb_format_grouped_u64(char *out, size_t size, uint64_t value)
{
    char raw[32];
    snprintf(raw, sizeof(raw), "%llu", (unsigned long long)value);
    size_t digits = strlen(raw);
    size_t commas = digits > 0 ? (digits - 1) / 3 : 0;
    if (digits + commas + 1 > size) {
        snprintf(out, size, "%s", raw);
        return;
    }
    out[digits + commas] = '\0';
    size_t src = digits;
    size_t dst = digits + commas;
    while (src > 0) {
        out[--dst] = raw[--src];
        /* Group by digits already emitted from the RIGHT, not by the
         * left-based index (which put 1000 out as "100,0"). */
        if (src > 0 && (digits - src) % 3 == 0) {
            out[--dst] = ',';
        }
    }
}
