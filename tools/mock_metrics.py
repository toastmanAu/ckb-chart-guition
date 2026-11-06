#!/usr/bin/env python3
"""Deterministic mock metric generator for host/UI development. Not chain data."""
import json, math
BASE=1793650000000
samples=[]
for i in range(120):
    samples.append({"id":"transactions_per_block","timestamp_ms":BASE+i*10000,
       "capture_ms":BASE+i*10000,"value":int(3+(i%7)+2*math.sin(i/4)),
       "unit":"tx/block including cellbase","source":"DEMO","quality":"LIVE", "chain":"mainnet",
       "block_height":17000000+i,"period_sec":0})
print(json.dumps(samples,indent=2))
