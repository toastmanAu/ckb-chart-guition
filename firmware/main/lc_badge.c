#include "lc_badge.h"

#include <stddef.h>

lc_badge_t lc_badge_decide(const lc_badge_input_t *in)
{
    if (in == NULL || !in->enabled) {
        return LC_BADGE_OFF;
    }
    /* Disagreement at two distinct proven heights is evidence against the
     * RPC endpoint, not a tip race; it outranks freshness and connection. */
    if (in->check == LC_CHECK_DIFFER && in->differ_streak >= 2) {
        return LC_BADGE_RPC_MISMATCH;
    }
    /* PROVEN needs a fresh proof AND the RPC agreeing with it. The proof
     * does not lapse when the session drops, only with age. */
    if (in->has_proof && in->proof_age_ms <= LC_PROOF_FRESH_MS &&
        in->check == LC_CHECK_MATCH) {
        return LC_BADGE_PROVEN;
    }
    /* Otherwise (no proof, stale, not compared yet, or a single DIFFER):
     * nothing is vouched for; show the session state. */
    return in->connected ? LC_BADGE_ANCHORING : LC_BADGE_OFFLINE;
}

const char *lc_badge_text(lc_badge_t badge)
{
    switch (badge) {
    case LC_BADGE_PROVEN:
        return "PROVEN";
    case LC_BADGE_ANCHORING:
        return "ANCHORING";
    case LC_BADGE_RPC_MISMATCH:
        return "RPC MISMATCH";
    case LC_BADGE_OFFLINE:
        return "LC OFFLINE";
    case LC_BADGE_OFF:
    default:
        return "";
    }
}
