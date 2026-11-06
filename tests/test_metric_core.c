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

/* Header `dao` field: C, AR, S, U as four little-endian u64 (RFC 0023).
 * Vectors are real mainnet headers from get_header_by_number. */
static void test_dao(void)
{
    ckb_dao_t dao;
    /* Genesis: 33.6B CKB issued, AR exactly 10^16. */
    assert(ckb_parse_dao("0x8874337e541ea12e0000c16ff286230029bfa3320800000000710b00c0fefe06", &dao));
    assert(dao.c == 3360000145238488200ULL);
    assert(dao.ar == 10000000000000000ULL);
    assert(dao.s == 35209330473ULL);
    assert(dao.u == 504120308900000000ULL);
    /* Block 20,940,681. */
    assert(ckb_parse_dao("0xd6a3ed86955f8f5bd329a4d588e42a00cdc6dd665a04950a00a0839ce5c63107", &dao));
    assert(dao.c == 6597597074939552726ULL);
    assert(dao.ar == 12073225372838355ULL);
    assert(dao.s == 762520498228545229ULL);
    assert(dao.u == 518414121600000000ULL);
    /* Upper-case hex is still hex. */
    assert(ckb_parse_dao("0xD6A3ED86955F8F5BD329A4D588E42A00CDC6DD665A04950A00A0839CE5C63107", &dao));
    assert(dao.u == 518414121600000000ULL);

    /* Malformed input leaves *out untouched. */
    ckb_dao_t keep = {1, 2, 3, 4};
    dao = keep;
    const char *bad[] = {
        NULL,
        "",
        "0x",
        "d6a3ed86955f8f5bd329a4d588e42a00cdc6dd665a04950a00a0839ce5c63107",     /* no 0x */
        "0xd6a3ed86955f8f5bd329a4d588e42a00cdc6dd665a04950a00a0839ce5c6310",    /* 63 digits */
        "0xd6a3ed86955f8f5bd329a4d588e42a00cdc6dd665a04950a00a0839ce5c6310700", /* 66 digits */
        "0xd6a3ed86955f8f5bd329a4d588e42a00cdc6dd665a04950a00a0839ce5c631g7",   /* non-hex */
        "0Xd6a3ed86955f8f5bd329a4d588e42a00cdc6dd665a04950a00a0839ce5c63107",   /* 0X */
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        assert(!ckb_parse_dao(bad[i], &dao));
        assert(memcmp(&dao, &keep, sizeof(dao)) == 0);
    }
    assert(!ckb_parse_dao("0xd6a3ed86955f8f5bd329a4d588e42a00cdc6dd665a04950a00a0839ce5c63107", NULL));
}

/* Chart frame: 8% headroom, but padding never invents values below zero
 * when the data never goes below zero. */
static void test_chart_bounds(void)
{
    double lo, hi;
    /* Block interval 1..37 s: padding would reach -1.88; clamp to 0. */
    const double interval[] = {1.0, 37.0, 12.0};
    ckb_chart_bounds(interval, 3, false, &lo, &hi);
    assert(lo == 0.0 && fabs(hi - (37.0 + 36.0 * 0.08)) < 1e-9);
    /* Far from zero (difficulty): ordinary padding both ways. */
    const double diff[] = {852.0, 853.0};
    ckb_chart_bounds(diff, 2, false, &lo, &hi);
    assert(fabs(lo - 851.92) < 1e-9 && fabs(hi - 853.08) < 1e-9);
    /* Data that really goes negative (capacity change) keeps its padding. */
    const double delta[] = {0.0, -610.0, 3000.0};
    ckb_chart_bounds(delta, 3, false, &lo, &hi);
    assert(fabs(lo - (-610.0 - 3610.0 * 0.08)) < 1e-9);
    /* Bars always start at zero for non-negative data. */
    const double tx[] = {1.0, 4.0};
    ckb_chart_bounds(tx, 2, true, &lo, &hi);
    assert(lo == 0.0 && fabs(hi - 4.24) < 1e-9);
    /* Flat series: a band around the value, still not below zero. */
    const double flat0[] = {0.0, 0.0};
    ckb_chart_bounds(flat0, 2, false, &lo, &hi);
    assert(lo == 0.0 && hi == 1.0);
    const double flat5[] = {5.0, 5.0};
    ckb_chart_bounds(flat5, 2, false, &lo, &hi);
    assert(fabs(lo - 4.5) < 1e-9 && fabs(hi - 5.5) < 1e-9);
    const double flatneg[] = {-2.0, -2.0};
    ckb_chart_bounds(flatneg, 2, false, &lo, &hi);
    assert(fabs(lo + 2.2) < 1e-9 && fabs(hi + 1.8) < 1e-9);
    /* Always a non-empty range. */
    const double one[] = {0.0};
    ckb_chart_bounds(one, 1, false, &lo, &hi);
    assert(hi > lo);
}

int main(void)
{
    test_hex();
    test_epoch_and_difficulty();
    test_ring();
    test_downsample();
    test_grouped_format();
    test_dao();
    test_chart_bounds();
    puts("test_metric_core: all tests passed");
    return 0;
}
