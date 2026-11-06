# Decisions and risks

- User chose Guition ESP32-4848S040C, LVGL, many switchable metric charts, CKB block-filtering client, local coding agent ZIP.
- Default framework ESP-IDF C/C++ with LVGL 9; actual dependency set pinned by implementer after driver qualification.
- No network-wide metrics masquerading as block-filter output. RPC metrics and verified on-device light-client facts separately labelled.
- `ckb-light-esp` was audited at `6dc326832f23a27261e507ff4556051353c7ff5f`; its RFC44 sync accepts the server tip without calling the MMR verifier. No upstream source is linked into the firmware and the embedded adapter remains `UNAVAILABLE`; see `UPSTREAM_AUDIT.md`.
- Caveat: suffix board variants could differ; board pin mapping from reference board must be checked.
- Risks: RGB buffer/PSRAM contention, Wi-Fi and light-client stack contention, peer connectivity over ESP-IDF, proof verification compute/storage, filter backfill latency and RPC permissions.
- Design is display-only; do not include wallet transaction signing scope.
