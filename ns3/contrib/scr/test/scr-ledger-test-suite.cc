/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * SCR component fixtures for the repair ledger, origin scheduler and service
 * evidence. These are deterministic unit fixtures: no radio, no mobility.
 *
 * Covers:
 *   T1  repeated RREP / route loss without qualifying delivery
 *   T3  permanent partition after fast exhaustion
 *   T5  two destinations sharing the same source limiter
 *   T7  route deletion / application restart preserving the ledger
 *   T2  stale, out-of-order and prior-episode reports (evidence part)
 */

#include "ns3/origin-scheduler.h"
#include "ns3/repair-ledger.h"
#include "ns3/service-evidence.h"
#include "ns3/service-headers.h"
#include "ns3/test.h"

using namespace ns3;
using namespace ns3::scr;

namespace
{
/// Helper: commit a fast origination at time t.
void
Originate(RepairLedger& led, Time t)
{
    AdmitDecision d = led.Evaluate(t, true);
    NS_ASSERT(d == ADMIT_FAST || d == ADMIT_PROBE);
    uint16_t ttl = (d == ADMIT_PROBE) ? 35 : led.TtlForNextAttempt();
    led.CommitSend(t, d, led.ReplyTimeout(ttl));
}
} // namespace

// ---------------------------------------------------------------------------
// E4: TTL escalation sequence must be [2,4,8,35] for B=4.
// ---------------------------------------------------------------------------
class LedgerTtlEscalationTest : public TestCase
{
  public:
    LedgerTtlEscalationTest()
        : TestCase("E4: TTL(j) = min(TTLmax, 2^(j+1)), TTL(B-1) = TTLmax")
    {
    }

  private:
    void DoRun() override
    {
        RepairLedger led;
        led.SetB(4);
        led.OnDiscoveryNeeded(Seconds(1));

        const uint16_t expected[4] = {2, 4, 8, 35};
        Time t = Seconds(1);
        for (uint32_t j = 0; j < 4; ++j)
        {
            NS_TEST_ASSERT_MSG_EQ(led.GetEscalationIndex(), j, "escalation index tracks attempts");
            NS_TEST_ASSERT_MSG_EQ(led.TtlForNextAttempt(),
                                  expected[j],
                                  "TTL sequence must be 2,4,8,35 for B=4");
            Originate(led, t);
            led.OnReplyTimeout(t + Seconds(10));
            t += Seconds(10);
        }
        NS_TEST_ASSERT_MSG_EQ(led.GetRemainingFast(), 0, "all four fast units spent");

        // B=1: the single fast request uses TTLmax directly.
        RepairLedger one;
        one.SetB(1);
        one.OnDiscoveryNeeded(Seconds(1));
        NS_TEST_ASSERT_MSG_EQ(one.TtlForNextAttempt(), 35, "B=1 uses TTLmax for its only request");
    }
};

// ---------------------------------------------------------------------------
// T1: RREP does not restore allowance; only confirmed service does.
// ---------------------------------------------------------------------------
class LedgerRrepDoesNotRenewTest : public TestCase
{
  public:
    LedgerRrepDoesNotRenewTest()
        : TestCase("T1 E3: repeated RREP and route loss never restore allowance")
    {
    }

  private:
    void DoRun() override
    {
        RepairLedger led;
        led.SetB(4);
        led.OnDiscoveryNeeded(Seconds(1));
        const uint64_t episode = led.GetEpisodeId();

        Time t = Seconds(1);
        for (int cycle = 0; cycle < 3; ++cycle)
        {
            Originate(led, t);
            // A route arrives, then is lost again, with no qualifying delivery.
            led.OnRouteInstalled(t + MilliSeconds(100));
            led.OnRouteInvalidated(t + Seconds(1));
            t += Seconds(5);
        }

        NS_TEST_ASSERT_MSG_EQ(led.GetRemainingFast(),
                              1,
                              "three RREP cycles must consume three units, not refill");
        NS_TEST_ASSERT_MSG_EQ(led.GetEscalationIndex(), 3, "j persists across route replacements");
        NS_TEST_ASSERT_MSG_EQ(led.GetEpisodeId(), episode, "episode id unchanged by RREP/RERR");
        NS_TEST_ASSERT_MSG_EQ(led.GetMode(), LEDGER_RECOVERING, "still RECOVERING without service");

        // Confirmation while a route exists renews exactly once.
        bool renewed = led.OnServiceConfirmed(t, true);
        NS_TEST_ASSERT_MSG_EQ(renewed, true, "confirmation with a route renews allowance");
        NS_TEST_ASSERT_MSG_EQ(led.GetRemainingFast(), 4, "B_remaining reset to B");
        NS_TEST_ASSERT_MSG_EQ(led.GetEscalationIndex(), 0, "j reset to zero");
        NS_TEST_ASSERT_MSG_EQ(led.GetMode(), LEDGER_SERVING, "mode becomes SERVING");

        // A second confirmation in SERVING grants nothing further.
        bool again = led.OnServiceConfirmed(t + Seconds(1), true);
        NS_TEST_ASSERT_MSG_EQ(again, false, "feedback in SERVING grants no extra allowance");
    }
};

// ---------------------------------------------------------------------------
// E3: confirmation without a usable route must not renew.
// ---------------------------------------------------------------------------
class LedgerConfirmWithoutRouteTest : public TestCase
{
  public:
    LedgerConfirmWithoutRouteTest()
        : TestCase("E3: confirmation with no usable route retains evidence but does not renew")
    {
    }

  private:
    void DoRun() override
    {
        RepairLedger led;
        led.SetB(4);
        led.OnDiscoveryNeeded(Seconds(1));
        Originate(led, Seconds(1));
        NS_TEST_ASSERT_MSG_EQ(led.GetRemainingFast(), 3, "one unit spent");

        bool renewed = led.OnServiceConfirmed(Seconds(2), /*routeExists=*/false);
        NS_TEST_ASSERT_MSG_EQ(renewed, false, "no renewal without a usable route");
        NS_TEST_ASSERT_MSG_EQ(led.GetRemainingFast(), 3, "allowance unchanged");
        NS_TEST_ASSERT_MSG_EQ(led.GetMode(), LEDGER_RECOVERING, "still RECOVERING");
    }
};

// ---------------------------------------------------------------------------
// T3 E5: after fast exhaustion, only probe tokens permit origination.
// ---------------------------------------------------------------------------
class LedgerProbeBucketTest : public TestCase
{
  public:
    LedgerProbeBucketTest()
        : TestCase("T3 E5: probe bucket starts empty, refills at rho, caps at one")
    {
    }

  private:
    void DoRun() override
    {
        RepairLedger led;
        led.SetB(1);
        led.SetRho(0.2); // one token per 5 s
        led.OnDiscoveryNeeded(Seconds(0));

        // The bucket starts EMPTY: no free probe at t=0.
        NS_TEST_ASSERT_MSG_EQ(led.GetProbeTokens(Seconds(0)) < 1.0,
                              true,
                              "probe bucket must start empty");

        Originate(led, Seconds(0)); // spends the single fast unit
        led.OnReplyTimeout(Seconds(3));
        NS_TEST_ASSERT_MSG_EQ(led.GetRemainingFast(), 0, "fast allowance exhausted");

        // At 3 s only 0.6 tokens have accrued: denial.
        NS_TEST_ASSERT_MSG_EQ(led.Evaluate(Seconds(3), true),
                              DENY_NO_ALLOWANCE,
                              "no probe before a whole token accrues");

        // At 5 s exactly one token: probe admitted.
        NS_TEST_ASSERT_MSG_EQ(led.Evaluate(Seconds(5), true),
                              ADMIT_PROBE,
                              "one whole token permits exactly one probe");
        Originate(led, Seconds(5));
        NS_TEST_ASSERT_MSG_EQ(led.GetProbesSpent(), 1, "probe counted");
        led.OnReplyTimeout(Seconds(9));
        NS_TEST_ASSERT_MSG_EQ(led.Evaluate(Seconds(9), true),
                              DENY_NO_ALLOWANCE,
                              "token consumed; must wait again");

        // Cap: after a long idle period the bucket holds at most one token, so
        // a long outage cannot accumulate a burst of probes.
        NS_TEST_ASSERT_MSG_EQ(led.GetProbeTokens(Seconds(1000)) <= 1.0,
                              true,
                              "probe bucket is capped at one token");
    }
};

// ---------------------------------------------------------------------------
// T7: application restart must not reset accumulated debt.
// ---------------------------------------------------------------------------
class LedgerTombstoneTest : public TestCase
{
  public:
    LedgerTombstoneTest()
        : TestCase("T7 E3: session stop is a tombstone and never renews credit")
    {
    }

  private:
    void DoRun() override
    {
        RepairLedger led;
        led.SetB(4);
        led.OnDiscoveryNeeded(Seconds(1));
        Originate(led, Seconds(1));
        // E4: one pair has at most one request outstanding, so the reply timeout
        // must elapse before the next origination can be admitted.
        led.OnReplyTimeout(Seconds(10));
        Originate(led, Seconds(20));
        NS_TEST_ASSERT_MSG_EQ(led.GetRemainingFast(), 2, "two units spent");

        led.OnSessionStop(Seconds(30));
        NS_TEST_ASSERT_MSG_EQ(led.GetRemainingFast(),
                              2,
                              "session stop must not restore allowance");
        NS_TEST_ASSERT_MSG_EQ(led.IsInFlight(), false, "pending work cancelled");

        // Restarting demand reuses the same debt.
        led.OnDiscoveryNeeded(Seconds(40));
        NS_TEST_ASSERT_MSG_EQ(led.GetRemainingFast(),
                              2,
                              "restarting the application cannot reset debt");
    }
};

// ---------------------------------------------------------------------------
// E4 reply timeout formula, including the deliberate D002 difference.
// ---------------------------------------------------------------------------
class LedgerReplyTimeoutTest : public TestCase
{
  public:
    LedgerReplyTimeoutTest()
        : TestCase("E4: T_wait(h) = 2*NodeTraversalTime*(h+TimeoutBuffer); flat at full diameter")
    {
    }

  private:
    void DoRun() override
    {
        RepairLedger led; // defaults: 40 ms, buffer 2, TTLmax 35, Net 2.8 s

        // h=2 -> 2*40ms*(2+2) = 320 ms
        NS_TEST_ASSERT_MSG_EQ(led.ReplyTimeout(2), MilliSeconds(320), "h=2 timeout");
        // h=4 -> 2*40ms*(4+2) = 480 ms
        NS_TEST_ASSERT_MSG_EQ(led.ReplyTimeout(4), MilliSeconds(480), "h=4 timeout");
        // h=8 -> 2*40ms*(8+2) = 800 ms
        NS_TEST_ASSERT_MSG_EQ(led.ReplyTimeout(8), MilliSeconds(800), "h=8 timeout");
        // Full diameter: flat NetTraversalTime, NOT the native 2^k backoff (D002).
        NS_TEST_ASSERT_MSG_EQ(led.ReplyTimeout(35), MilliSeconds(2800), "full diameter is flat");
    }
};

// ---------------------------------------------------------------------------
// T5: two destinations share one node limiter, FIFO, queued only once.
// ---------------------------------------------------------------------------
class SchedulerFifoTest : public TestCase
{
  public:
    SchedulerFifoTest()
        : TestCase("T5 E6: node budget is shared FIFO and a destination queues only once")
    {
    }

  private:
    void DoRun() override
    {
        OriginScheduler s;
        s.SetGmax(4);
        s.SetLambda(2);

        Ipv4Address a("10.1.0.2");
        Ipv4Address b("10.1.0.3");

        NS_TEST_ASSERT_MSG_EQ(s.MarkReady(a, Seconds(0)), true, "a enqueued");
        NS_TEST_ASSERT_MSG_EQ(s.MarkReady(b, Seconds(0)), true, "b enqueued");
        // Re-marking must not re-enqueue or reorder.
        NS_TEST_ASSERT_MSG_EQ(s.MarkReady(a, Seconds(0)), false, "a cannot queue twice");
        NS_TEST_ASSERT_MSG_EQ(s.GetQueueLength(), 2, "queue holds two distinct destinations");

        Ipv4Address head;
        NS_TEST_ASSERT_MSG_EQ(s.PeekNext(head), true, "queue non-empty");
        NS_TEST_ASSERT_MSG_EQ(head, a, "FIFO order preserved: a first");

        // Node budget starts full at Gmax=4.
        NS_TEST_ASSERT_MSG_EQ(s.GetTokens(Seconds(0)), 4.0, "node budget initialised full");

        s.CommitSend(Seconds(0), a);
        NS_TEST_ASSERT_MSG_EQ(s.GetTokens(Seconds(0)), 3.0, "one token debited");
        NS_TEST_ASSERT_MSG_EQ(s.IsQueued(a), false, "admitted destination leaves the queue");
        NS_TEST_ASSERT_MSG_EQ(s.PeekNext(head), true, "b still queued");
        NS_TEST_ASSERT_MSG_EQ(head, b, "b is now at the head");

        // Exhaust the budget and check refill timing at lambda = 2/s.
        s.CommitSend(Seconds(0), b);
        s.MarkReady(a, Seconds(0));
        s.CommitSend(Seconds(0), a);
        s.MarkReady(b, Seconds(0));
        s.CommitSend(Seconds(0), b);
        NS_TEST_ASSERT_MSG_EQ(s.GetTokens(Seconds(0)), 0.0, "budget exhausted");
        NS_TEST_ASSERT_MSG_EQ(s.HasToken(Seconds(0)), false, "no token available");
        // 1 token needs 0.5 s at lambda=2.
        NS_TEST_ASSERT_MSG_EQ(s.HasToken(Seconds(0.5)), true, "token refills after 0.5 s");
        NS_TEST_ASSERT_MSG_EQ(s.GetTokens(Seconds(10)), 4.0, "budget caps at Gmax");
    }
};

// ---------------------------------------------------------------------------
// T2: evidence acceptance and the E2 confirmation rule.
// ---------------------------------------------------------------------------
class EvidenceConfirmationTest : public TestCase
{
  public:
    EvidenceConfirmationTest()
        : TestCase("T2 E2: K consecutive good fresh blocks at or after episode start")
    {
    }

    /// Build a receipt for block b with n on-time packets.
    static ServiceReceiptHeader MakeReceipt(uint64_t session,
                                            uint64_t block,
                                            uint32_t n,
                                            uint32_t M,
                                            Time closeTime)
    {
        ServiceReceiptHeader rh;
        rh.SetSessionId(session);
        rh.SetBlockId(block);
        rh.SetBlockSize(M);
        rh.SetCloseTime(closeTime);
        uint32_t mask = 0;
        for (uint32_t i = 0; i < n; ++i)
        {
            mask |= (1u << i);
        }
        rh.SetOntimeMask(mask);
        return rh;
    }

  private:
    void DoRun() override
    {
        const uint64_t session = 7;
        const uint32_t M = 10;
        const Time I = MilliSeconds(50);
        const Time D = MilliSeconds(250);
        const Time t0 = Seconds(0);

        ServiceEvidence ev;
        ev.Register(session, Ipv4Address("10.1.0.9"), t0, I, D, M);
        ev.SetTheta(0.8);
        ev.SetK(2);
        ev.SetFreshness(Seconds(2));
        ev.SetHighestGeneratedSequence(1000);

        std::string why;
        NS_TEST_ASSERT_MSG_EQ(ev.ValidateConfiguration(why),
                              true,
                              "F=2s satisfies F >= (K-1)*M*I = 0.5 s");

        // Block 0 closes at t0 + (10-1)*50ms + 250ms = 700 ms.
        const Time c0 = ev.BlockCloseTime(0);
        NS_TEST_ASSERT_MSG_EQ(c0, MilliSeconds(700), "c_0 derived from the schedule");
        const Time c1 = ev.BlockCloseTime(1);
        NS_TEST_ASSERT_MSG_EQ(c1, MilliSeconds(1200), "c_1 = c_0 + M*I");

        // A future block must be rejected: it cannot have closed.
        NS_TEST_ASSERT_MSG_EQ(ev.AcceptReport(MakeReceipt(session, 5, 10, M, ev.BlockCloseTime(5)),
                                              MilliSeconds(700)),
                              false,
                              "future block rejected");
        NS_TEST_ASSERT_MSG_EQ(ev.GetLastRejectReason(),
                              REJECT_BLOCK_NOT_CLOSED,
                              "reason recorded as block_not_closed");

        // Wrong session rejected.
        NS_TEST_ASSERT_MSG_EQ(ev.AcceptReport(MakeReceipt(999, 0, 10, M, c0), MilliSeconds(750)),
                              false,
                              "session mismatch rejected");
        NS_TEST_ASSERT_MSG_EQ(ev.GetLastRejectReason(), REJECT_SESSION_MISMATCH, "reason recorded");

        // Good block 0 (10/10 >= 0.8) accepted.
        NS_TEST_ASSERT_MSG_EQ(ev.AcceptReport(MakeReceipt(session, 0, 10, M, c0),
                                              MilliSeconds(750)),
                              true,
                              "good block accepted");
        // Duplicate ignored.
        NS_TEST_ASSERT_MSG_EQ(ev.AcceptReport(MakeReceipt(session, 0, 10, M, c0),
                                              MilliSeconds(760)),
                              false,
                              "duplicate block rejected");
        NS_TEST_ASSERT_MSG_EQ(ev.GetLastRejectReason(), REJECT_DUPLICATE, "reason recorded");

        // One good block is not enough for K=2.
        NS_TEST_ASSERT_MSG_EQ(ev.CanConfirm(MilliSeconds(760), Seconds(0)),
                              false,
                              "K=2 needs two consecutive good blocks");

        // Block 1 good -> confirmation holds.
        NS_TEST_ASSERT_MSG_EQ(ev.AcceptReport(MakeReceipt(session, 1, 9, M, c1),
                                              MilliSeconds(1250)),
                              true,
                              "second good block accepted (9/10 >= 0.8)");
        NS_TEST_ASSERT_MSG_EQ(ev.CanConfirm(MilliSeconds(1250), Seconds(0)),
                              true,
                              "two consecutive fresh good blocks confirm");

        // A block below theta breaks the run.
        ServiceEvidence ev2;
        ev2.Register(session, Ipv4Address("10.1.0.9"), t0, I, D, M);
        ev2.SetHighestGeneratedSequence(1000);
        ev2.AcceptReport(MakeReceipt(session, 0, 10, M, ev2.BlockCloseTime(0)), MilliSeconds(750));
        ev2.AcceptReport(MakeReceipt(session, 1, 7, M, ev2.BlockCloseTime(1)), MilliSeconds(1250));
        NS_TEST_ASSERT_MSG_EQ(ev2.CanConfirm(MilliSeconds(1250), Seconds(0)),
                              false,
                              "7/10 = 0.7 < theta breaks the consecutive run");

        // Episode gating: a pre-outage block must not close a newer episode.
        NS_TEST_ASSERT_MSG_EQ(ev.CanConfirm(MilliSeconds(1250), Seconds(1)),
                              false,
                              "blocks generated before the episode start cannot confirm it");

        // Staleness: beyond c_b + F the evidence no longer supports confirmation.
        NS_TEST_ASSERT_MSG_EQ(ev.CanConfirm(Seconds(10), Seconds(0)),
                              false,
                              "expired blocks cannot confirm");
    }
};

// ---------------------------------------------------------------------------
// E2 structural condition F >= (K-1)*M*I must be enforced.
// ---------------------------------------------------------------------------
class EvidenceConfigValidationTest : public TestCase
{
  public:
    EvidenceConfigValidationTest()
        : TestCase("E2: configuration validation enforces F >= (K-1)*M*I")
    {
    }

  private:
    void DoRun() override
    {
        ServiceEvidence ev;
        ev.Register(1, Ipv4Address("10.1.0.9"), Seconds(0), MilliSeconds(50), MilliSeconds(250), 10);
        ev.SetK(4);
        ev.SetFreshness(MilliSeconds(100)); // (K-1)*M*I = 1.5 s > 0.1 s

        std::string why;
        NS_TEST_ASSERT_MSG_EQ(ev.ValidateConfiguration(why),
                              false,
                              "F too small must be rejected, not silently accepted");
        NS_TEST_ASSERT_MSG_EQ(why.empty(), false, "a reason must be reported");
    }
};

// ---------------------------------------------------------------------------
// E5 configuration: rho = 0 only for the named NO-PROBE ablation.
// ---------------------------------------------------------------------------
class LedgerConfigValidationTest : public TestCase
{
  public:
    LedgerConfigValidationTest()
        : TestCase("E5: rho=0 admitted only for the named NO-PROBE ablation")
    {
    }

  private:
    void DoRun() override
    {
        RepairLedger led;
        led.SetRho(0.0);
        std::string why;
        NS_TEST_ASSERT_MSG_EQ(led.ValidateConfiguration(why, /*allowZeroRho=*/false),
                              false,
                              "rho=0 rejected outside the NO-PROBE ablation");
        NS_TEST_ASSERT_MSG_EQ(led.ValidateConfiguration(why, /*allowZeroRho=*/true),
                              true,
                              "rho=0 permitted for the declared ablation");

        RepairLedger bad;
        bad.SetB(9);
        NS_TEST_ASSERT_MSG_EQ(bad.ValidateConfiguration(why, false), false, "B=9 outside [1,8]");
    }
};


// ---------------------------------------------------------------------------
// T8: a reply timeout and an RREP that resolve at the same instant must not
// double-count, and must leave the ledger in a coherent state.
// ---------------------------------------------------------------------------
class LedgerSimultaneousTimeoutAndRrepTest : public TestCase
{
  public:
    LedgerSimultaneousTimeoutAndRrepTest()
        : TestCase("T8 E4: simultaneous reply timeout and RREP leave one coherent state")
    {
    }

  private:
    void DoRun() override
    {
        RepairLedger led;
        led.SetB(4);
        led.OnDiscoveryNeeded(Seconds(1));
        Originate(led, Seconds(1));

        NS_TEST_ASSERT_MSG_EQ(led.IsInFlight(), true, "request outstanding");
        const uint32_t spentBefore = led.GetRemainingFast();
        const uint32_t jBefore = led.GetEscalationIndex();

        // Both events land at the same simulated instant. Whichever runs first,
        // the outcome must be identical: in_flight cleared exactly once, and no
        // allowance consumed or refunded by either event.
        const Time tSame = Seconds(1) + led.ReplyTimeout(2);
        led.OnReplyTimeout(tSame);
        led.OnRouteInstalled(tSame);

        NS_TEST_ASSERT_MSG_EQ(led.IsInFlight(), false, "in_flight cleared exactly once");
        NS_TEST_ASSERT_MSG_EQ(led.GetRemainingFast(), spentBefore,
                              "neither event may change the allowance");
        NS_TEST_ASSERT_MSG_EQ(led.GetEscalationIndex(), jBefore,
                              "neither event may change j");

        // Reverse order must give the same state.
        RepairLedger led2;
        led2.SetB(4);
        led2.OnDiscoveryNeeded(Seconds(1));
        Originate(led2, Seconds(1));
        led2.OnRouteInstalled(tSame);
        led2.OnReplyTimeout(tSame);

        NS_TEST_ASSERT_MSG_EQ(led2.GetRemainingFast(), led.GetRemainingFast(),
                              "event order must not change the allowance");
        NS_TEST_ASSERT_MSG_EQ(led2.GetEscalationIndex(), led.GetEscalationIndex(),
                              "event order must not change j");
    }
};

// ---------------------------------------------------------------------------
// E9: a pair must never be permanently locked out. This is the regression test
// for the defect where a stale in_flight marker survived forever because the
// upstream retry timer had stopped.
// ---------------------------------------------------------------------------
class LedgerNoPermanentLockoutTest : public TestCase
{
  public:
    LedgerNoPermanentLockoutTest()
        : TestCase("E9: a stale in_flight marker can never lock a pair out permanently")
    {
    }

  private:
    void DoRun() override
    {
        RepairLedger led;
        led.SetB(1);
        led.SetRho(0.2);
        led.OnDiscoveryNeeded(Seconds(0));
        Originate(led, Seconds(0));

        // Deliberately never call OnReplyTimeout or OnRouteInstalled: this
        // reproduces the exact condition where upstream AODV has deleted the
        // routing entry and stopped its retry timer.
        NS_TEST_ASSERT_MSG_EQ(led.IsInFlight(), true, "request still marked outstanding");
        NS_TEST_ASSERT_MSG_EQ(led.Evaluate(Seconds(100), true), DENY_IN_FLIGHT,
                              "without the guard the pair would stay blocked");

        // ExpireIfDue must clear it once the reply window has closed.
        const bool cleared = led.ExpireIfDue(Seconds(100));
        NS_TEST_ASSERT_MSG_EQ(cleared, true, "stale marker must be cleared");
        NS_TEST_ASSERT_MSG_EQ(led.IsInFlight(), false, "pair is free again");
        NS_TEST_ASSERT_MSG_EQ(led.Evaluate(Seconds(100), true), ADMIT_PROBE,
                              "a probe token has accrued and must now be admissible");

        // And it must be idempotent: calling it again does nothing harmful.
        NS_TEST_ASSERT_MSG_EQ(led.ExpireIfDue(Seconds(100)), false,
                              "no stale marker remains to clear");
    }
};

// ---------------------------------------------------------------------------
// E6: the node budget must be debited atomically with the pair allowance, and
// only at a successful handoff. A denial must cost nothing.
// ---------------------------------------------------------------------------
class SchedulerDenialCostsNothingTest : public TestCase
{
  public:
    SchedulerDenialCostsNothingTest()
        : TestCase("E6: a denied origination consumes no node token and no allowance")
    {
    }

  private:
    void DoRun() override
    {
        OriginScheduler s;
        s.SetGmax(4);
        s.SetLambda(2);
        RepairLedger led;
        led.SetB(4);
        led.OnDiscoveryNeeded(Seconds(0));

        const double tokensBefore = s.GetTokens(Seconds(0));
        const uint32_t allowanceBefore = led.GetRemainingFast();

        // Evaluate alone must never mutate state - debiting happens only in
        // CommitSend, at the socket handoff boundary.
        for (int i = 0; i < 5; ++i)
        {
            led.Evaluate(Seconds(0), true);
        }
        NS_TEST_ASSERT_MSG_EQ(s.GetTokens(Seconds(0)), tokensBefore,
                              "evaluation must not spend node tokens");
        NS_TEST_ASSERT_MSG_EQ(led.GetRemainingFast(), allowanceBefore,
                              "evaluation must not spend pair allowance");

        // A denial for lack of demand likewise costs nothing.
        NS_TEST_ASSERT_MSG_EQ(led.Evaluate(Seconds(0), false), DENY_NO_DEMAND, "denied");
        NS_TEST_ASSERT_MSG_EQ(led.GetRemainingFast(), allowanceBefore, "still unspent");
    }
};

// ---------------------------------------------------------------------------
// R014 regression: enqueuing must not perturb token accounting.
//
// RoutingProtocol::ScrTryDiscovery tests HasToken(now), and on a false result
// calls MarkReady(dst, now) and then NextTokenTime(now) to schedule its retry.
// If MarkReady mutates the token state, the third call reads a state the first
// never saw and can answer "a token is already available" -> `now` -> a
// zero-delay retry that re-enters at the same timestamp and freezes simulated
// time. That is exactly what happened: a 300 s run stalled past two hours at
// t=128.6 s at full CPU, constant memory and total log silence.
//
// The invariant that forecloses it: MarkReady is pure queue bookkeeping and
// must leave GetTokens() bit-identical.
// ---------------------------------------------------------------------------
class SchedulerMarkReadyDoesNotPerturbTokensTest : public TestCase
{
  public:
    SchedulerMarkReadyDoesNotPerturbTokensTest()
        : TestCase("R014: MarkReady is pure bookkeeping and never perturbs the token count")
    {
    }

  private:
    void DoRun() override
    {
        OriginScheduler s;
        s.SetGmax(4);
        s.SetLambda(2);

        // Deliberately "dirty" timestamps: the failure needed a token level a
        // single ULP away from 1.0, which round decimal times never produce.
        Time now = Seconds(0);
        int clamped = 0;
        for (int step = 1; step <= 6000; ++step)
        {
            now += NanoSeconds(37 + (step % 971) * 1013);
            Ipv4Address dst(static_cast<uint32_t>(0x0A010000u + (step % 8) + 50));

            if (s.HasToken(now))
            {
                s.CommitSend(now, dst);
                continue;
            }

            // The exact call order used by ScrTryDiscovery.
            const double before = s.GetTokens(now);
            s.MarkReady(dst, now);
            const double after = s.GetTokens(now);

            NS_TEST_ASSERT_MSG_EQ(before, after,
                                  "MarkReady perturbed the token count at "
                                      << now.As(Time::NS) << " (" << before << " -> " << after
                                      << "); this is what let NextTokenTime disagree with "
                                         "HasToken and livelock the simulator");

            if (!(s.NextTokenTime(now) > now))
            {
                ++clamped;
            }
        }

        NS_TEST_ASSERT_MSG_EQ(clamped, 0,
                              "NextTokenTime returned a non-advancing retry instant "
                                  << clamped << " times while reporting no available token; "
                                     "each one is a zero-delay reschedule that freezes "
                                     "simulated time (R014)");
    }
};

// ---------------------------------------------------------------------------

class ScrLedgerTestSuite : public TestSuite
{
  public:
    ScrLedgerTestSuite()
        : TestSuite("scr-ledger", Type::UNIT)
    {
        AddTestCase(new LedgerTtlEscalationTest, TestCase::Duration::QUICK);
        AddTestCase(new LedgerRrepDoesNotRenewTest, TestCase::Duration::QUICK);
        AddTestCase(new LedgerConfirmWithoutRouteTest, TestCase::Duration::QUICK);
        AddTestCase(new LedgerProbeBucketTest, TestCase::Duration::QUICK);
        AddTestCase(new LedgerTombstoneTest, TestCase::Duration::QUICK);
        AddTestCase(new LedgerReplyTimeoutTest, TestCase::Duration::QUICK);
        AddTestCase(new SchedulerFifoTest, TestCase::Duration::QUICK);
        AddTestCase(new EvidenceConfirmationTest, TestCase::Duration::QUICK);
        AddTestCase(new EvidenceConfigValidationTest, TestCase::Duration::QUICK);
        AddTestCase(new LedgerConfigValidationTest, TestCase::Duration::QUICK);
        AddTestCase(new LedgerSimultaneousTimeoutAndRrepTest, TestCase::Duration::QUICK);
        AddTestCase(new LedgerNoPermanentLockoutTest, TestCase::Duration::QUICK);
        AddTestCase(new SchedulerDenialCostsNothingTest, TestCase::Duration::QUICK);
        AddTestCase(new SchedulerMarkReadyDoesNotPerturbTokensTest, TestCase::Duration::QUICK);
    }
};

static ScrLedgerTestSuite g_scrLedgerTestSuite;
