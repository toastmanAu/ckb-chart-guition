#pragma once
/*
 * On-device CKB light client (ckb-light-esp ckb_session, checkpoint mode).
 *
 * One P2P session at a time to CONFIG_CKB_LC_HOST. The session proves the
 * peer's tip from a trusted checkpoint with RFC 0044 last-state proofs
 * (proof of work, MMR inclusion, FlyClient sampling over long gaps, reorgs
 * anchored at a known block). Nothing here trusts the JSON-RPC endpoint: the
 * network task compares the two.
 */
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    LC_STATE_DISABLED = 0,   /* not compiled in, or demo mode */
    LC_STATE_WAITING,        /* no Wi-Fi yet */
    LC_STATE_CONNECTING,     /* opening TCP + SecIO + yamux */
    LC_STATE_ANCHORING,      /* session open, no tip proven this session yet */
    LC_STATE_PROVEN,         /* session open, tip proven */
    LC_STATE_RETRYING,       /* session ended; backing off */
} lc_state_t;

typedef struct {
    lc_state_t state;
    bool has_tip;              /* a tip was proven this boot (kept across sessions) */
    uint64_t tip_number;
    uint8_t tip_hash[32];
    uint64_t tip_proven_ms;    /* monotonic ms when tip_number was last (re)proven */
    uint64_t checkpoint;       /* the trusted start this session proved from */
    uint32_t sessions;         /* sessions opened this boot */
    int last_status;           /* the last session's final ckb_session status */
} lc_status_t;

/* Start the task (no-op when CONFIG_CKB_LC_ENABLE is off). wifi_up reports
 * whether the station holds an IP; the task waits on it. */
esp_err_t lc_task_start(bool (*wifi_up)(void));

/* Snapshot of the status (thread-safe). */
void lc_task_status(lc_status_t *out);
