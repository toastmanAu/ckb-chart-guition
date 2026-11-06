# Validation and hardware qualification report

Validation date: 2026-11-03 (local logs use UTC+10:30).

## Overall status

**An RPC-default firmware with Wi-Fi scanning and custom-node provisioning was flashed to the user-confirmed Guition ESP32-4848S040C. Display, GT911 touch, buttons, swipes, and all seven screens passed earlier demo-mode interactive checks. The final no-scan build was stable through its memory sample. No successful provisioning scan or RPC connection was recorded, so those features remain unvalidated. Negative/fault-injection tests, 30-minute Wi-Fi/display stress, and the 24-hour soak also remain open.**

The exact PCB silkscreen revision and board/display photographs were not recorded. Chip identification and the user's explicit port confirmation establish the target used for final testing; they do not document every PCB variant.

## Target identification and flash

After the POS terminal was disconnected, the user explicitly confirmed `/dev/ttyUSB0` as the Guition port. Read-only esptool identification then reported:

- ESP32-S3 QFN56 revision 0.2, USB-Serial/JTAG-capable MCU behind the CH340 interface.
- Embedded 8 MB PSRAM.
- 16 MB quad flash, 3.3 V.

Evidence: `logs/guition-identification.log`.

The final application, bootloader, and partition table were written to `/dev/ttyUSB0`; esptool reported hash verification for every region. Evidence: `logs/guition-final-build-flash.log`.

**Log provenance warning:** `/dev/ttyACM2` was an unrelated Blackbox POS terminal and was mistakenly flashed before the user corrected the device selection. `logs/flash-demo.log`, `logs/board-identification.log`, and the failed `/dev/ttyACM2` capture are not Guition validation. No original POS firmware backup was made. All final target results below use `/dev/ttyUSB0` only.

## Display and touch bring-up findings

The first Guition boot used Espressif's generic ST7701 init. The user observed horizontal red/green/blue/dark bands, so HW01 failed at that stage. GT911 also failed because the legacy I²C LCD IO API requires `scl_speed_hz=0`; the macro's nonzero default was invalid.

Corrections:

- Ported the Guition-specific ST7701 power/gamma/gate sequence from the documented working ESP32-4848S040 sample.
- Changed to 6 MHz PCLK, HSYNC pulse/back/front 8/50/10 and VSYNC 8/20/10.
- Set legacy GT911 LCD IO `scl_speed_hz=0`, retaining the actual I²C bus at 400 kHz.

After reflashing, the user confirmed the dashboard looked correct. GT911 logged ID bytes `0x39,0x31,0x31` (`911`), config version 99, and `GT911 ready`. The user confirmed both navigation buttons and swipes work, then separately confirmed HOME, BLK, TX, NET, ECON, CAP, and LC all open correctly.

Evidence:

- `logs/guition-panel-init-fix.log` and `logs/guition-touch-fix-final.log` — corrected builds/flashes.
- `logs/guition-final-runtime.log` — final boot and runtime memory.
- Interactive user confirmations are summarized here; no photograph or five-point coordinate recording was captured.

## RPC default and Wi-Fi provisioning follow-up (2026-11-04)

The default was changed from DEMO to public mainnet RPC. The settings UI now includes masked Wi-Fi credentials, a custom RPC URL, public-versus-LAN source selection, and an asynchronous scan with an eight-network RSSI-sorted picker. NVS configuration version 2 migrates the previously flashed board from demo to RPC without erasing other settings. The snapshot queue was reduced from 32 to 8 after Wi-Fi setup reduced internal heap headroom.

The first scan attempt failed because an already-created default event loop was treated as fatal; that was corrected. A later experimental boot-time scan caused a repeatable Core 1 LVGL `LoadProhibited` reboot loop. Boot scanning was removed, asynchronous overlay deletion was applied, and the restored build completed a 35-second boot/runtime capture without a crash (`logs/wifi-restore-stable-runtime.log`). Subsequent provisioning captures showed memory logs but no scan request/completion, so **scan results and RPC connectivity are not marked as passing**. The intermediate failure logs are retained under `logs/wifi-*.log`.

## Build and pinned configuration

Toolchain: ESP-IDF 5.4.0, Xtensa ESP-ELF GCC 14.2.0, esptool 4.12.0. Dependency lock: `firmware/dependencies.lock`. LVGL 9.2.2, ST7701 driver 2.0.2, 3-wire SPI additions 1.0.0, GT911 driver/base 1.2.1.

Final artifacts:

| Artifact | Bytes | SHA-256 |
|---|---:|---|
| `firmware/build/ckb_chart.bin` | 1,478,432 | `df6b2d14b3fea77b7a937937fdbe5fa56bcc579b6e3822b2c18828dec0b74547` |
| `firmware/build/bootloader/bootloader.bin` | 22,432 | `45e9c95e5d44e9e4b2b5ee1ffb3e391a38c2e5fbc247c8aa7244a3b6e7868958` |
| `firmware/build/partition_table/partition-table.bin` | 3,072 | `b032755036af3dfc02baa4de9093ce280e51101b7ea2f240cd1cd4e47c9e5627` |

The 3 MiB application partition has 53% free. Final ELF summary: 943,724 bytes flash code, 417,224 bytes flash data, 192,803/341,760 bytes DIRAM (56.41%), and 16,383/16,384 bytes in the separately reported IRAM region. See `logs/firmware-size-map.log`, `logs/firmware-sizes.txt`, and `logs/firmware-sha256.txt`.

## Runtime memory and short stability observation

At 31.375 seconds after final boot, in DEMO mode with the UI running:

- UI task stack high-water free: **5,588 bytes**.
- Internal heap free: **85,819 bytes**.
- Minimum internal heap free since boot: **81,363 bytes**.
- PSRAM free: **6,583,188 bytes**.
- Minimum PSRAM free since boot: **6,583,188 bytes**.

The final 38-second capture showed successful PSRAM memory test, Guition panel init, GT911 init, application startup, and the memory sample, with no LCD flush error, watchdog, exception, or reboot. This is only a short demo observation, not a Wi-Fi stress test or soak.

Configured allocations include two 460,800-byte PSRAM framebuffers, two 9,600-byte internal bounce buffers, a 768 KiB PSRAM RPC body buffer, PSRAM cJSON allocations, and bounded PSRAM metric rings.

## Host tests

```bash
PYTHONDONTWRITEBYTECODE=1 bash tools/run_host_tests.sh
```

Result: PASS (`logs/host-tests.log`).

- Python JSON-schema/demo contract: 1 test passed, covering all 120 generated demo samples.
- Native C metric suite passed: valid/malformed/overflow hex input, epoch boundaries and clamping, compact-target difficulty, 600-point insertion into a 480-point ring, duplicate replacement, ordered 240-point sampling, coverage, reorg invalidation, clear, and downsampling.

The first ring test exposed an ordering bug; it was fixed and rerun to pass.

Upstream audit tests:

```bash
make -C upstream/ckb-light-esp/test_host clean test
```

Result: 292/292 PASS (`logs/upstream-host-tests.log`). These tests do not close the RFC44 integration gap documented in `docs/UPSTREAM_AUDIT.md`: the sync state machine accepts a tip without calling its MMR verifier.

## Live public RPC schema check (host only)

Endpoint: `https://mainnet.ckb.dev/rpc`. These were Python host requests, not firmware on Wi-Fi.

The default Python user agent received HTTP 403 (`logs/public-rpc-schema.log`). Repeating with `User-Agent: ckb-chart-esp32/1.0` and `Accept: application/json` succeeded (`logs/public-rpc-schema-retry.log`); firmware sends those explicit headers.

Observed live results:

- `get_blockchain_info.chain = "ckb"`.
- Mainnet genesis `0x92b197aa1fba0f63633922c61c92375c9c074a93e85963554f5499fe1450d0e5`.
- Tip fields number, timestamp, hash, parent hash, and compact target.
- Current epoch number, start number, length, and compact target.
- Full block header and transaction array, including cellbase in the count.
- Pool pending/bytes, median fee rate, peer connections, and node version.

The live check caught and corrected a truncated genesis constant. Mainnet and testnet hashes now match CKB's published `docs/hashes.toml`: <https://github.com/nervosnetwork/ckb/blob/develop/docs/hashes.toml>. Public RPC facts are not light-client verified.

## Test matrix disposition

| ID | Result | Evidence / limitation |
|---|---|---|
| HW01 LCD boot/colors | PASS (interactive) | Generic init failed visibly; Guition-specific init fixed it. No photo. |
| HW02 touch/swipe/five points | PARTIAL PASS | GT911 ready; buttons and swipes confirmed. No five-point coordinate log. |
| HW03 30-minute UI+Wi-Fi/PSRAM | NOT RUN | Only a 38-second final DEMO capture; no concurrent Wi-Fi stress. |
| RPC01 parsing/hex | PARTIAL PASS | Host helpers/live schemas passed; firmware RPC not run on device. |
| RPC02 missing permissions | NOT RUN | Independent unavailable flags implemented; no restricted endpoint test. |
| RPC03 timeout/malformed/oversize | NOT RUN | Bounds/timeouts implemented; no firmware fault injection. |
| RPC04 chain mismatch | PARTIAL | Mainnet chain/genesis observed live; no negative mismatch/testnet device test. |
| DATA01 ring wrap/downsample | PASS (host) | Native C test passed. |
| DATA02 epoch boundary | PASS (host) | Zero length, before start, completion, and clamp tested. |
| DATA03 tx including cellbase | PASS (host schema) | Live response array counted; no device/RPC chart comparison. |
| DATA04 reorg | PARTIAL PASS (host) | Height invalidation tested; live reorg/backfill not exercised. |
| LC01–LC03 | NOT RUN / UNAVAILABLE | Upstream integration rejected at audit gate. |
| UI01 seven screens/controls | PASS (interactive) | All seven screens, buttons, and swipes confirmed by user. |
| OFF01 offline resume | NOT RUN | Reconnect/stale code compiled; no Wi-Fi disconnect/physical power-cycle test. |
| SOAK01 24-hour soak | NOT RUN | No 24-hour run, leak test, or reconnect log. |

## Unresolved issues and limitations

1. **On-device RPC remains unvalidated.** Provision Wi-Fi locally, then test TLS, live chart values, timeouts, malformed/oversize responses, restricted methods, reconnect, and negative chain/genesis cases.
2. **Long-term qualification remains open.** Run five-point touch coordinates, 30-minute RGB+Wi-Fi stress, physical power cycles, memory-pressure/backfill tests, and a 24-hour soak. Capture display/board photographs and PCB revision.
3. ~~**No embedded verified mode.**~~ Superseded 2026-11-06: the on-device light client proves the tip and the RPC hash is compared with it (`docs/LIGHT_CLIENT.md`). RPC data is still never relabeled verified; only the tip comparison is.
4. **History is small and volatile.** Backfill is 20 blocks per tip change; rings hold 480 points and render 240. Long ranges show honest low coverage. LittleFS/history restore is not implemented despite the reserved partition.
5. **Provisioning is first-boot/menuconfig based.** NVS persists; no on-screen editor exists for SSID/password/URL. Reprovisioning with build defaults currently requires deliberate flash erasure.
6. **Some metrics are intentionally excluded.** Finalized rewards and circulating supply remain unavailable instead of using unsupported estimates. Pool/peer values are endpoint-local. (Since 2026-11-06 the Capacity page shows occupied/issued capacity and the DAO rate decoded from header `dao` fields; see `docs/METRICS.md`.)

This delivery passes demo hardware bring-up on the tested Guition, but full RPC and resilience qualification is not complete.

## 2026-11-04 repair pass (Claude Code)

Root causes found and fixed. Logs: `logs/claude-fix-*.log`.

1. **Silent stack overflow → Core 1 LVGL `LoadProhibited` crash loop.** `network_task` had an 8,336-byte frame (3.7 KiB snapshot locals inlined from `poll_rpc`/`scan_wifi*`) inside a 12 KiB stack. Wi-Fi start/scan and TLS ran past the end without hitting the FreeRTOS canary. The large frame jumped over it and corrupted the neighbouring UI task, which is why the panics landed in `lv_refr` with CORRUPTED backtraces. Snapshots are now static; frame is 192 bytes; measured RPC stack peak ≈4.5 KiB. The previously crashing boot-scan scenario (`CONFIG_CKB_DEBUG_BOOT_SCAN`) now completes in ~2 s.
2. **RPC never returned data.** `on_http` dropped every chunk because `user_data` was never set, so every call failed as `ESP_OK (HTTP 200)`. Fixed; live mainnet tip 20,919,340 with 20 blocks observed.
3. **Every poll was a full 20-block re-download and every new block looked like a reorg.** `parse_tip` wrote the new hash into `s_last`. Fixed. Backfill is now incremental (only new heights, with a parent-hash link check; a mismatch falls back to a full backfill and is flagged as a reorg).
4. **Provisioning state machine.** `esp_wifi_connect()` was re-issued every loop pass (aborting handshakes). Scans were refused while a connect attempt was in flight (`ESP_ERR_WIFI_STATE`), so re-scanning after a bad password was impossible. Controls waited behind `vTaskDelay` of up to 30 s. The UI's sticky `scan_complete` showed stale results and hid fresh ones. Now: event-driven connect with backoff, disconnect reasons shown in the status line ("wrong password?", "network not found"…), scans abort pending connects, waits wake on controls, scan results carry a sequence number, SSIDs are deduplicated, open networks and WPA3 (SAE H2E) are supported, and the Wi-Fi driver keeps credentials in RAM (no flash writes on `set_config`).
5. **UI robustness.** Overlays are hidden before `lv_obj_delete_async`, so a double tap cannot double-free. Scan-list callbacks are null-guarded. Config strings are copied under a mutex across tasks.
6. **Memory / display.** The snapshot queue (≈30 KiB) and TLS buffers moved to PSRAM: internal free 92→103 KiB, UI stack headroom 5.3→8.9 KiB, LVGL pool 53% used. `SPIRAM_XIP_FROM_PSRAM` + `LCD_RGB_ISR_IRAM_SAFE` + `LCD_RGB_RESTART_IN_VSYNC` keep the RGB panel running during NVS writes.
7. **Thousands separators** were grouped from the left (`1000` → `100,0`). Moved to `ckb_format_grouped_u64()` in `metric_core.c` with host tests.

Still unverified: touch-driven provisioning end-to-end (scan list → select → save), wrong-password recovery on-device, and the 30-minute and 24-hour soaks.
8. **Power brownout (hardware).** On the shared, unpowered USB hub the board power-cycled under the Wi-Fi calibration and TLS load: the CH340 dropped off USB and boots reported `rst:0x1 POWERON`. It is stable on a dedicated supply. The backlight now soft-starts at 15% and ramps after the Wi-Fi bring-up peak.
9. **Flicker.** `flush_display` handed the framebuffer to the RGB driver on every dirty area. That scheduled buffer swaps onto partly drawn frames, and the buffer was released before vsync. It now swaps only on the last area and waits on the panel's `on_vsync` before `lv_display_flush_ready`. On-device: no vsync timeouts, no resets in 110 s. Visual confirmation is pending.
10. **Refresh rate.** PCLK raised from 6 to 12 MHz. At 6 MHz the panel refreshed at only ~21 Hz, a visible shimmer; it is now ~42 Hz. Confirmed flicker-free on device.
11. **UI.** LVGL's printf was built without float support, so `%.1f` rendered as "f". Text now uses libc `vsnprintf`. Charts have max/mid/min y-axis labels and are mean-downsampled to 60 line points or 30 bars. The top section shows live values. Page layout no longer overlaps the status bar.
12. **Nav button text wrapping.** The default theme's button padding (~13–20 px per side at 130 DPI) left a 64 px button too narrow for "HOME". Padding is now 2 px and captions are single-line. Changing the RGB porch timing first was a misdiagnosis: the panel uses DE mode, and porches had no visible effect. They were reverted to 8/50/10 and 8/20/10.
13. **Bottom-bar shift: actual root cause.** Item 12's padding change was not the fix. `CONFIG_LCD_RGB_RESTART_IN_VSYNC` (enabled earlier in this pass) restarts GDMA every frame via the driver's restart link, which begins `LCD_FIFO_PRESERVE_SIZE_PX` = 17 px into the buffer. That shifted the nav rows ~17 px left with wrap-around. Disabled; the driver still auto-restarts on detected desync. Confirmed fixed on device from a photo.
14. **Black screen after flashing** was the bench capture script leaving RTS (wired to EN) asserted on close. Flashing was always correct. Capture scripts must set DTR/RTS low before open and leave them released; `stty -F /dev/ttyUSB0 -hupcl` stops close from toggling them. Opening the port still resets the board once (kernel raises the lines on open); that is harmless.
