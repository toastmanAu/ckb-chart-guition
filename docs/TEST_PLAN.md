# Test matrix

| ID | Case | Expected |
|---|---|---|
| HW01 | boot 480x480 + image / gradients | correct orientation/colours, no black screen |
| HW02 | 5 point calibration + swipe | correct x/y without ghost taps |
| HW03 | 30-minute UI + Wi-Fi concurrent | no crash, responsive touches, measured PSRAM/heap |
| RPC01 | mock JSON fixtures of tip, epoch, block | accurate parsing/hex conversion |
| RPC02 | missing RPC permissions | individual metric unavailable, rest working |
| RPC03 | timeout/malformed JSON/huge response | bounded memory, visible stale badge, reconnect |
| RPC04 | testnet against mainnet config | reject mismatch |
| DATA01 | 240-point chart + 480-point buffer wrap | correct order, no memory growth |
| DATA02 | epoch boundary length changes | correct epoch progress |
| DATA03 | transaction count incl. cellbase | count entire transaction array; optional exclude cellbase = max(n-1,0) |
| DATA04 | reorg prev_hash mismatch | affected points invalidated/backfilled |
| LC01 | RFC44/45 reference proof vectors | accepted valid, rejected corrupted |
| LC02 | conflicting peers/disconnect | no new VERIFIED samples |
| LC03 | watched-script matching | verified inclusion on match, false positive harmless |
| UI01 | all seven screens, time pills, tooltips | no illegible labels/touch misses |
| OFF01 | unplug Wi-Fi, reboot | UI live, old samples stale, retry backoff |
| SOAK01 | 24h with fluctuating connectivity | no leaks, bounded partitions, stable frame rendering |

Agent must record commands, observed results and timestamps; do not pre-check passing cells. Capture memory high-water marks and firmware flash/partition utilization. Prefer unit tests on host for epoch packing and ring buffer before hardware integration.
