# Suggested local commands and build sequence

1. `git clone https://github.com/toastmanAu/ckb-light-esp.git upstream/ckb-light-esp` (may require GitHub credentials; if inaccessible mark gap).
2. `git clone https://github.com/toastmanAu/CKB-ESP32.git upstream/CKB-ESP32` (optional RPC reference).
3. Set up an Espressif ESP-IDF stable environment, pin actual version and run `idf.py --version`.
4. Use vendor Guition `esp32_4848s040` driver/demo to qualify backlight, RGB, GT911. Save the validated BSP config into source control.
5. Add LVGL 9 components version-pinned; display offline mock dashboard from `python3 tools/mock_metrics.py` fixture.
6. Implement bounded RPC client and ring buffers; compare 3 sampled values against local full node.
7. Integrate `ILightClientAdapter` from audited upstream only if works on board; otherwise `UNAVAILABLE` and document.
8. `idf.py build`, `idf.py -p /dev/ttyACM0 flash monitor` (replace port with real one); save build logs.
9. Run host tests: `bash tools/run_host_tests.sh`; run on-hardware test matrix and 24h soak.

**Current state:** The ESP-IDF 5.4.0/LVGL 9.2.2 firmware, pinned component lockfile, upstream audit copy, build logs, host tests, and compiled binaries are present. Follow `README.md` for the current build/flash commands. Hardware qualification and the 24-hour soak remain pending; do not flash until the exact board revision is identified.
