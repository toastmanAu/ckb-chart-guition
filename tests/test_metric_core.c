#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../firmware/main/metric_core.h"

static void test_hex(void)
{
    uint64_t value = 0;
    assert(ckb_parse_hex_u64("0x0", &value) && value == 0);
    assert(ckb_parse_hex_u64("0xffffffffffffffff", &value) && value == UINT64_MAX);
    assert(!ckb_parse_hex_u64("0x", &value));
    assert(!ckb_parse_hex_u64("1", &value));
    assert(!ckb_parse_hex_u64("0xz", &value));
    assert(!ckb_parse_hex_u64("0x10000000000000000", &value));
    assert(!ckb_parse_hex_u64("0x1junk", &value));
    double number = 0;
    assert(ckb_parse_hex_double("0x44b", &number) && number == 1099.0);
}

static void test_epoch_and_difficulty(void)
{
    assert(isnan(ckb_epoch_progress(10, 0, 0)));
    assert(isnan(ckb_epoch_progress(9, 10, 100)));
    assert(ckb_epoch_progress(10, 10, 100) == 0.01);
    assert(ckb_epoch_progress(109, 10, 100) == 1.0);
    assert(ckb_epoch_progress(200, 10, 100) == 1.0);
    double difficulty = ckb_compact_target_difficulty(0x1d00ffff);
    assert(fabs(difficulty - 4295032833.0) < 1.0);
    assert(isnan(ckb_compact_target_difficulty(0)));
    assert(isnan(ckb_compact_target_difficulty(0x1d80ffff)));
}

static metric_point_t point(double value, uint32_t height, uint64_t timestamp)
{
    metric_point_t p = {
        .value = value,
        .timestamp_ms = timestamp,
        .block_height = height,
        .source = SOURCE_FULL_NODE_RPC,
        .quality = QUALITY_LIVE,
        .valid = 1,
    };
    return p;
}

static void test_ring(void)
{
    metric_store_t *store = malloc(sizeof(*store));
    assert(store);
    metric_store_init(store);
    metric_ring_t *ring = &store->series[METRIC_TX_PER_BLOCK];
    for (uint32_t i = 0; i < 600; ++i) {
        metric_point_t p = point(i, i + 1, 1000 + i);
        assert(metric_ring_add(ring, &p));
    }
    assert(ring->count == METRIC_HISTORY_CAP);
    metric_point_t replacement = point(999, 600, 1599);
    assert(metric_ring_add(ring, &replacement));
    assert(ring->count == METRIC_HISTORY_CAP);
    metric_point_t output[240];
    size_t count = metric_ring_read(ring, 0, output, 240);
    assert(count == 240);
    assert(output[0].block_height == 121);
    assert(output[239].block_height == 600);
    assert(output[239].value == 999);
    double coverage = metric_ring_coverage(ring, 1599, 1000);
    assert(coverage > 0.478 && coverage < 0.480);
    metric_store_drop_from_height(store, 550);
    assert(ring->count == 429);
    metric_store_clear(store);
    assert(ring->count == 0);
    free(store);
}

static void test_downsample(void)
{
    double input[8] = {1, 3, 5, 7, 9, 11, 13, 15};
    double output[4];
    assert(ckb_downsample_mean(input, 8, output, 4) == 4);
    assert(output[0] == 2 && output[1] == 6 && output[2] == 10 && output[3] == 14);
    assert(ckb_downsample_mean(input, 2, output, 4) == 2);
    assert(output[0] == 1 && output[1] == 3);
    assert(ckb_downsample_mean(input, 8, output, 0) == 0);
}

static void test_grouped_format(void)
{
    static const struct { uint64_t value; const char *text; } cases[] = {
        {0, "0"}, {7, "7"}, {999, "999"}, {1000, "1,000"}, {12345, "12,345"},
        {123456, "123,456"}, {1234567, "1,234,567"}, {20919340, "20,919,340"},
        {18446744073709551615ULL, "18,446,744,073,709,551,615"},
    };
    char out[40];
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        ckb_format_grouped_u64(out, sizeof(out), cases[i].value);
        if (strcmp(out, cases[i].text) != 0) {
            fprintf(stderr, "grouped %llu: got %s want %s\n",
                    (unsigned long long)cases[i].value, out, cases[i].text);
        }
        assert(strcmp(out, cases[i].text) == 0);
    }
    char tiny[6];
    ckb_format_grouped_u64(tiny, sizeof(tiny), 12345); /* "12,345" needs 7 */
    assert(strcmp(tiny, "12345") == 0);
}

int main(void)
{
    test_hex();
    test_epoch_and_difficulty();
    test_ring();
    test_downsample();
    test_grouped_format();
    puts("test_metric_core: all tests passed");
    return 0;
}
