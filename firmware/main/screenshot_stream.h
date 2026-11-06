#pragma once
/*
 * Debug screen capture over a text console (CONFIG_CKB_DEBUG_SCREENSHOTS).
 *
 * One RGB565 frame becomes plain text lines that survive any line-based
 * serial logger:
 *
 *   SHOT_BEGIN <id> <w> <h>
 *   SHOT_DATA <id> <seq> <base64>      seq = 0, 1, 2 ...
 *   SHOT_END <id> <pixels> <fnv1a32 hex>
 *
 * The base64 payload is a stream of run records, 4 bytes each: u16 count
 * (1..65535) then the u16 pixel, both little-endian, row-major over the
 * visible w x h pixels. The FNV-1a hash covers the visible pixels as
 * little-endian bytes, so the decoder (tools/screenshot_decode.py) can
 * prove it rebuilt the exact frame. Pure C, no ESP-IDF: host-tested.
 */
#include <stdint.h>

#define SHOT_LINE_MAX 112

typedef void (*shot_emit_fn)(const char *line, void *ctx);

/* stride_px: pixels per stored row (>= w). */
void shot_stream(uint32_t id, const uint16_t *pixels, uint32_t w, uint32_t h,
                 uint32_t stride_px, shot_emit_fn emit, void *ctx);
