/* firmware/main/screenshot_stream.c: RGB565 frame -> SHOT_* text lines.
 * Round-trips frames through an independent decoder written here. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../firmware/main/screenshot_stream.h"

#define MAX_PIXELS (300 * 300)

typedef struct {
    int begun, ended, lines;
    unsigned id, w, h, seq_next, end_pixels;
    unsigned long end_fnv;
    uint16_t out[MAX_PIXELS];
    size_t filled;
    int bad;
} capture_t;

static int b64v(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static size_t b64_decode(const char *in, uint8_t *out)
{
    size_t n = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (; *in && *in != '='; ++in) {
        int v = b64v(*in);
        assert(v >= 0);
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[n++] = (uint8_t)(acc >> bits);
        }
    }
    return n;
}

static void on_line(const char *line, void *ctx)
{
    capture_t *c = ctx;
    c->lines++;
    assert(strlen(line) <= SHOT_LINE_MAX);
    unsigned id, a, b;
    char payload[SHOT_LINE_MAX + 1];
    if (sscanf(line, "SHOT_BEGIN %u %u %u", &id, &a, &b) == 3) {
        c->begun = 1; c->id = id; c->w = a; c->h = b;
    } else if (sscanf(line, "SHOT_DATA %u %u %s", &id, &a, payload) == 3) {
        assert(c->begun && !c->ended && id == c->id);
        if (a != c->seq_next) c->bad = 1;
        c->seq_next++;
        uint8_t raw[SHOT_LINE_MAX];
        size_t n = b64_decode(payload, raw);
        assert(n % 4 == 0);
        for (size_t i = 0; i < n; i += 4) {
            unsigned count = raw[i] | (raw[i + 1] << 8);
            uint16_t px = (uint16_t)(raw[i + 2] | (raw[i + 3] << 8));
            assert(count >= 1);
            assert(c->filled + count <= MAX_PIXELS);
            for (unsigned k = 0; k < count; ++k) c->out[c->filled++] = px;
        }
    } else if (sscanf(line, "SHOT_END %u %u %lx", &id, &a, &c->end_fnv) == 3) {
        assert(id == c->id);
        c->ended = 1; c->end_pixels = a;
    } else {
        assert(!"unexpected line");
    }
}

static uint32_t fnv(const uint16_t *px, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; ++i) {
        h = (h ^ (px[i] & 0xff)) * 16777619u;
        h = (h ^ (px[i] >> 8)) * 16777619u;
    }
    return h;
}

static capture_t *run(unsigned id, const uint16_t *px, unsigned w, unsigned h, unsigned stride)
{
    capture_t *c = calloc(1, sizeof(*c));
    shot_stream(id, px, w, h, stride, on_line, c);
    assert(c->begun && c->ended && !c->bad);
    assert(c->id == id && c->w == w && c->h == h);
    assert(c->filled == (size_t)w * h && c->end_pixels == w * h);
    return c;
}

static void test_single_colour(void)
{
    static uint16_t px[200 * 200];
    for (size_t i = 0; i < 200 * 200; ++i) px[i] = 0x1234;
    capture_t *c = run(3, px, 200, 200, 200);
    /* 40,000 equal pixels are one record: one data line. */
    assert(c->lines == 3);
    assert(c->end_fnv == fnv(px, 200 * 200));
    for (size_t i = 0; i < 200 * 200; ++i) assert(c->out[i] == 0x1234);
    free(c);
}

static void test_run_cap(void)
{
    /* 90,000 equal pixels exceed the u16 count: must split, not wrap. */
    static uint16_t px[300 * 300];
    for (size_t i = 0; i < 300 * 300; ++i) px[i] = 0xF800;
    capture_t *c = run(0, px, 300, 300, 300);
    for (size_t i = 0; i < 300 * 300; ++i) assert(c->out[i] == 0xF800);
    assert(c->end_fnv == fnv(px, 300 * 300));
    free(c);
}

static void test_stride_and_noise(void)
{
    /* 37 wide, stored with stride 40: the 3 padding pixels per row are
     * poison and must never appear in the output. */
    enum { W = 37, H = 23, S = 40 };
    static uint16_t px[S * H];
    static uint16_t visible[W * H];
    uint32_t seed = 12345;
    for (unsigned y = 0; y < H; ++y) {
        for (unsigned x = 0; x < S; ++x) {
            seed = seed * 1103515245u + 12345u;
            uint16_t v = (x >= W) ? 0xDEAD : (uint16_t)((seed >> 16) % 3 ? seed >> 8 : 0x0000);
            px[y * S + x] = v;
            if (x < W) visible[y * W + x] = v;
        }
    }
    capture_t *c = run(6, px, W, H, S);
    assert(memcmp(c->out, visible, sizeof(visible)) == 0);
    assert(c->end_fnv == fnv(visible, W * H));
    free(c);
}

int main(void)
{
    test_single_colour();
    test_run_cap();
    test_stride_and_noise();
    puts("test_screenshot_stream: ok");
    return 0;
}
