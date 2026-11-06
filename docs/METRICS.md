# Metrics catalogue, calculation, provenance

Default time selectors `1H / 6H / 24H / 7D / 30D` where data is available; visible window with honest coverage indicator. A 24H chart does not imply prior 24h history until backfill is done. Never show fake 0 for missing blocks.

| View | Metric | Collection and method | Trust | Caveat |
| --- | --- | --- | --- | --- |
| 01 Overview | chain tip height, age, last block interval | `get_tip_header`, header.timestamp previous timestamp | RPC / optionally verified header | interval not fixed |
| 01 Overview | chain epoch number, progress | `get_current_epoch` fields number,start_number,length; progress = clamp((tip - start_number + 1)/length) | RPC | distinguish epoch from 24h |
| 02 Blocks | block intervals, 20-sample moving mean, per-window count | canonical headers and arrival timeline; time differences from header timestamps | RPC | timestamp irregularities; use median |
| 03 Transactions | tx per block **including cellbase** | full `get_block_by_number`/get_block count of transactions array; store count only | RPC | exclude cellbase via max(total-1,0) as optional overlay |
| 03 Transactions | tx/day and 24h tx | observed/backfilled canonical blocks, bucket by UTC timestamp | derived RPC | partial coverage flagged |
| 04 Network | reported pool pending count, pool size, fee stats | `tx_pool_info`, `get_fee_rate_statistics` when node permits | RPC | local node's pool, not global mempool |
| 04 Network | peer count | `local_node_info`/`get_peers` when permitted | RPC | connected peers of endpoint, not network global |
| 05 Economics | epoch difficulty trend | epoch/header compact target or `get_blockchain_info` difficulty; correct compact target conversion | RPC | no assumption that difficulty = hashrate |
| 05 Economics | issuance/miner rewards/fees | `get_block_economic_state` of finalized blocks | RPC | delayed finalisation, null near tip |
| 06 Capacity | total CKB occupied/DAO capacity | validated method/schema from `get_blockchain_info`/header DAO *only if source actually provides it* | RPC | decoding DAO fields is protocol-specific; no unsupported total supply estimate |
| 07 Client | verified tip, peers, sync progress, filter matches, filter failures | embedded adapter internal state | LOCAL VERIFIED STATE | no invented sync percentage |

**Optional later**: watched-script balances and live cells when verified. Never promise network-wide active-address count, distinct wallets, chain-wide cell creation, exact live total supply or true global hashrate directly from filters. For complex totals require full indexer, archive/node data or published indexer API, with source label.

### Charts layout & transforms
Time series cap 240 samples per visible chart by default. Ring buffer per metric 480 raw sample max; backfill batch 20 blocks and yield. Render OHLC? No; this is not price/financial chart. Line chart for block intervals/difficulty, bars for tx counts/fees, segmented arcs for epoch. UTC for data grouping with optional local timezone formatting.

### Sampling schedule
Tip header poll 5–10s configurable; new-block fetch triggered by changed height/hash, cap backfill to 120 blocks per catchup cycle; pool stats 30–60s; epoch 60s or block change; economic state delayed by consensus rules; session cache write every ~2min or on orderly shutdown.

### Reorg
Keep latest 16–32 `(height,hash,parent_hash)` canonical entries. If parent mismatch, walk back to common ancestor within cache; drop derived points in affected range; if deeper reorg clear affected rolling windows and backfill. Surface REORG event.
