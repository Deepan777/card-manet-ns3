/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Service-Confirmed Repair (SCR) — per-source aggregate limiter (E6).
 *
 * Specification E6:
 *   G_s(t) = min(Gmax, G_s(t_last) + lambda*(t - t_last)), Gmax=4, lambda=2/s,
 *   initialized FULL. The budget spans all destinations and never resets after
 *   service confirmation. Admit destinations by FIFO order of becoming ready,
 *   with round-robin tie handling and stable destination order; a destination
 *   can occupy the queue only once.
 *   Debit the pair allowance and G atomically only when the routing socket send
 *   is attempted successfully.
 *
 * Traceability: M09. Validating fixture: T5.
 *
 * State location: source routing module. One scheduler per source node,
 * shared across all destinations that node originates for.
 */

#ifndef SCR_ORIGIN_SCHEDULER_H
#define SCR_ORIGIN_SCHEDULER_H

#include "ns3/ipv4-address.h"
#include "ns3/nstime.h"

#include <cstdint>
#include <deque>
#include <set>
#include <string>

namespace ns3
{
namespace scr
{

/**
 * @ingroup scr
 * @brief Node-wide origination budget and FIFO arbitration across destinations.
 */
class OriginScheduler
{
  public:
    OriginScheduler();

    void SetGmax(double v) { m_gmax = v; }
    void SetLambda(double v) { m_lambda = v; }
    double GetGmax() const { return m_gmax; }
    double GetLambda() const { return m_lambda; }

    /// E6 configuration validation: Gmax >= 1 and lambda > 0.
    bool ValidateConfiguration(std::string& why) const;

    /// Current node token level, refilled to now.
    double GetTokens(Time now) const;

    /**
     * Mark a destination ready to originate. A destination may occupy the queue
     * only once; re-marking an already-queued destination does NOT move it to
     * the back, so a flapping destination cannot starve others or jump ahead.
     * @return true if the destination was newly enqueued.
     */
    bool MarkReady(Ipv4Address destination, Time now);

    /// Remove a destination from the queue (route installed, demand ended).
    void Withdraw(Ipv4Address destination);

    /// True if this destination is currently queued.
    bool IsQueued(Ipv4Address destination) const;

    /// Destination at the head of the FIFO, or false if the queue is empty.
    bool PeekNext(Ipv4Address& destination) const;

    /// True if a node token is available at this time.
    bool HasToken(Time now) const { return GetTokens(now) >= 1.0; }

    /**
     * Debit one node token. Called ONLY at successful routing socket handoff,
     * atomically with the pair-level debit (E6).
     */
    void CommitSend(Time now, Ipv4Address destination);

    /// Time at which the next whole token becomes available.
    Time NextTokenTime(Time now) const;

    uint64_t GetAdmittedCount() const { return m_admitted; }
    size_t GetQueueLength() const { return m_fifo.size(); }

  private:
    void Refill(Time now);

    double m_gmax;
    double m_lambda;
    double m_tokens;
    Time m_lastUpdate;

    std::deque<Ipv4Address> m_fifo;
    std::set<Ipv4Address> m_queued; //!< membership test; enforces "only once"

    uint64_t m_admitted;
};

} // namespace scr
} // namespace ns3

#endif /* SCR_ORIGIN_SCHEDULER_H */
