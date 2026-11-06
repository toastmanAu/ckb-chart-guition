# On-device light client

The firmware runs the ckb-light-esp light client (upstream `3e47434`, see `UPSTREAM_AUDIT.md`)
next to the RPC poller. The light client proves the mainnet tip on its own, and the dashboard
compares that proof with the RPC endpoint.

## What it proves, and what it does not

- **Proven:** a chain tip, by its number and hash. RFC44 last-state proofs from a trusted
  checkpoint check proof of work, MMR inclusion, FlyClient sampling over long gaps, and reorgs
  anchored at a known block.
- **Compared:** the RPC endpoint's hash at the proven height. The comparison is done by
  `network.c`, not by the light client.
- **Not proven:** charts, block lists, transaction counts, difficulty, economics and capacity
  figures. These are RPC data and keep the `RPC` / `STALE` label. A `PROVEN` badge means only
  that the RPC endpoint agrees with an independently proven tip, which makes it unlikely to be
  serving a fake chain. It does not make the RPC's other values verified.
- **Not used:** RFC45 filters. The session runs a filter sync over a dummy lock as part of the
  session protocol, and no chart uses it. Filters are not committed to headers, and fetching
  every block through them is not a way to build network-wide charts.

## How it runs

- **Task** (`firmware/main/lc_task.c`)
  - Runs one P2P session at a time to `CONFIG_CKB_LC_HOST:PORT` (default `192.168.68.105:8115`).
  - The remote peer id is pinned (`CONFIG_CKB_LC_PEER_ID`), and any other id is refused.
  - Reconnects back off from 2 s to 60 s, with jitter.
- **Checkpoint**
  - Starts from the compiled mainnet checkpoint (20,912,678), or from the newer saved one in NVS.
  - The saved checkpoint is namespace `ckb_lc`, key `cp_mainnet`. It sits 16 blocks below the
    proven tip and is written at most every 30 min, plus at the end of a session.
  - If the saved checkpoint is refused, the task falls back once to the compiled one.
- **Priority**
  - The task runs at `tskIDLE_PRIORITY` on core 0.
  - Checking a proof is several seconds of pure CPU. At priority 3 it starved IDLE0 while the
    RPC task did TLS, and the 5 s task watchdog reset the board. At idle priority it shares
    time with IDLE0 instead.
- **Proof-size caps**
  - 500 headers and 4,096 items (`CONFIG_CKB_LC_PROOF_HEADERS` / `_ITEMS`).
  - At boot the task checks that the protocol component was compiled with the same caps
    (`lc_proof_headers_cap`), and disables itself if not.
- **Buffers**
  - About 3.3 MiB of PSRAM, allocated once at boot: rx 768 KiB, block 583, zbuf 583,
    scratch 384, lsp 885 and filter 64.
- **Interface:** `lc_task.h` (`lc_task_start`, `lc_task_status`). It replaced the handoff's
  proposed `ILightClientAdapter` boundary and its `UNAVAILABLE` stub, both removed.

## Badge (`firmware/main/lc_badge.c`, tests in `tests/test_lc_badge.c`)

| Situation | Badge |
|---|---|
| Light client disabled, or demo mode | none |
| The RPC hash differed from the proof at 2 or more different proven heights | `RPC MISMATCH`, shown even when the proof is stale or the session is down |
| Proof at most 5 min old (`LC_PROOF_FRESH_MS`), and the RPC hash matches it | `PROVEN`, with or without an open session |
| Otherwise, with a session open | `ANCHORING` |
| Otherwise, with no session | `LC OFFLINE` |

- **One difference is not an alarm.** It can be a tip race, where the node and the RPC see
  a one-block reorg differently, so it only withholds `PROVEN`. The streak counts
  differences at distinct proven heights and resets on a match.
- **Not compared yet means not `PROVEN`.** This covers an RPC behind the proven height and a
  failed lookup.
- **Mutation testing:** breaking each of the four conditions in turn makes a test fail.

The header source label shows `PROVEN` (green) or `MISMATCH` (red) only while the RPC data is
live. Stale and demo data keep their own labels. Page 6 ("Client") shows the badge, the proven
tip and its age, the comparison result, and the session details.

## Measured on the Guition (2026-11-06)

- **First proof from the compiled checkpoint**
  - Proved tip 20,938,277 with 252 headers and 795 items.
  - Took about 31 s of CPU. No watchdog reset after the priority change.
- **Restart from the saved checkpoint**
  - Started from 20,938,436 and proved tip 20,938,857 with 102 headers and 31 items, in 10.9 s.
  - The first connection attempt after a run of power-loss resets failed once
    (`session open failed: -1`). The retry 4 s later succeeded.
- **Following the chain:** a new proof roughly every block, 2–3 headers each.
  Filter catch-up steps take 4–9 s.
- **RPC comparison:** the public RPC (`mainnet.ckb.dev`), which is independent of the P2P
  peer, matched the proof ("block 20938868 matches the light-client proof").
- **Memory:** internal heap is about 51–61 KB free (minimum 51,315), down from about 80 KB
  without the light client. PSRAM free is about 1.44 MB. Stack high-water marks: rpc 7,392
  bytes, lc about 7,200 bytes left.
- **Power:** on an unpowered hub the board browns out as Wi-Fi associates (POWERON resets with
  the CH340 re-enumerating). It needs a powered hub or a dedicated supply.

## Not yet tested

- A real `RPC MISMATCH` against a lying endpoint. Only the unit tests cover it.
- A long soak with the light client, LVGL and RPC all running together.
- Testnet. The testnet checkpoint (22,952,000) is compiled in but untested on the device.
- Behaviour when the pinned peer is down for a long time. Expected: `LC OFFLINE`, with the
  RPC data still shown under its own label.
