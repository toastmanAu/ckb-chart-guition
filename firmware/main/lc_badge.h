#pragma once
/*
 * The dashboard's trust badge: what the on-device light client says about
 * the chain the JSON-RPC endpoint is showing. Pure (no ESP-IDF), so the
 * policy can be tested on the host.
 *
 * Facts it is decided from:
 *   - the light client's own state (lc_task.c): is it running, connected,
 *     and has it proven a tip (proof of work + MMR + FlyClient, RFC 0044)?
 *   - how old that proof is;
 *   - the cross-check (network.c): does the RPC endpoint report the SAME
 *     block hash at the proven tip's height?
 */
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    LC_BADGE_OFF = 0,       /* light client disabled (Kconfig) or demo mode */
    LC_BADGE_OFFLINE,       /* no session and no usable proof */
    LC_BADGE_ANCHORING,     /* connected, proving from the checkpoint */
    LC_BADGE_PROVEN,        /* RPC agrees with a recent proven tip */
    LC_BADGE_RPC_MISMATCH,  /* RPC disagrees with the proven chain */
} lc_badge_t;

typedef enum {
    LC_CHECK_UNKNOWN = 0,   /* not compared yet, or RPC could not answer for that height */
    LC_CHECK_MATCH,         /* RPC hash at the proven height == the proven hash */
    LC_CHECK_DIFFER,        /* RPC hash at the proven height != the proven hash */
} lc_check_t;

typedef struct {
    bool enabled;            /* light client compiled in and not in demo mode */
    bool connected;          /* a P2P session is open right now */
    bool has_proof;          /* a tip has been proven (this boot) */
    uint64_t proof_age_ms;   /* since that proof (valid when has_proof) */
    lc_check_t check;        /* latest comparison against RPC */
    uint32_t differ_streak;  /* consecutive comparisons that came back DIFFER */
} lc_badge_input_t;

/* A proof older than this no longer vouches for the RPC tip: ~30 blocks. */
#define LC_PROOF_FRESH_MS (5u * 60u * 1000u)

lc_badge_t lc_badge_decide(const lc_badge_input_t *in);
const char *lc_badge_text(lc_badge_t badge);
