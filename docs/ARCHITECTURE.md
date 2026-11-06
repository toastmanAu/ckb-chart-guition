# Architecture and execution model

```
Wi-Fi --> (optional peer P2P RFC44/45 verifier) --> Verified event adapter --+
      --> CKB HTTP JSON-RPC ------------------> RPC metric adapter -------+--> Normaliser --> bounded time-series store --> UI event queue --> LVGL dashboard
      --> optional indexer/remote metrics ------> auxiliary adapter ------+

NVS config + Wi-Fi manager <-- settings screen
LittleFS/NVS snapshots <-- history persister (not every block)
```

**Mode A RPC dashboard** ships independently. **Mode B verified embedded client** only after upstream audit and protocol tests. **Mode C external local light-client RPC** can query a Rust `ckb-light-client` hosted on user's LAN; label as `EXTERNAL_LIGHT_CLIENT` and do not claim proof verification happened on-board.

### FreeRTOS tasks (proposed; tune against hardware)
- LVGL/UI: main core policy defined by board driver; ~5-10ms LVGL tick; must own widgets.
- RPC worker: periodic requests and bounded JSON parse; enqueue metric events. No LVGL calls.
- Embedded light client worker: separate stack/heap budget, bounded I/O and snapshot; must not starve display.
- Persister: batch writes and atomic snapshots, guard wear.

### Nominal budgets (engineering targets, not measured)
- 480×480×2 bytes = **460,800 bytes per full RGB565 framebuffer**; two = **921,600 bytes** in PSRAM, plus draw buffers and driver overhead. Confirm RGB DMA cache coherency and correct LCD buffer allocation.
- Chart histories: 12 series × 480 × (8-byte value + 8-byte timestamp + 2-byte flags, padded) ≈ 138KB, plus heap and queues. Prefer 2–8 byte compact encodings and ~120–240 points per metric.
- UI queue max 32 events, no raw variable-size JSON passed to renderer.
- UI refresh ~5–10Hz on changing numbers; chart redraw on new datapoint / navigation. Avoid re-rendering entire plots per frame.

### Data correctness
`MetricSample` includes source, quality, chain ID, block height, block hash, UTC ms, capture ms, unit, period, aggregation method, value, and flags. Never fill missing samples with zero; mark gap. On reorg, invalidate/rewrite affected block-derived samples by hash. RPC-only metrics independent from locally verified filters.

### Persistence
NVS: ssid, config, chain, endpoint with version/CRC. LittleFS: rotating chunked history/last tip hash; no sensitive key material. SD card optional only after verifying physical socket and pins. Backfill limited by endpoint availability, configurable (e.g. 120 previous blocks; 24h aggregates require acquisition strategy).
