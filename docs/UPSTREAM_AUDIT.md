# Upstream audit: toastmanAu/ckb-light-esp

> **Superseded 2026-11-06.** The blocking finding below was fixed upstream, and the
> firmware now links upstream at `3e47434`. See [Re-audit at 3e47434](#re-audit-at-3e47434)
> and the [current integration decision](#integration-decision-2026-11-06). The original
> audit of `6dc3268` is kept unchanged as the record of why integration was refused.

## Source and scope

- Repository: `https://github.com/toastmanAu/ckb-light-esp`
- Audited commit: `6dc326832f23a27261e507ff4556051353c7ff5f`
- Commit date: 2026-07-29T16:25:08+09:30
- Local audit copy: `upstream/ckb-light-esp` (clean at audit time)
- README license declaration: MIT. No standalone `LICENSE`, `LICENSE.md`, or `LICENSE.txt` file exists at this commit.

The audit covered the README, ESP32-S3 defaults, native Wi-Fi/RPC application, public component headers, Molecule/SecIO/Yamux transport layout, RFC44 protocol state machine, MMR implementation, RFC45 filter matching and hash-chain verification, and the host-test layout. The upstream host-test command and its observed result are recorded separately in `docs/VALIDATION.md`; README claims are not treated as test results.

## What is implemented

The tree contains genuine protocol-building blocks, not just an HTTP wrapper:

- `ckb_core`: CKB Blake2b, SipHash, header types, U256 arithmetic, MMR helpers.
- `ckb_transport`: Molecule, SecIO and Yamux codecs/state machines.
- `ckb_protocol`: RFC44 message codecs and a FlyClient-shaped sync state machine.
- `ckb_filter`: RFC45 GCS matching and sequential filter hash-chain verification.
- `ckb_wifi`: native ESP Wi-Fi and an SPI co-processor abstraction.

The native ESP32-S3 application in `main/main.c` is nevertheless an HTTP RPC poller. It calls `get_tip_header`, `get_peers`, and `local_node_info`; it does not run the P2P sync path. The repository also documents a separate host `connect_live` handshake utility.

## Blocking finding: RFC44 proof validation is incomplete

The README says the device “syncs verified block headers using FlyClient proofs.” The code does not substantiate that claim at this commit.

`components/ckb_protocol/src/ckb_protocol.c:472-536` (`lc_sync_process_last_state_proof`):

1. Decodes the response.
2. Checks sampled header numbers are ascending.
3. Checks proof digest difficulty is non-decreasing.
4. Copies the server-supplied tip and sets `LC_SYNC_SYNCED`.

It never calls `ckb_mmr_verify`, never hashes the supplied headers for an inclusion proof, and never verifies the sampled headers against a trusted MMR root. The comments at lines 519-525 explicitly leave full MMR cryptographic verification as future work. Structural ordering is not a proof of canonical chain membership. No state from this state machine may therefore be labeled `LIGHT_VERIFIED`.

`components/ckb_core/src/ckb_mmr.c` contains a standalone MMR verifier, but the RFC44 sync state machine does not connect the response to it. Its existence does not close the gap.

## RFC45 trust boundary

`components/ckb_filter/src/ckb_filter.c:70-94` does verify each filter hash-chain step before matching. That detects corruption against the peer-provided chain. It does not make the peer's checkpoint/hash stream complete or canonical: the filter hashes are not committed to CKB headers. The upstream README correctly says completeness is accepted from one trusted peer. A filter hit is only a probabilistic candidate, not proof that a cell exists.

Consequently, even after fixing RFC44, a deployment would need documented trusted-peer policy, genesis/root anchoring, candidate block/transaction proof checks, malformed-input tests, and explicit per-fact provenance.

## Other observations

- The repository has useful bounded codec and filter components, but static buffers and stack-heavy protocol messages need independent auditing before sharing memory with a full-screen RGB/LVGL application.
- RFC45 reserves a 64,000-byte hash batch plus filter/watch state. RGB frame buffers alone require 921,600 bytes for two 480x480 RGB565 frames.
- Upstream's default S3 config selects 8 MB flash; the target board requires 16 MB.
- No upstream Guition ESP32-4848S040 board support is present.
- The HTTP example truncates responses into a 512-byte buffer and uses substring parsing; it is not reused.

## Integration decision (original, 2026-07, superseded)

**Do not integrate this commit into the dashboard.** No upstream source is linked into `firmware/` and `EmbeddedFilterAdapter` remains `UNAVAILABLE`. This avoids both an unsupported verification claim and an unbudgeted P2P memory/UI-interaction risk.

The shipping modes are deterministic `DEMO` and unverified but usable `FULL_NODE_RPC`/`PUBLIC_RPC`. RPC facts never become light-client-verified facts. Future integration must first implement end-to-end RFC44 proof verification, add valid/corrupted reference-vector tests, define RFC45 peer trust and candidate confirmation, then pass the board's concurrent P2P/Wi-Fi/RGB/PSRAM stress tests.

## Re-audit at 3e47434

- Branch `fix/lsp-pow-verify` (merged to `master` by fast-forward on 2026-11-09), pinned commit `3e47434e` (2026-11-06), checked out at
  `upstream/ckb-light-esp` with no local patches.
- The firmware links `ckb_core`, `ckb_transport`, `ckb_protocol`, `ckb_filter` and `ckb_session`
  from there (`firmware/CMakeLists.txt`, `EXTRA_COMPONENT_DIRS`).

The RFC44 blocking finding is closed by these commits:

| Commit | What a SendLastStateProof must now satisfy |
|---|---|
| `4f9b70b` | Every header carries real proof of work for its network (mainnet Eaglesong, testnet ckbhash(Eaglesong)). |
| `0691dfe` | A sample-free proof is the full contiguous run from the trusted start to the tip: parent hashes link, the in-epoch target is constant, per-epoch change is at most 4x. |
| `029b5dd` | FlyClient-sampled proofs: the start block itself is the first sample, MMR-proven under the tip, and each header's carried total difficulty equals its parent's. |
| `a230855` | Reorgs: the proof is anchored at a block the client already knows, so a reorg is followed rather than accepted blind. |
| `3e47434` | Under ESP-IDF the proof-size caps from `sdkconfig` take effect (see below). |

With these, the MMR inclusion checks run on every proof. The upstream host suite passes at
`3e47434`: 2,595 checks, 0 failures, including corrupted and mutated proofs that must be refused.

### Defect found during integration: proof caps ignored on ESP-IDF

- **What went wrong.** `MAX_SAMPLED_BLOCKS` and `MAX_LSP_PROOF_ITEMS` come from
  `CONFIG_CKB_LC_PROOF_HEADERS` and `_ITEMS`. No upstream component included `sdkconfig.h`, so
  every ESP build silently used 64 for both. On the device, the first 500-header proof request
  could not be built.
- **The fix (`3e47434`).** The protocol component now includes `sdkconfig.h` and exports the
  compiled caps (`lc_proof_headers_cap`, `lc_proof_items_cap`). `lc_task.c` refuses to start
  the light client if its own view of the caps differs.
- **Evidence.** The built object holds 500 (`0x1f4`). Removing the include drops it back to 64.
- **Correction.** An earlier note in this project said the xtensa builds were clean "at caps
  64 and 500". Both of those builds were really at 64.

### What remains outside the proof

- **The peer chooses the tip.** RFC44 proves that a tip has real work behind it and links
  to the trusted checkpoint. It does not prove the tip is the heaviest chain anywhere,
  since only one peer is asked. The firmware pins that peer by host and peer id.
- **RFC45 filters are not committed to headers.** As in the original audit, a filter hit is
  a candidate, not proof, and the dashboard does not use filters for its charts.

## Integration decision (2026-11-06)

**Integrated, with a narrow claim.** The device proves the chain tip on its own and
compares the tip hash with the RPC endpoint. That comparison is the only fact the
`PROVEN` badge vouches for. Every chart, block list and statistic is still RPC data and
keeps its RPC trust label. See `LIGHT_CLIENT.md` for the badge rules and the device
measurements.
