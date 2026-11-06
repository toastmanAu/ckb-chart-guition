/* The trust badge policy (firmware/main/lc_badge.c). */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../firmware/main/lc_badge.h"

static lc_badge_input_t base(void)
{
    lc_badge_input_t in;
    memset(&in, 0, sizeof(in));
    in.enabled = true;
    return in;
}

static void test_off(void)
{
    assert(lc_badge_decide(NULL) == LC_BADGE_OFF);
    lc_badge_input_t in = base();
    in.enabled = false;
    in.connected = true;
    in.has_proof = true;
    in.check = LC_CHECK_MATCH;
    assert(lc_badge_decide(&in) == LC_BADGE_OFF);
}

static void test_texts(void)
{
    assert(strcmp(lc_badge_text(LC_BADGE_PROVEN), "PROVEN") == 0);
    assert(strcmp(lc_badge_text(LC_BADGE_RPC_MISMATCH), "RPC MISMATCH") == 0);
    assert(strcmp(lc_badge_text(LC_BADGE_ANCHORING), "ANCHORING") == 0);
    assert(strcmp(lc_badge_text(LC_BADGE_OFFLINE), "LC OFFLINE") == 0);
}

static lc_badge_input_t proven(void)
{
    lc_badge_input_t in = base();
    in.connected = true;
    in.has_proof = true;
    in.proof_age_ms = 1000;
    in.check = LC_CHECK_MATCH;
    return in;
}

/* A fresh proof that the RPC agrees with is PROVEN, connected or not:
 * the proof does not expire when the socket drops, only with age. */
static void test_proven(void)
{
    lc_badge_input_t in = proven();
    assert(lc_badge_decide(&in) == LC_BADGE_PROVEN);
    in.connected = false;
    assert(lc_badge_decide(&in) == LC_BADGE_PROVEN);
    in.proof_age_ms = LC_PROOF_FRESH_MS;          /* boundary: still fresh */
    assert(lc_badge_decide(&in) == LC_BADGE_PROVEN);
}

/* Past LC_PROOF_FRESH_MS the proof vouches for nothing current. */
static void test_stale(void)
{
    lc_badge_input_t in = proven();
    in.proof_age_ms = LC_PROOF_FRESH_MS + 1;
    assert(lc_badge_decide(&in) == LC_BADGE_ANCHORING);
    in.connected = false;
    assert(lc_badge_decide(&in) == LC_BADGE_OFFLINE);
}

/* No comparison yet (RPC behind the proof, or lookup failed): the RPC
 * data is not vouched for, so not PROVEN. */
static void test_unknown(void)
{
    lc_badge_input_t in = proven();
    in.check = LC_CHECK_UNKNOWN;
    assert(lc_badge_decide(&in) == LC_BADGE_ANCHORING);
    in.connected = false;
    assert(lc_badge_decide(&in) == LC_BADGE_OFFLINE);
}

/* One disagreement can be a tip race (one-block reorg seen differently by
 * the node and the RPC): withhold PROVEN but do not raise the alarm. */
static void test_single_differ(void)
{
    lc_badge_input_t in = proven();
    in.check = LC_CHECK_DIFFER;
    in.differ_streak = 1;
    assert(lc_badge_decide(&in) == LC_BADGE_ANCHORING);
}

/* Disagreement at two distinct proven heights is evidence against the RPC
 * endpoint and outranks everything: stale proof, no session. */
static void test_mismatch(void)
{
    lc_badge_input_t in = proven();
    in.check = LC_CHECK_DIFFER;
    in.differ_streak = 2;
    assert(lc_badge_decide(&in) == LC_BADGE_RPC_MISMATCH);
    in.connected = false;
    in.proof_age_ms = LC_PROOF_FRESH_MS * 10;
    assert(lc_badge_decide(&in) == LC_BADGE_RPC_MISMATCH);
    in.enabled = false;                           /* but OFF still wins */
    assert(lc_badge_decide(&in) == LC_BADGE_OFF);
}

/* No proof at all: the session state is all there is to show. */
static void test_no_proof(void)
{
    lc_badge_input_t in = base();
    assert(lc_badge_decide(&in) == LC_BADGE_OFFLINE);
    in.connected = true;
    assert(lc_badge_decide(&in) == LC_BADGE_ANCHORING);
    in.check = LC_CHECK_MATCH;    /* a MATCH without a proof is meaningless */
    assert(lc_badge_decide(&in) == LC_BADGE_ANCHORING);
}

int main(void)
{
    test_off();
    test_texts();
    test_proven();
    test_stale();
    test_unknown();
    test_single_differ();
    test_mismatch();
    test_no_proof();
    puts("test_lc_badge: ok");
    return 0;
}
