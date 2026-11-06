# LVGL 480x480 touchscreen UX

Visual design: dark graphite background `#081014`, subdued grid `#203039`, Nervos emerald `#00D68F`, sky accent `#59B9FF`, warning amber `#FFB547`, critical red `#FF5D6C`. Typography: numerals readable from ~1m; no downloading fonts at runtime. Card-like display with 10px gutters, safe touch targets >=44px.

Reusable shell: top 46px network badge / `CKB` / age + Wi-Fi; body 360px KPI and chart; bottom ~64px navigation/small page indicators. One metric view per screen with swipe left/right; tap bottom nav for overview/blocks/tx/network/economics/capacity/client (seven screens), hold chart to inspect an exact point, tap time-range pill to cycle windows, settings via top-right gear.

Screens:
1 Overview: large `#12,345,678` tip, age, epoch % radial progress, recent blocks mini-bar.
2 Blocks: interval line with moving mean and last-min/median/max; highlight delayed block.
3 Transactions: per-block histogram `including cellbase` toggle, total last hour/day (coverage dependent).
4 Network: mempool pending trend + node-specific peer count and fee rate where allowed; missing permissions gracefully unavailable.
5 Economics: difficulty sparkline and finalized reward/fee bars; delay explanation for recent reward nulls.
6 Capacity: strictly sourced capacity indicators with units and availability warnings; substitute `Network Data` source health if capacity data unavailable.
7 Client: light-client tip, verified status and filters/peers; use RPC source-only mode when upstream inaccessible.

Settings: Wi-Fi and local node URL (HTTP allowed only as explicit trusted LAN option), mainnet/testnet, monitoring mode, refresh, brightness, screen timeout, demo mode, clear history. Never log Wi-Fi password. Endpoint remote HTTPS certificate validation mandatory; no disabling TLS certificate verification.

States: `CONNECTING`, `RPC`, `SYNCING`, `VERIFIED`, `STALE`, `ERROR`; use text+icon not colour alone. On stale data display last valid sample, age badge, and paused trend rather than fake zero.
