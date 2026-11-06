#include "screenshot_stream.h"

#include <stdio.h>

/* 57 raw bytes -> 76 base64 chars; 56 keeps whole 4-byte records per line. */
#define RAW_PER_LINE 56

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

typedef struct {
    uint32_t id;
    uint32_t seq;
    uint8_t raw[RAW_PER_LINE];
    uint32_t used;
    shot_emit_fn emit;
    void *ctx;
} writer_t;

static void flush_line(writer_t *w)
{
    if (w->used == 0) {
        return;
    }
    char line[SHOT_LINE_MAX + 1];
    int n = snprintf(line, sizeof(line), "SHOT_DATA %lu %lu ", (unsigned long)w->id,
                     (unsigned long)w->seq++);
    for (uint32_t i = 0; i < w->used; i += 3) {
        uint32_t left = w->used - i;
        uint32_t v = (uint32_t)w->raw[i] << 16;
        v |= left > 1 ? (uint32_t)w->raw[i + 1] << 8 : 0;
        v |= left > 2 ? (uint32_t)w->raw[i + 2] : 0;
        line[n++] = B64[(v >> 18) & 63];
        line[n++] = B64[(v >> 12) & 63];
        line[n++] = left > 1 ? B64[(v >> 6) & 63] : '=';
        line[n++] = left > 2 ? B64[v & 63] : '=';
    }
    line[n] = '\0';
    w->emit(line, w->ctx);
    w->used = 0;
}

static void put_run(writer_t *w, uint32_t count, uint16_t px)
{
    const uint8_t rec[4] = {(uint8_t)count, (uint8_t)(count >> 8), (uint8_t)px, (uint8_t)(px >> 8)};
    for (int i = 0; i < 4; ++i) {
        w->raw[w->used++] = rec[i];
    }
    if (w->used == RAW_PER_LINE) {
        flush_line(w);
    }
}

void shot_stream(uint32_t id, const uint16_t *pixels, uint32_t width, uint32_t height,
                 uint32_t stride_px, shot_emit_fn emit, void *ctx)
{
    char line[SHOT_LINE_MAX + 1];
    snprintf(line, sizeof(line), "SHOT_BEGIN %lu %lu %lu", (unsigned long)id,
             (unsigned long)width, (unsigned long)height);
    emit(line, ctx);

    writer_t w = {.id = id, .emit = emit, .ctx = ctx};
    uint32_t hash = 2166136261u;
    uint32_t run = 0;
    uint16_t run_px = 0;
    for (uint32_t y = 0; y < height; ++y) {
        const uint16_t *row = pixels + (size_t)y * stride_px;
        for (uint32_t x = 0; x < width; ++x) {
            uint16_t px = row[x];
            hash = (hash ^ (px & 0xffu)) * 16777619u;
            hash = (hash ^ (px >> 8)) * 16777619u;
            if (run > 0 && (px != run_px || run == 0xffffu)) {
                put_run(&w, run, run_px);
                run = 0;
            }
            run_px = px;
            run++;
        }
    }
    if (run > 0) {
        put_run(&w, run, run_px);
    }
    flush_line(&w);

    snprintf(line, sizeof(line), "SHOT_END %lu %lu %08lx", (unsigned long)id,
             (unsigned long)width * height, (unsigned long)hash);
    emit(line, ctx);
}
