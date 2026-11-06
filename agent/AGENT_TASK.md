# Coding-agent build contract

You are implementing a locally buildable, maintainable ESP32-S3 firmware for the Guition **ESP32-4848S040C** board. Produce source, lockfiles/manifests, commands, logs, tests, and a real firmware binary only when actually compiled. Do not falsely mark tests as run.

## Mandatory discovery gate (before implementation)
- Identify exact silkscreen PCB revision, flash/PSRAM and touch presence; photograph/log board. Confirm Guition S040C vs S040 variants.
- Clone `toastmanAu/ckb-light-esp` and pin its commit; inspect LICENSE, targets, transport, P2P, storage, cryptography, filter semantics, proof validation, memory use, public headers and tests. Produce `docs/UPSTREAM_AUDIT.md` with evidence. If repo inaccessible, leave `EmbeddedFilterAdapter` as `UNAVAILABLE` and ship working RPC-only dashboard without representing it as verified.
- Identify whether upstream supports actual RFC44/45 full flow *on ESP32* or merely full-node HTTP polling/filter convenience. Do not equate RPC `get_block_filter` with verified filtering.
- Pin ESP-IDF, LVGL, Guition board component, dependencies by exact revisions. Verify board driver against vendor demo/known working sample before custom pins.

## Implementation phases and exit criteria
P0: build environment and reproducible debug toolchain, documented partitions and serial logging.
P1: display backlight, 480x480 RGB, GT911 touch; stress-test Wi-Fi + PSRAM concurrent use, no tearing/reboots. Display a 60fps timer stress-test only if achieved; target responsive touch, not mandatory 60fps.
P2: LVGL UI, seven screens, demo mode with deterministic fixture series; swipe and bottom nav, landscape not assumed.
P3: Wi-Fi credentials stored locally in NVS, endpoint config, timeout, reconnect with jitter; no secrets logged; RPC source validity badges; testnet/mainnet genesis mismatch guard.
P4: metric ingestion, bounded ring buffers, backfill, sample quality flags; canonical block hash history, reorg handling, epoch changes, timestamps, throttling.
P5: actual light-client integration via adapter and proof/peer validation; report VERIFYING / VERIFIED / DEGRADED / UNAVAILABLE. Only claim VERIFIED if RFC44/45 trust conditions implemented and passing.
P6: power-cycle, 24h Wi-Fi soak, offline resume, memory-pressure test, malformed JSON/filters, and reproducible flash.

## Non-negotiable engineering
- Use ESP-IDF C/C++ and LVGL 9.x unless actual board-driver compatibility forces explicitly documented alternate version. Prefer maintained Guition component for exact board revision; no silent library version jumps.
- LVGL calls happen on the UI task under the appropriate lock; transport and verification tasks never touch LVGL objects.
- Fixed-size/bounded queue between tasks. PSRAM used for framebuffers/history; internal DMA-capable allocations per ESP-IDF RGB driver constraints.
- Do not request or plot full unbounded chain history; e.g. 120/240/480 datapoints/window. Use downsampling for zoom.
- Networking is separate from chart render. Parse JSON with bounds checks, max HTTP body size, timeout and retry; avoid allocating a full block where only counts are needed, if source API can supply minimal data.
- Strict source labels: `LIGHT_VERIFIED`, `FULL_NODE_RPC`, `PUBLIC_RPC`, `DERIVED`, `ESTIMATE`, `STALE`, `DEMO`; any metric can combine trust and age. Verification applies to individual facts, not entire UI globally.
- Any P2P proof verification happens only with documented checks. Never silently trust a single remote peer or relabel RPC data verified.
- Do not assume historical hashrate, total supply or daily active addresses directly exists in light-client interface. Provide optional full-node/indexer source or explicitly exclude.
- Graphs should work without light client loaded; show a persistent amber `RPC` source label, not a green VERIFIED.

## Agent outputs
1. Codebase with ESP-IDF/PlatformIO instructions, `idf.py build`, `idf.py flash monitor`, optional emulator mock.
2. Hardware qualification report and light client source audit.
3. Tests with recorded commands/results and README usage.
4. UI screenshots/photo on actual hardware, heap/PSRAM high-water marks and reconnect logs.
5. Explicit unresolved issues, actual limitations, binary size and compile options.

## Done means
Build + flash + LCD/touch + 7 usable dashboards + real chain RPC metrics + explicit provenance + resilience + tests. Integrated filter client is a separate conditional milestone; never claim complete unless independently validated.
