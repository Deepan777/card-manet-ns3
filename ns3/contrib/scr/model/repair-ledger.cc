/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * SCR — per-pair repair ledger. Traceability M06, M07, M08 (E3, E4, E5).
 */

#include "repair-ledger.h"

#include "ns3/log.h"
#include "ns3/simulator.h"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <string>

namespace ns3
{
namespace scr
{

NS_LOG_COMPONENT_DEFINE("ScrRepairLedger");


const char*
AlgorithmModeName(AlgorithmMode m)
{
    switch (m)
    {
    case ALGO_SCR: return "SCR";
    case ALGO_SCR2: return "SCR2";
    case ALGO_RREP_RESET: return "RREP-RESET";
    case ALGO_TIME_BUCKET: return "TIME-BUCKET";
    case ALGO_PERSISTENT_BACKOFF: return "PERSISTENT-BACKOFF";
    case ALGO_AODV_FB: return "AODV-FB";
    case ALGO_AODV_STOCK: return "AODV-STOCK";
    case ALGO_RESET_ESCALATION: return "RESET-ESCALATION";
    case ALGO_NO_PROBE: return "NO-PROBE";
    case ALGO_ONE_BLOCK: return "ONE-BLOCK";
    case ALGO_NO_DEADLINE: return "NO-DEADLINE";
    case ALGO_NO_NODE_CAP: return "NO-NODE-CAP";
    case ALGO_CARD: return "CARD";
    case ALGO_CARD_NO_CAUSE: return "CARD-NO-CAUSE";
    case ALGO_CARD_NO_REACH: return "CARD-NO-REACH";
    case ALGO_CARD_EXEMPT_ALL: return "CARD-EXEMPT-ALL";
    case ALGO_CARD_NO_EXEMPT: return "CARD-NO-EXEMPT";
    case ALGO_CLAF_AODV: return "CLAF-AODV";
    case ALGO_TAAODV: return "TAAODV";
    case ALGO_CARD_CLAF: return "CARD-CLAF";
    }
    return "UNKNOWN";
}

bool
IsCardMode(AlgorithmMode m)
{
    return m == ALGO_CARD || m == ALGO_CARD_NO_CAUSE || m == ALGO_CARD_NO_REACH ||
           m == ALGO_CARD_EXEMPT_ALL || m == ALGO_CARD_NO_EXEMPT || m == ALGO_CARD_CLAF;
}

bool
IsClafMode(AlgorithmMode m)
{
    return m == ALGO_CLAF_AODV || m == ALGO_CARD_CLAF;
}

bool
CardUsesCause(AlgorithmMode m)
{
    return m == ALGO_CARD || m == ALGO_CARD_NO_REACH || m == ALGO_CARD_NO_EXEMPT ||
           m == ALGO_CARD_CLAF;
}

bool
CardUsesExemption(AlgorithmMode m)
{
    return m == ALGO_CARD || m == ALGO_CARD_NO_REACH || m == ALGO_CARD_EXEMPT_ALL ||
           m == ALGO_CARD_CLAF;
}

bool
CardHintAlways(AlgorithmMode m)
{
    // The two cause-blind variants: CARD-NO-CAUSE by definition, and
    // CARD-EXEMPT-ALL, which treats every break as a topology change. Both keep
    // exactly their rev-6 behaviour (R054).
    return m == ALGO_CARD_NO_CAUSE || m == ALGO_CARD_EXEMPT_ALL;
}

bool
CardHintOnTopology(AlgorithmMode m)
{
    return m == ALGO_CARD || m == ALGO_CARD_NO_EXEMPT || m == ALGO_CARD_CLAF;
}

bool
ParseAlgorithmMode(const std::string& s, AlgorithmMode& out)
{
    // Exhaustive, exact-match table. An unrecognised name is REJECTED rather
    // than defaulted, so a typo can never silently run a different algorithm
    // under the requested label.
    static const struct
    {
        const char* name;
        AlgorithmMode mode;
    } kTable[] = {
        {"SCR", ALGO_SCR},
        {"SCR2", ALGO_SCR2},
        {"RREP-RESET", ALGO_RREP_RESET},
        {"TIME-BUCKET", ALGO_TIME_BUCKET},
        {"PERSISTENT-BACKOFF", ALGO_PERSISTENT_BACKOFF},
        {"AODV-FB", ALGO_AODV_FB},
        {"AODV-STOCK", ALGO_AODV_STOCK},
        {"RESET-ESCALATION", ALGO_RESET_ESCALATION},
        {"NO-PROBE", ALGO_NO_PROBE},
        {"ONE-BLOCK", ALGO_ONE_BLOCK},
        {"NO-DEADLINE", ALGO_NO_DEADLINE},
        {"NO-NODE-CAP", ALGO_NO_NODE_CAP},
        {"CARD", ALGO_CARD},
        {"CARD-NO-CAUSE", ALGO_CARD_NO_CAUSE},
        {"CARD-NO-REACH", ALGO_CARD_NO_REACH},
        {"CARD-EXEMPT-ALL", ALGO_CARD_EXEMPT_ALL},
        {"CARD-NO-EXEMPT", ALGO_CARD_NO_EXEMPT},
        {"CLAF-AODV", ALGO_CLAF_AODV},
        {"TAAODV", ALGO_TAAODV},
        {"CARD-CLAF", ALGO_CARD_CLAF},
    };
    for (const auto& e : kTable)
    {
        if (s == e.name)
        {
            out = e.mode;
            return true;
        }
    }
    return false;
}

const char*
LedgerModeName(LedgerMode m)
{
    switch (m)
    {
    case LEDGER_IDLE: return "IDLE";
    case LEDGER_RECOVERING: return "RECOVERING";
    case LEDGER_SERVING: return "SERVING";
    }
    return "UNKNOWN";
}

const char*
AdmitDecisionName(AdmitDecision d)
{
    switch (d)
    {
    case ADMIT_FAST: return "admit_fast";
    case ADMIT_PROBE: return "admit_probe";
    case DENY_IN_FLIGHT: return "deny_in_flight";
    case DENY_NO_ALLOWANCE: return "deny_no_allowance";
    case DENY_NOT_ELIGIBLE: return "deny_not_eligible";
    case DENY_NO_DEMAND: return "deny_no_demand";
    case ADMIT_EXEMPT: return "admit_exempt";
    }
    return "unknown";
}

RepairLedger::RepairLedger()
    : m_B(4),
      m_rho(0.2),
      m_ttlMax(35),
      m_nodeTraversalTime(MilliSeconds(40)),
      m_timeoutBuffer(2),
      m_netTraversalTime(MilliSeconds(2800)),
      m_mode(LEDGER_IDLE),
      m_episodeId(0),
      m_episodeStart(Seconds(0)),
      m_bRemaining(4),
      m_j(0),
      m_inFlight(false),
      m_nextEligible(Seconds(0)),
      // E5: the probe bucket starts EMPTY. Starting it full would grant a free
      // extra origination at t=0 that the specification does not allow.
      m_probeTokens(0.0),
      m_probeLastUpdate(Seconds(0)),
      m_fastSpent(0),
      m_probesSpent(0),
      m_renewals(0),
      m_algo(ALGO_SCR),
      m_bucketRefill(0.5),
      m_bucketTokens(4.0),
      m_bucketLastUpdate(Seconds(0)),
      m_k(0),
      m_originations(0),
      m_lastConfirm(Time(0)),
      m_replyReceived(false),
      m_backoffEligible(Time(0)),
      m_scr2Holdoff(Seconds(1.0)),
      m_scr2FreshWindow(Seconds(3.0)),
      m_cardTopology(false),
      m_hopHint(0),
      m_exemptSpent(0)
{
    m_bRemaining = m_B;
}

bool
RepairLedger::ValidateConfiguration(std::string& why, bool allowZeroRho) const
{
    std::ostringstream oss;
    bool ok = true;
    if (m_B < 1 || m_B > 8)
    {
        oss << "B=" << m_B << " outside [1,8]; ";
        ok = false;
    }
    if (m_rho < 0.0)
    {
        oss << "rho must be >= 0; ";
        ok = false;
    }
    if (m_rho == 0.0 && !allowZeroRho)
    {
        // E5: rho=0 is admitted ONLY for the named NO-PROBE ablation. Allowing
        // it silently elsewhere would disable probing without it appearing as a
        // declared experimental condition.
        oss << "rho=0 is permitted only for the NO-PROBE ablation; ";
        ok = false;
    }
    why = oss.str();
    return ok;
}

void
RepairLedger::RefillProbe(Time now)
{
    if (now <= m_probeLastUpdate)
    {
        return;
    }
    const double dt = (now - m_probeLastUpdate).GetSeconds();
    // P(t) = min(1, P(t_last) + rho*(t - t_last))
    m_probeTokens = std::min(1.0, m_probeTokens + m_rho * dt);
    m_probeLastUpdate = now;
}

double
RepairLedger::GetProbeTokens(Time now) const
{
    if (now <= m_probeLastUpdate)
    {
        return m_probeTokens;
    }
    const double dt = (now - m_probeLastUpdate).GetSeconds();
    return std::min(1.0, m_probeTokens + m_rho * dt);
}

void
RepairLedger::OnDiscoveryNeeded(Time now)
{
    RefillProbe(now);
    if (m_mode != LEDGER_RECOVERING)
    {
        // E3: an episode opens on the first actual need for discovery while live
        // demand exists and no usable route exists. IDLE or SERVING -> RECOVERING.
        ++m_episodeId;
        m_episodeStart = now;
        m_mode = LEDGER_RECOVERING;
        if (m_algo == ALGO_SCR2 && ServiceIsFresh(now))
        {
            // R031 THE BRAKE. Service was confirmed within the freshness window,
            // so this break is probably transient and flooding immediately is
            // probably waste. Wait first.
            //
            // It is applied HERE, once, on the transition into RECOVERING -- not
            // on every evaluation -- so a pair that stays broken is delayed once
            // and then searches freely. Re-arming it per attempt would rebuild
            // the lockout this design exists to remove.
            m_nextEligible = std::max(m_nextEligible, now + m_scr2Holdoff);
        }
        NS_LOG_INFO("episode " << m_episodeId << " opened at " << now.As(Time::S)
                               << " with B_remaining=" << m_bRemaining << " j=" << m_j);
    }
    // Already RECOVERING: episode id, allowance and j are all unchanged (E3).
}

void
RepairLedger::OnRouteInstalled(Time now, uint16_t hops)
{
    if (IsCardMode(m_algo))
    {
        // R053: remember how far away the destination was. A later break is
        // searched from this distance rather than from the smallest ring.
        m_hopHint = hops;
    }
    OnRouteInstalled(now);
}

void
RepairLedger::OnRouteInstalled(Time now)
{
    RefillProbe(now);
    // Baseline controls diverge here, and this divergence IS the mechanism
    // being tested: whether a route alone may renew credit, or whether
    // confirmed service is required.
    const bool completedOutstanding = m_inFlight;
    m_inFlight = false;
    if (completedOutstanding)
    {
        m_replyReceived = true;
        // R032 THE ANSWERED REQUEST ENDS THE WAIT.
        //
        // CommitSend sets next_eligible = send time + T_wait, so that a pair
        // waits for its reply. E4 specifies exactly that: "at most one request
        // waiting for a reply", with T_wait bounding the wait. It specifies no
        // cooldown after the reply arrives -- yet until this change the deadline
        // simply ran on. A reply 50 ms after a full-diameter send left the pair
        // ineligible for the remaining 2.75 s, and if the new route broke inside
        // that window, the pair could not search at all.
        //
        // Measured with the R032 denial breakdown (C1 healthy, seed 201): every
        // DENY_NOT_ELIGIBLE in every SCR-family arm occurred AFTER the reply had
        // arrived -- 27,830 of SCR-2's 31,945 denials and 16,043 of SCR's. It was
        // the dominant throttle in the family, and it was not registered
        // anywhere. D002 rejected an inherited exponential timer for precisely
        // this reason: "a second, unregistered throttle".
        //
        // PERSISTENT-BACKOFF is the one exception. Its registered schedule is an
        // interval BETWEEN originations, min(0.5*2^k,16) s, so a reply must end
        // the reply wait without erasing that floor.
        m_nextEligible = (m_algo == ALGO_PERSISTENT_BACKOFF) ? std::max(now, m_backoffEligible)
                                                             : now;
    }

    if (IsCardMode(m_algo) && completedOutstanding)
    {
        // CARD keeps RREP-RESET's renewal evidence unchanged: route acquisition
        // renews the allowance. What CARD adds acts on the NEXT break -- whether
        // its recovery is rationed at all, and how far its first search reaches.
        m_bRemaining = m_B;
        m_j = 0;
        m_cardTopology = false;
        ++m_renewals;
        NS_LOG_INFO("CARD: allowance renewed by route installation, hop hint " << m_hopHint);
        return;
    }
    if (m_algo == ALGO_RREP_RESET && completedOutstanding)
    {
        // RREP-RESET: only a RREP that completes an OUTSTANDING source
        // discovery renews. Duplicate, gratuitous or unsolicited replies
        // cannot manufacture unlimited credit - that would be a deliberately
        // pathological baseline, which the specification forbids.
        m_bRemaining = m_B;
        m_j = 0;
        ++m_renewals;
        NS_LOG_INFO("RREP-RESET: allowance renewed by route installation");
        return;
    }
    if (m_algo == ALGO_RESET_ESCALATION && completedOutstanding)
    {
        // RESET-ESCALATION ablation: keep the debt, reset only j.
        m_j = 0;
        NS_LOG_INFO("RESET-ESCALATION: j reset, debt retained");
        return;
    }

    // SCR and the remaining modes: E3 - RREP does NOT close the episode and
    // does NOT restore allowance.
    NS_LOG_INFO("route installed; episode " << m_episodeId << " stays " << LedgerModeName(m_mode)
                                            << " B_remaining=" << m_bRemaining << " j=" << m_j);
}

void
RepairLedger::OnRouteInvalidated(Time now)
{
    RefillProbe(now);
    // E3: route invalidation within RECOVERING leaves episode id, remaining
    // allowance and j unchanged. Routing-table deletions do not affect the ledger.
    NS_LOG_INFO("route invalidated; ledger unchanged (episode " << m_episodeId << ")");
}

bool
RepairLedger::OnServiceConfirmed(Time now, bool routeExists)
{
    RefillProbe(now);

    // R031. Record the arrival whether or not it renews anything. SCR reads
    // this event as a permit and so only cares when it can spend it; SCR-2
    // reads it as evidence that service was recently good, which is true
    // regardless of the ledger's mode or whether a route happens to exist.
    m_lastConfirm = now;
    if (m_algo == ALGO_SCR2)
    {
        // Fresh evidence means the destination was reachable moments ago, so
        // the next search should start narrow again.
        m_j = 0;
    }

    if (!routeExists)
    {
        // E3: if confirmation arrives while no route exists, retain the evidence
        // but do not renew. Re-evaluation happens after an ordinary RREP if the
        // evidence is still fresh.
        NS_LOG_INFO("confirmation held: no usable route, no renewal");
        return false;
    }
    if (m_mode != LEDGER_RECOVERING)
    {
        // E3: feedback in SERVING does not grant additional stored allowance.
        NS_LOG_INFO("confirmation in " << LedgerModeName(m_mode) << ": no additional allowance");
        return false;
    }

    // Exactly once per episode: RECOVERING -> SERVING, B_remaining=B, j=0.
    m_mode = LEDGER_SERVING;
    m_bRemaining = m_B;
    m_j = 0;
    m_k = 0; // PERSISTENT-BACKOFF: k resets only here
    ++m_renewals;
    NS_LOG_INFO("service confirmed: episode " << m_episodeId << " -> SERVING, allowance renewed");
    return true;
}

void
RepairLedger::OnSessionStop(Time now)
{
    RefillProbe(now);
    // E3: session stop cancels pending work but does not renew credits. The
    // ledger is retained as a tombstone for the whole run so that restarting the
    // application cannot reset accumulated debt.
    m_inFlight = false;
    NS_LOG_INFO("session stopped; ledger retained as tombstone (no credit renewal)");
}

bool
RepairLedger::ServiceIsFresh(Time now) const
{
    // Time(0) means "never confirmed", which is not fresh. Distinguishing this
    // matters at t=0: without it every pair would start out looking as though
    // it had just been served, and SCR-2 would begin by braking.
    if (m_lastConfirm <= Time(0))
    {
        return false;
    }
    return (now - m_lastConfirm) <= m_scr2FreshWindow;
}

AdmitDecision
RepairLedger::Evaluate(Time now, bool hasDemand) const
{
    if (!hasDemand)
    {
        return DENY_NO_DEMAND;
    }
    if (m_inFlight)
    {
        // E4: one pair has at most one request waiting for a reply.
        return DENY_IN_FLIGHT;
    }
    if (now < m_nextEligible)
    {
        return DENY_NOT_ELIGIBLE;
    }
    if (m_algo == ALGO_TIME_BUCKET)
    {
        // TIME-BUCKET: a single per-pair token bucket of capacity B refilled at
        // r. No feedback-based renewal exists in this baseline, so there is no
        // fast/probe split - that split is precisely what SCR adds and what this
        // control isolates.
        double tok = m_bucketTokens;
        if (now > m_bucketLastUpdate)
        {
            tok = std::min<double>(m_B,
                                   tok + m_bucketRefill * (now - m_bucketLastUpdate).GetSeconds());
        }
        return (tok >= 1.0) ? ADMIT_FAST : DENY_NO_ALLOWANCE;
    }

    if (CardExempt())
    {
        // R053. The break that started this recovery changed the topology, so
        // the old route will not come back by itself and a search is the only
        // way to recover. Such searches are not charged to the allowance. The
        // in-flight, eligibility and demand checks above still apply, as do the
        // node limiter G and the RREQ rate limit downstream, so an exempt pair
        // searches at most once per reply timeout.
        return ADMIT_EXEMPT;
    }

    if (m_algo == ALGO_SCR2)
    {
        // R031. SCR-2 never withholds permission. The in-flight, demand and
        // eligibility checks above still apply -- and the eligibility check is
        // where the brake lands, set by OnDiscoveryNeeded -- but there is no
        // allowance to exhaust, so a pair can never be locked out by the
        // absence of the very evidence it is trying to earn.
        return ADMIT_FAST;
    }

    if (m_bRemaining > 0)
    {
        return ADMIT_FAST;
    }
    if (m_algo == ALGO_NO_PROBE)
    {
        // NO-PROBE ablation: rho = 0 and P = 0, so exhaustion is terminal until
        // service is confirmed. This measures the lockout cost of a finite budget.
        return DENY_NO_ALLOWANCE;
    }
    // E5: when B_remaining=0, one probe requires P >= 1.
    if (GetProbeTokens(now) >= 1.0)
    {
        return ADMIT_PROBE;
    }
    return DENY_NO_ALLOWANCE;
}

uint16_t
RepairLedger::RingTtl(uint32_t j) const
{
    // The registered ring: TTL(j) = min(TTLmax, 2^(j+1)) for j < B-1, else TTLmax.
    if (m_B == 0 || j >= m_B - 1)
    {
        return m_ttlMax;
    }
    const uint32_t shift = j + 1;
    const uint32_t val = (shift >= 16) ? m_ttlMax : (1u << shift);
    return static_cast<uint16_t>(std::min<uint32_t>(m_ttlMax, val));
}

bool
RepairLedger::CardExempt() const
{
    return IsCardMode(m_algo) && CardUsesExemption(m_algo) && m_cardTopology;
}

bool
RepairLedger::CardHintApplies() const
{
    // R054. The pilot showed that the reach hint is right only after a
    // topology change. After a transient break the old path still exists and
    // nodes near the source still hold routes, so a narrow search is answered
    // locally; starting at the last hop count turned each such recovery on the
    // long bottleneck path into a near-network-wide flood (timely PDR 0.555 ->
    // 0.395, control bytes doubled, development seeds).
    if (m_hopHint == 0)
    {
        return false;
    }
    return CardHintAlways(m_algo) || (CardHintOnTopology(m_algo) && m_cardTopology);
}

uint16_t
RepairLedger::TtlForNextAttempt() const
{
    if (IsCardMode(m_algo))
    {
        // R053 REACH HINT. Renewal resets j to 0, so a rationed controller
        // opens every recovery with a 2-hop search wherever the destination
        // was. Stock AODV instead starts from the last known hop count plus
        // TTL_INCREMENT (RFC 3561, section 6.4). CARD does the same for the
        // first attempt, and the ring continues above it (see CommitSend), so
        // the hint moves the start of the ring but never shortens its end.
        uint16_t t = RingTtl(m_j);
        if (CardHintApplies() && t < m_ttlMax)
        {
            const uint32_t hinted = static_cast<uint32_t>(m_hopHint) + 2;
            t = static_cast<uint16_t>(std::min<uint32_t>(m_ttlMax, std::max<uint32_t>(t, hinted)));
        }
        return t;
    }

    // E4: TTL(j) = min(TTLmax, 2^(j+1)) for j < B-1; TTL(B-1) = TTLmax.
    // For B=1 the single fast request uses TTLmax.
    if (m_B == 0)
    {
        return m_ttlMax;
    }
    if (m_algo == ALGO_PERSISTENT_BACKOFF)
    {
        // PERSISTENT-BACKOFF uses full-diameter requests throughout; its control
        // variable is the interval, not the ring.
        return m_ttlMax;
    }

    if (m_algo == ALGO_SCR2)
    {
        // R031 REACH FROM EVIDENCE. Fresh confirmation means the destination was
        // reachable seconds ago, so look close before flooding. With no evidence,
        // escalate on j without bound until the ring reaches the diameter.
        //
        // This is where SCR-2 differs most from SCR in cost. SCR's first four
        // searches expand 2, 4, 8, 35 -- but once B is spent every later search
        // is a PROBE, and probes go at full diameter. Since SCR spends most of
        // its life out of allowance, most of its searches flood the whole
        // network. That is why cutting source broadcasts by 48% cut measured
        // control traffic by only 25%.
        //
        // "Start narrow" is implemented by OnServiceConfirmed resetting j to 0,
        // NOT by returning a small TTL whenever evidence is fresh. The first
        // version did the latter, and it could not escalate: while the
        // confirmation stayed fresh -- up to the whole freshness window -- every
        // attempt went out at TTL 2, so a destination that had moved three hops
        // away would receive repeated 2-hop searches that could never reach it.
        // That is the SCR lockout again in a different form. With j reset on
        // confirmation, a fresh pair starts at 2 and still escalates 4, 8, ...
        const uint32_t shift = m_j + 1;
        const uint32_t val = (shift >= 16) ? m_ttlMax : (1u << shift);
        return static_cast<uint16_t>(std::min<uint32_t>(m_ttlMax, val));
    }

    uint32_t j = m_j;
    if (m_algo == ALGO_TIME_BUCKET)
    {
        // TIME-BUCKET repeats the same [2,4,8,35] sequence every B actual
        // originations, so its ring behaviour matches SCR and any cost
        // difference cannot be attributed to a different search pattern.
        j = (m_B > 0) ? (m_originations % m_B) : 0;
    }
    else if (m_algo == ALGO_RESET_ESCALATION)
    {
        // Ablation: clamp with min(j, B-1) as specified.
        j = std::min<uint32_t>(j, m_B - 1);
    }

    if (j >= m_B - 1)
    {
        return m_ttlMax;
    }
    // 2^(j+1), guarded against shift overflow for large j.
    const uint32_t shift = j + 1;
    const uint32_t val = (shift >= 16) ? m_ttlMax : (1u << shift);
    return static_cast<uint16_t>(std::min<uint32_t>(m_ttlMax, val));
}

Time
RepairLedger::ReplyTimeout(uint16_t ttl) const
{
    if (ttl >= m_ttlMax)
    {
        // E4 full-diameter case: flat NetTraversalTime.
        // Note: upstream AODV applies binary exponential backoff here. SCR
        // deliberately does not - see DESIGN_DEVIATIONS D002.
        return m_netTraversalTime;
    }
    // T_wait(h) = 2 * NodeTraversalTime * (h + TimeoutBuffer)
    return m_nodeTraversalTime * static_cast<int64_t>(2 * (ttl + m_timeoutBuffer));
}

void
RepairLedger::CommitSend(Time now, AdmitDecision kind, Time replyTimeout)
{
    RefillProbe(now);

    if (kind == ADMIT_FAST)
    {
        if (m_algo == ALGO_TIME_BUCKET)
        {
            // TIME-BUCKET's limiter is the per-pair token bucket debited below,
            // not the SCR fast allowance. It has no B_remaining/j accounting at
            // all, so asserting on B_remaining here would abort the run.
            ++m_fastSpent;
        }
        else if (m_algo == ALGO_SCR2)
        {
            // R031. No allowance exists to debit. j still advances, because for
            // SCR-2 j is the search-reach index -- how far this episode has had
            // to look -- not a count of spent budget.
            ++m_j;
            ++m_fastSpent;
        }
        else
        {
            NS_ASSERT_MSG(m_bRemaining > 0, "CommitSend(ADMIT_FAST) with no allowance");
            // E4: B_remaining' = B_remaining - 1, j' = j + 1.
            // j increments ONLY here, at actual send handoff - never on denial
            // and never while waiting for a route.
            --m_bRemaining;
            ++m_j;
            ++m_fastSpent;
        }
    }
    else if (kind == ADMIT_EXEMPT)
    {
        NS_ASSERT_MSG(CardExempt(), "ADMIT_EXEMPT outside a CARD exemption");
        // Not charged to B or P. j still advances: it is the reach index.
        ++m_exemptSpent;
    }
    else if (kind == ADMIT_PROBE)
    {
        NS_ASSERT_MSG(m_probeTokens >= 1.0, "CommitSend(ADMIT_PROBE) with no token");
        // E5: consumes one token. A probe does not change j.
        m_probeTokens -= 1.0;
        ++m_probesSpent;
    }
    else
    {
        NS_ABORT_MSG("CommitSend called with a denial decision");
    }

    if (IsCardMode(m_algo) && (kind == ADMIT_FAST || kind == ADMIT_EXEMPT))
    {
        // The TTL that was just sent. For ADMIT_FAST j has already advanced,
        // so read the ring at the pre-send index.
        const uint32_t jSent = (kind == ADMIT_FAST) ? m_j - 1 : m_j;
        uint16_t sent = RingTtl(jSent);
        if (CardHintApplies() && sent < m_ttlMax)
        {
            sent = static_cast<uint16_t>(std::min<uint32_t>(
                m_ttlMax, std::max<uint32_t>(sent, static_cast<uint32_t>(m_hopHint) + 2)));
        }
        uint32_t j = jSent + 1;
        // Continue the ring strictly above the reach already tried, so a hinted
        // first search is never repeated at the same radius.
        while (m_B > 0 && j < m_B - 1 && RingTtl(j) <= sent)
        {
            ++j;
        }
        m_j = j;
    }

    ++m_originations;

    if (m_algo == ALGO_TIME_BUCKET)
    {
        if (now > m_bucketLastUpdate)
        {
            m_bucketTokens = std::min<double>(
                m_B, m_bucketTokens + m_bucketRefill * (now - m_bucketLastUpdate).GetSeconds());
            m_bucketLastUpdate = now;
        }
        m_bucketTokens -= 1.0;
    }

    m_inFlight = true;
    m_replyReceived = false; // R032 observation only
    m_nextEligible = now + replyTimeout;

    if (m_algo == ALGO_PERSISTENT_BACKOFF)
    {
        // Registered schedule: wait at least min(0.5*2^k, 16) s AND the reply
        // timeout. k increments after every actual origination and resets only
        // on confirmed service. This is deliberately its OWN schedule and does
        // not inherit upstream AODV's exponential backoff (see D002), so the two
        // are not accidentally the same mechanism.
        const double gap = std::min(0.5 * std::pow(2.0, static_cast<double>(m_k)), 16.0);
        const Time backoff = Seconds(gap);
        m_nextEligible = now + std::max(backoff, replyTimeout);
        m_backoffEligible = now + backoff;
        ++m_k;
    }
}

bool
RepairLedger::ExpireIfDue(Time now)
{
    if (m_inFlight && now >= m_nextEligible)
    {
        NS_LOG_INFO("stale in_flight cleared at " << now.As(Time::S)
                                                  << " (reply window closed)");
        OnReplyTimeout(now);
        return true;
    }
    return false;
}

void
RepairLedger::OnReplyTimeout(Time now)
{
    RefillProbe(now);
    m_inFlight = false;
    NS_LOG_INFO("reply timeout; pair free to originate again (B_remaining=" << m_bRemaining
                                                                           << " j=" << m_j << ")");
}

} // namespace scr
} // namespace ns3
