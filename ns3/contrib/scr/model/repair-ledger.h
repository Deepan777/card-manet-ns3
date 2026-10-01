/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Service-Confirmed Repair (SCR) — per-pair repair ledger (E3, E4, E5).
 *
 * Traceability: M06 (episode state), M07 (fast allowance / TTL escalation),
 *               M08 (probe bucket).
 * Validating fixtures: T1, T3, T4, T7, T8.
 *
 * State location: source routing module. One ledger per source-destination pair.
 */

#ifndef SCR_REPAIR_LEDGER_H
#define SCR_REPAIR_LEDGER_H

#include "ns3/ipv4-address.h"
#include "ns3/nstime.h"

#include <cstdint>
#include <string>

namespace ns3
{
namespace scr
{


/**
 * Registered algorithm modes (specification section 1 and the ablation table).
 *
 * The mode is emitted from C++ at run time, never assigned by an analysis-side
 * filename, so a mislabelled run cannot silently become a different algorithm.
 */
enum AlgorithmMode : uint8_t
{
    ALGO_SCR = 0,             //!< proposed method
    ALGO_SCR2,                //!< SCR-2: evidence brakes and narrows, never permits
    ALGO_RREP_RESET,          //!< RREP completing a discovery renews B and j
    ALGO_TIME_BUCKET,         //!< per-pair token bucket, no feedback renewal
    ALGO_PERSISTENT_BACKOFF,  //!< min(0.5*2^k,16) s, k resets only on confirmation
    ALGO_AODV_FB,             //!< native AODV discovery plus feedback apps
    ALGO_AODV_STOCK,          //!< native AODV, no feedback
    // ---- ablations ----
    ALGO_RESET_ESCALATION,    //!< keep debt, reset j on RREP, TTL uses min(j,B-1)
    ALGO_NO_PROBE,            //!< rho = 0
    ALGO_ONE_BLOCK,           //!< K = 1
    ALGO_NO_DEADLINE,         //!< receiver ignores D within the closure window
    ALGO_NO_NODE_CAP,         //!< remove only the G limiter
    // ---- CARD: cause-aware rationed discovery (R053, fork_rev 6) ----
    // Appended so that every earlier mode keeps its numeric value.
    ALGO_CARD,                //!< renewal on acquisition; after a topology change: exempt + reach hint
    ALGO_CARD_NO_CAUSE,       //!< reach hint on every recovery, never exempt (cause-blind hint)
    ALGO_CARD_NO_REACH,       //!< exempt after a topology change, no reach hint
    ALGO_CARD_EXEMPT_ALL,     //!< every break treated as a topology change (exempt + hint)
    ALGO_CARD_NO_EXEMPT,      //!< reach hint after a topology change only, never exempt (R054)
    // ---- published baselines (R057, fork_rev 8) ----
    ALGO_CLAF_AODV,           //!< CLAF-AODV, Safari et al., IEEE Access 11 (2023): fuzzy RREQ forwarding
    // ---- published baselines (R058, fork_rev 9) ----
    ALGO_TAAODV,              //!< TAAODV, Li et al., IEEE T-ITS 26 (2025): TOPSIS-selected RREQ forwarders, pheromone path choice
    // ---- preregistered composition test (R060, fork_rev 10) ----
    ALGO_CARD_CLAF,           //!< CARD at the sources, CLAF-AODV's relay filter at the relays
};

/// True for the CARD family.
bool IsCardMode(AlgorithmMode m);
/// True for the modes whose relays use CLAF-AODV's forwarding rule (R057, R060).
bool IsClafMode(AlgorithmMode m);
/// CARD modes that report and read the break cause.
bool CardUsesCause(AlgorithmMode m);
/// CARD modes whose recoveries after a topology change are not charged.
bool CardUsesExemption(AlgorithmMode m);
/// CARD modes that apply the reach hint to EVERY recovery, whatever the cause.
bool CardHintAlways(AlgorithmMode m);
/// CARD modes that apply the reach hint only after a topology change.
bool CardHintOnTopology(AlgorithmMode m);

const char* AlgorithmModeName(AlgorithmMode m);
/// Parse a registered mode name; returns false if unknown (never guesses).
bool ParseAlgorithmMode(const std::string& s, AlgorithmMode& out);

/// E3 episode modes.
enum LedgerMode : uint8_t
{
    LEDGER_IDLE = 0,
    LEDGER_RECOVERING,
    LEDGER_SERVING,
};

const char* LedgerModeName(LedgerMode m);

/// Why an origination was or was not admitted. Every decision is logged.
enum AdmitDecision : uint8_t
{
    ADMIT_FAST = 0,        //!< charged against the fast allowance B
    ADMIT_PROBE,           //!< charged against the probe bucket P
    DENY_IN_FLIGHT,        //!< a request is already outstanding for this pair
    DENY_NO_ALLOWANCE,     //!< B exhausted and no probe token available
    DENY_NOT_ELIGIBLE,     //!< next_eligible time not reached
    DENY_NO_DEMAND,        //!< no live application demand
    ADMIT_EXEMPT,          //!< CARD: topology change reported, not charged to B or P
};

const char* AdmitDecisionName(AdmitDecision d);

/**
 * @ingroup scr
 * @brief Per source-destination pair episode, allowance and probe state.
 *
 * Deliberate design point: nothing in this class is reset by RREP, by routing
 * table deletion, or by application restart. Only an E2 confirmation while a
 * usable route exists renews allowance (E3).
 */
class RepairLedger
{
  public:
    RepairLedger();

    // ---- configuration (E4/E5 pinned defaults) ----
    /**
     * Set B. This is a CONFIGURATION-time setter: it also resets the
     * remaining allowance, because leaving B_remaining at its constructor
     * default would silently grant a different budget than the configured
     * one - which would corrupt every B-varying ablation.
     */
    void SetB(uint32_t b)
    {
        m_B = b;
        m_bRemaining = b;
    }
    void SetRho(double rho) { m_rho = rho; }
    void SetAlgorithm(AlgorithmMode a) { m_algo = a; }
    AlgorithmMode GetAlgorithm() const { return m_algo; }
    /// TIME-BUCKET refill rate r (requests/s).
    void SetBucketRefill(double r) { m_bucketRefill = r; }
    /// PERSISTENT-BACKOFF origination index k.
    uint32_t GetBackoffIndex() const { return m_k; }
    void SetTtlMax(uint16_t v) { m_ttlMax = v; }
    void SetNodeTraversalTime(Time v) { m_nodeTraversalTime = v; }
    void SetTimeoutBuffer(uint32_t v) { m_timeoutBuffer = v; }
    void SetNetTraversalTime(Time v) { m_netTraversalTime = v; }

    uint32_t GetB() const { return m_B; }

    // ---- SCR-2 (R031) -----------------------------------------------------
    //
    // SCR as specified uses confirmed service as a PERMIT: allowance is renewed
    // only by an E2 confirmation. Measured on C1, that is self-defeating. The
    // evidence needed to authorise repair is produced by the delivery that the
    // repair exists to restore, so a pair that falls behind cannot earn its way
    // back: SCR obtained 7.8 confirmations per run where RREP-RESET obtained
    // 68.3, and delivered 0.077 of packets on time against stock AODV's 0.469.
    //
    // SCR-2 keeps the same evidence -- deadline-qualified, end-to-end, carried
    // in receipts -- and reverses its role:
    //
    //   fresh confirmation  -> WAIT briefly, and search NARROWLY first.
    //                          The path worked seconds ago; a break is probably
    //                          transient and the destination is probably close.
    //   no confirmation     -> search IMMEDIATELY and escalate reach.
    //
    // Failure therefore grants freedom rather than removing it, so the deadlock
    // cannot occur by construction rather than by parameter choice.
    void SetScr2Holdoff(Time v) { m_scr2Holdoff = v; }
    void SetScr2FreshWindow(Time v) { m_scr2FreshWindow = v; }
    Time GetScr2Holdoff() const { return m_scr2Holdoff; }
    Time GetScr2FreshWindow() const { return m_scr2FreshWindow; }
    /// True when an E2 confirmation arrived recently enough to be acted on.
    bool ServiceIsFresh(Time now) const;
    /// Last time an E2 confirmation was accepted, renewing or not.
    Time GetLastConfirmTime() const { return m_lastConfirm; }
    /// R032 diagnosis: true when the outstanding request was ANSWERED by a
    /// route installation since the last send. Observation only -- nothing in
    /// the admission logic reads it.
    bool ReplyReceivedSinceSend() const { return m_replyReceived; }
    double GetRho() const { return m_rho; }

    /// E5 configuration validation: 1<=B<=8, rho>=0.
    bool ValidateConfiguration(std::string& why, bool allowZeroRho) const;

    // ---- state accessors ----
    LedgerMode GetMode() const { return m_mode; }
    uint64_t GetEpisodeId() const { return m_episodeId; }
    Time GetEpisodeStart() const { return m_episodeStart; }
    uint32_t GetRemainingFast() const { return m_bRemaining; }
    uint32_t GetEscalationIndex() const { return m_j; }
    bool IsInFlight() const { return m_inFlight; }
    Time GetNextEligible() const { return m_nextEligible; }
    double GetProbeTokens(Time now) const;

    uint64_t GetFastSpent() const { return m_fastSpent; }
    uint64_t GetProbesSpent() const { return m_probesSpent; }
    uint64_t GetRenewals() const { return m_renewals; }

    // ---- E3 episode transitions ----

    /**
     * Called when discovery is actually needed: live demand exists and no usable
     * route exists. Opens a new episode if not already RECOVERING.
     */
    void OnDiscoveryNeeded(Time now);

    /**
     * RREP installed a usable route. E3: this cancels unsent discovery and clears
     * in_flight, but does NOT close the episode and does NOT restore allowance.
     */
    void OnRouteInstalled(Time now);
    /// As above, also recording the installed route's hop count, which CARD
    /// uses as the starting reach of the next search (R053).
    void OnRouteInstalled(Time now, uint16_t hops);

    // ---- CARD (R053) ------------------------------------------------------
    //
    // A rationed controller treats every break alike. Measured across the three
    // families, rationing paid where a break did not change the topology (the
    // bridge outage, congestion losses on static links) and cost delivery where
    // it did (mobility), because there the searches it withheld or started too
    // narrow were the ones recovery needed. CARD lets the network say which
    // kind of break occurred. The node that detects the break classifies it
    // from the neighbour's received signal, the cause travels in the RERR, and
    // the source then either rations the recovery or exempts it.
    //
    // The protocol tells the ledger, before each evaluation, whether the
    // current recovery follows a reported topology change; the ledger does not
    // know where the cause came from. R054: the cause now decides reach as
    // well as admission.
    void SetCardTopology(bool t) { m_cardTopology = t; }
    bool GetCardTopology() const { return m_cardTopology; }
    uint16_t GetHopHint() const { return m_hopHint; }
    uint64_t GetExemptSpent() const { return m_exemptSpent; }

    /**
     * Route invalidated. E3: episode id, remaining allowance and j are unchanged.
     */
    void OnRouteInvalidated(Time now);

    /**
     * E2 confirmation held while a usable route exists. Transitions RECOVERING to
     * SERVING, sets B_remaining=B and j=0 exactly once.
     * @return true if a renewal actually occurred.
     */
    bool OnServiceConfirmed(Time now, bool routeExists);

    /// Session stop: cancels pending work, never renews credits (tombstone).
    void OnSessionStop(Time now);

    // ---- E4/E5 admission ----

    /**
     * Decide whether an origination may be admitted now. This does NOT debit;
     * debiting happens only at successful socket handoff (E6).
     */
    AdmitDecision Evaluate(Time now, bool hasDemand) const;

    /**
     * TTL for the next fast attempt at the current escalation index.
     * TTL(j) = min(TTLmax, 2^(j+1)) for j < B-1; TTL(B-1) = TTLmax.
     * A probe always uses TTLmax.
     */
    uint16_t TtlForNextAttempt() const;

    /**
     * Reply timeout for a request sent with the given TTL.
     * T_wait(h) = 2*NodeTraversalTime*(h+TimeoutBuffer) for h < TTLmax;
     * NetTraversalTime at full diameter.
     */
    Time ReplyTimeout(uint16_t ttl) const;

    /**
     * Commit an origination that reached successful socket handoff.
     * E4: B_remaining -= 1 and j += 1 for a fast request; a probe consumes one
     * probe token and does not change j. Never called on denial or on failed
     * handoff.
     */
    void CommitSend(Time now, AdmitDecision kind, Time replyTimeout);

    /// The outstanding request timed out without a reply.
    void OnReplyTimeout(Time now);

    /**
     * Clear a stale in_flight marker whose reply window has already closed.
     *
     * in_flight is set at CommitSend and normally cleared by OnRouteInstalled
     * or OnReplyTimeout. Upstream AODV, however, deletes the routing entry and
     * stops its request timer after RreqRetries, so under a long partition the
     * timeout callback may never fire again. Without this check the pair would
     * stay in_flight forever and could never originate again - a PERMANENT
     * ADMISSION LOCKOUT, which E9 explicitly forbids.
     *
     * next_eligible is set to (send time + reply timeout) at CommitSend, so
     * once now >= next_eligible the reply window has demonstrably closed.
     * @return true if a stale marker was cleared.
     */
    bool ExpireIfDue(Time now);

  private:
    void RefillProbe(Time now);

    // configuration
    uint32_t m_B;
    double m_rho;
    uint16_t m_ttlMax;
    Time m_nodeTraversalTime;
    uint32_t m_timeoutBuffer;
    Time m_netTraversalTime;

    // E3 episode state
    LedgerMode m_mode;
    uint64_t m_episodeId;
    Time m_episodeStart;
    uint32_t m_bRemaining;
    uint32_t m_j;
    bool m_inFlight;
    Time m_nextEligible;

    // E5 probe bucket
    double m_probeTokens;
    Time m_probeLastUpdate;

    // counters
    uint64_t m_fastSpent;
    uint64_t m_probesSpent;
    uint64_t m_renewals;

    // ---- baseline-specific state ----
    AlgorithmMode m_algo;
    double m_bucketRefill;   //!< TIME-BUCKET r
    double m_bucketTokens;   //!< TIME-BUCKET per-pair tokens
    Time m_bucketLastUpdate;
    uint32_t m_k;            //!< PERSISTENT-BACKOFF origination index
    uint32_t m_originations; //!< TIME-BUCKET TTL cycle counter

    // ---- SCR-2 state (R031) ----
    /// When an E2 confirmation last arrived. Recorded whether or not it renewed
    /// anything, because SCR-2 reads it as evidence of recent service rather
    /// than as a permit to spend.
    Time m_lastConfirm;
    /// R032 diagnosis flag; see ReplyReceivedSinceSend().
    bool m_replyReceived;
    /// R032: PERSISTENT-BACKOFF's registered inter-origination floor, kept apart
    /// from the reply-timeout deadline so that a reply can end the wait without
    /// also erasing the backoff schedule that arm is defined by.
    Time m_backoffEligible;
    /// How long to wait before searching when service was recently confirmed.
    Time m_scr2Holdoff;
    /// How recent a confirmation must be to count as evidence of live service.
    Time m_scr2FreshWindow;

    // ---- CARD state (R053) ----
    uint16_t RingTtl(uint32_t j) const;
    bool CardExempt() const;
    bool CardHintApplies() const;
    bool m_cardTopology;     //!< current recovery follows a reported topology change
    uint16_t m_hopHint;      //!< hop count of the last installed route, 0 if unknown
    uint64_t m_exemptSpent;  //!< originations admitted under ADMIT_EXEMPT
};

} // namespace scr
} // namespace ns3

#endif /* SCR_REPAIR_LEDGER_H */
