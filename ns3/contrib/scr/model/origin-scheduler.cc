/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * SCR — per-source aggregate limiter. Traceability M09 (E6).
 */

#include "origin-scheduler.h"

#include "ns3/log.h"

#include <algorithm>
#include <sstream>

namespace ns3
{
namespace scr
{

NS_LOG_COMPONENT_DEFINE("ScrOriginScheduler");

OriginScheduler::OriginScheduler()
    : m_gmax(4.0),
      m_lambda(2.0),
      // E6: "initialize full". Unlike the probe bucket P (which starts empty),
      // the node budget starts at Gmax.
      m_tokens(4.0),
      m_lastUpdate(Seconds(0)),
      m_admitted(0)
{
}

bool
OriginScheduler::ValidateConfiguration(std::string& why) const
{
    std::ostringstream oss;
    bool ok = true;
    if (m_gmax < 1.0)
    {
        oss << "Gmax=" << m_gmax << " must be >= 1; ";
        ok = false;
    }
    if (m_lambda <= 0.0)
    {
        // lambda = 0 would make the node budget permanently unrefillable, which
        // is a lockout by configuration rather than a declared condition.
        oss << "lambda=" << m_lambda << " must be > 0; ";
        ok = false;
    }
    why = oss.str();
    return ok;
}

void
OriginScheduler::Refill(Time now)
{
    if (now <= m_lastUpdate)
    {
        return;
    }
    const double dt = (now - m_lastUpdate).GetSeconds();
    m_tokens = std::min(m_gmax, m_tokens + m_lambda * dt);
    m_lastUpdate = now;
}

double
OriginScheduler::GetTokens(Time now) const
{
    if (now <= m_lastUpdate)
    {
        return m_tokens;
    }
    const double dt = (now - m_lastUpdate).GetSeconds();
    return std::min(m_gmax, m_tokens + m_lambda * dt);
}

bool
OriginScheduler::MarkReady(Ipv4Address destination, Time now)
{
    // R014 ROOT CAUSE. This used to call Refill(now), which mutates m_tokens
    // and m_lastUpdate. Enqueuing is pure queue bookkeeping and needs neither:
    // GetTokens()/HasToken() refill on the fly from m_lastUpdate, and
    // CommitSend() refills before it debits, so token accounting is unaffected.
    //
    // The mutation was actively harmful. RoutingProtocol::ScrTryDiscovery tests
    // HasToken(now), and on a false result calls MarkReady(dst, now) and then
    // NextTokenTime(now). With the Refill here, that third call read a state
    // the first call never saw: Refill stores std::min(gmax, m_tokens +
    // lambda*dt) while GetTokens recomputes it, and the two can differ by one
    // ULP (the compiler is free to contract one into an FMA and not the other).
    // When the stored value landed at >= 1.0 while the on-the-fly value had
    // been < 1.0, NextTokenTime took its "token already available" early return
    // and answered `now`. The caller scheduled its retry at zero delay, the
    // retry re-entered at the same timestamp against the same state, and
    // simulated time stopped advancing -- at full CPU, at constant memory, and
    // with no log line, because this branch had none. A 300 s run sat at
    // t=128.6 s for over two hours.
    //
    // Removing the Refill makes both reads observe identical state, so the
    // disagreement cannot occur. The clamp in ScrTryDiscovery is retained as
    // defence in depth and is counted, not silently applied.
    (void)now;
    if (m_queued.find(destination) != m_queued.end())
    {
        // E6: "a destination can occupy the queue only once". Re-marking must
        // NOT re-enqueue and must NOT move the entry to the back; otherwise a
        // destination that repeatedly becomes ready could either starve others
        // or unfairly refresh its own position.
        return false;
    }
    m_fifo.push_back(destination);
    m_queued.insert(destination);
    return true;
}

void
OriginScheduler::Withdraw(Ipv4Address destination)
{
    auto it = std::find(m_fifo.begin(), m_fifo.end(), destination);
    if (it != m_fifo.end())
    {
        m_fifo.erase(it);
    }
    m_queued.erase(destination);
}

bool
OriginScheduler::IsQueued(Ipv4Address destination) const
{
    return m_queued.find(destination) != m_queued.end();
}

bool
OriginScheduler::PeekNext(Ipv4Address& destination) const
{
    if (m_fifo.empty())
    {
        return false;
    }
    destination = m_fifo.front();
    return true;
}

void
OriginScheduler::CommitSend(Time now, Ipv4Address destination)
{
    Refill(now);
    NS_ASSERT_MSG(m_tokens >= 1.0, "OriginScheduler::CommitSend with no node token");
    m_tokens -= 1.0;
    ++m_admitted;

    // Round-robin tie handling: the admitted destination leaves the queue head.
    // It re-enters at the back only when it next becomes ready, so no single
    // destination can monopolise the node budget while others wait.
    Withdraw(destination);
}

Time
OriginScheduler::NextTokenTime(Time now) const
{
    const double have = GetTokens(now);
    if (have >= 1.0)
    {
        return now;
    }
    const double need = 1.0 - have;
    Time delay = Seconds(need / m_lambda);

    // LIVELOCK GUARD (R014). Seconds() truncates to the simulator's time
    // resolution. When the deficit is smaller than one time unit -- which
    // happens routinely, because CommitSend debits exactly 1.0 while Refill
    // accumulates in floating point, leaving `have` a few ULPs below 1.0 --
    // the delay becomes exactly zero. A caller that reschedules itself at
    // NextTokenTime then re-enters at the SAME timestamp with the SAME token
    // level and reschedules again: an unbreakable zero-delay loop that freezes
    // simulated time while burning CPU, with no growth in the event queue to
    // betray it. Observed as a hard stall at t=128.575000000 s.
    //
    // Guarantee strictly forward progress. One time unit is always enough to
    // clear a sub-unit deficit: advancing dt refills lambda*dt, and if the
    // deficit needed less than one unit of refill then one unit covers it.
    if (delay <= Time(0))
    {
        delay = TimeStep(1);
    }
    return now + delay;
}

} // namespace scr
} // namespace ns3
