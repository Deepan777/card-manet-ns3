/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * SCR — source-side service evidence. Traceability M05 (E2).
 */

#include "service-evidence.h"

#include "ns3/log.h"
#include "ns3/simulator.h"

#include <sstream>

namespace ns3
{
namespace scr
{

NS_LOG_COMPONENT_DEFINE("ScrServiceEvidence");
NS_OBJECT_ENSURE_REGISTERED(ServiceEvidence);

const char*
ReportRejectReasonName(ReportRejectReason r)
{
    switch (r)
    {
    case REJECT_NONE: return "none";
    case REJECT_SESSION_MISMATCH: return "session_mismatch";
    case REJECT_SCHEDULE_MISMATCH: return "schedule_mismatch";
    case REJECT_BLOCK_NOT_CLOSED: return "block_not_closed";
    case REJECT_STALE: return "stale";
    case REJECT_DUPLICATE: return "duplicate";
    case REJECT_IMPOSSIBLE_MASK: return "impossible_mask";
    case REJECT_MALFORMED: return "malformed";
    }
    return "unknown";
}

TypeId
ServiceEvidence::GetTypeId()
{
    static TypeId tid = TypeId("ns3::scr::ServiceEvidence")
                            .SetParent<Object>()
                            .SetGroupName("Scr")
                            .AddConstructor<ServiceEvidence>()
                            .AddTraceSource("Report",
                                            "A receipt was accepted or rejected",
                                            MakeTraceSourceAccessor(&ServiceEvidence::m_reportTrace),
                                            "ns3::scr::ServiceEvidence::ReportTracedCallback");
    return tid;
}

ServiceEvidence::ServiceEvidence()
    : m_registered(false),
      m_sessionId(0),
      m_t0(Seconds(0)),
      m_interval(MilliSeconds(50)),
      m_deadline(MilliSeconds(250)),
      m_blockSize(10),
      m_highestGenerated(0),
      m_theta(0.8),
      m_k(2),
      m_freshness(Seconds(2)),
      m_accepted(0),
      m_rejected(0),
      m_lastReject(REJECT_NONE)
{
}

ServiceEvidence::~ServiceEvidence()
{
}

void
ServiceEvidence::Register(uint64_t sessionId,
                          Ipv4Address destination,
                          Time t0,
                          Time interval,
                          Time deadline,
                          uint32_t blockSize)
{
    m_sessionId = sessionId;
    m_destination = destination;
    m_t0 = t0;
    m_interval = interval;
    m_deadline = deadline;
    m_blockSize = blockSize;
    m_registered = true;
}

Time
ServiceEvidence::BlockFirstGenerationTime(uint64_t block) const
{
    return m_t0 + m_interval * static_cast<int64_t>(block * m_blockSize);
}

Time
ServiceEvidence::BlockCloseTime(uint64_t block) const
{
    // c_b = t0 + ((b+1)M - 1) * I + D
    return m_t0 + m_interval * static_cast<int64_t>((block + 1) * m_blockSize - 1) + m_deadline;
}

bool
ServiceEvidence::ValidateConfiguration(std::string& why) const
{
    std::ostringstream oss;
    bool ok = true;

    if (m_theta < 0.0 || m_theta > 1.0)
    {
        oss << "theta must be in [0,1]; ";
        ok = false;
    }
    if (m_k < 1)
    {
        oss << "K must be >= 1; ";
        ok = false;
    }
    if (m_blockSize < 1 || m_blockSize > 32)
    {
        oss << "M must be in [1,32]; ";
        ok = false;
    }
    // E2 structural condition: F >= (K-1)*M*I. Without it, K successive reports
    // cannot all be simultaneously fresh even on a zero-delay path, so
    // confirmation would be unreachable by construction rather than by network
    // behaviour - which would silently make SCR never confirm and look like a
    // protocol result instead of a configuration error.
    Time required = m_interval * static_cast<int64_t>((m_k - 1) * m_blockSize);
    if (m_freshness < required)
    {
        oss << "F=" << m_freshness.As(Time::S) << " < (K-1)*M*I=" << required.As(Time::S)
            << "; ";
        ok = false;
    }
    why = oss.str();
    return ok;
}

bool
ServiceEvidence::AcceptReport(const ServiceReceiptHeader& rh, Time now)
{
    ReportRejectReason reason = REJECT_NONE;
    const uint64_t b = rh.GetBlockId();

    if (!m_registered || !rh.IsValid())
    {
        reason = REJECT_MALFORMED;
    }
    else if (rh.GetSessionId() != m_sessionId)
    {
        reason = REJECT_SESSION_MISMATCH;
    }
    else if (rh.GetBlockSize() != m_blockSize)
    {
        // The receiver echoes M from the serialized schedule; a mismatch means
        // the report does not describe the schedule this source is generating.
        reason = REJECT_SCHEDULE_MISMATCH;
    }
    else
    {
        const Time c_b = BlockCloseTime(b);

        if (now < c_b)
        {
            // Future block: cannot have closed yet. Accepting it would let a
            // receipt confirm service that has not happened.
            reason = REJECT_BLOCK_NOT_CLOSED;
        }
        else if (rh.GetCloseTime() != c_b)
        {
            // The claimed closing time must match the locally derived schedule.
            reason = REJECT_SCHEDULE_MISMATCH;
        }
        else if (now > c_b + m_freshness)
        {
            // Equality at freshness expiry is accepted (E2), so strict > here.
            reason = REJECT_STALE;
        }
        else if (m_blocks.find(b) != m_blocks.end())
        {
            reason = REJECT_DUPLICATE;
        }
        else
        {
            // Impossible mask: the report must not claim on-time delivery of a
            // packet this source never generated.
            const uint64_t lastSeqOfBlock = (b + 1) * m_blockSize - 1;
            uint32_t highestBitSet = 0;
            bool anyBit = false;
            for (uint32_t i = 0; i < m_blockSize; ++i)
            {
                if (rh.GetOntimeMask() & (1u << i))
                {
                    highestBitSet = i;
                    anyBit = true;
                }
            }
            if (anyBit)
            {
                const uint64_t claimedSeq = b * m_blockSize + highestBitSet;
                if (claimedSeq > m_highestGenerated || lastSeqOfBlock < claimedSeq)
                {
                    reason = REJECT_IMPOSSIBLE_MASK;
                }
            }
        }
    }

    if (reason != REJECT_NONE)
    {
        ++m_rejected;
        m_lastReject = reason;
        NS_LOG_INFO("report block " << b << " rejected: " << ReportRejectReasonName(reason));
        m_reportTrace(b, false, static_cast<uint8_t>(reason));
        return false;
    }

    BlockRecord rec;
    rec.ontime = rh.CountOntime();
    rec.blockSize = rh.GetBlockSize();
    rec.closeTime = BlockCloseTime(b);
    rec.acceptedAt = now;
    // q_b = ontime / M ; good_b = 1{q_b >= theta}
    const double q = static_cast<double>(rec.ontime) / static_cast<double>(m_blockSize);
    rec.good = (q >= m_theta);

    m_blocks[b] = rec;
    ++m_accepted;
    m_lastReject = REJECT_NONE;
    NS_LOG_INFO("report block " << b << " accepted: q=" << q << " good=" << rec.good);
    m_reportTrace(b, true, static_cast<uint8_t>(REJECT_NONE));
    return true;
}

bool
ServiceEvidence::CanConfirm(Time now, Time episodeStart) const
{
    if (!m_registered || m_blocks.empty())
    {
        return false;
    }

    // E2: there must exist b such that blocks b .. b+K-1 are all good, all still
    // fresh at now, and each block's FIRST generation time is at or after the
    // episode start a_e. The search is over consecutive BLOCK IDs, not over
    // arrival order, because reports may arrive out of order.
    for (const auto& kv : m_blocks)
    {
        const uint64_t start = kv.first;
        bool run = true;
        for (uint32_t i = 0; i < m_k; ++i)
        {
            auto it = m_blocks.find(start + i);
            if (it == m_blocks.end() || !it->second.good)
            {
                run = false;
                break;
            }
            // Freshness at now; equality at expiry is accepted.
            if (now > it->second.closeTime + m_freshness)
            {
                run = false;
                break;
            }
            // No pre-outage report may close a newly opened episode.
            if (BlockFirstGenerationTime(start + i) < episodeStart)
            {
                run = false;
                break;
            }
        }
        if (run)
        {
            return true;
        }
    }
    return false;
}

void
ServiceEvidence::PruneExpired(Time now)
{
    for (auto it = m_blocks.begin(); it != m_blocks.end();)
    {
        if (now > it->second.closeTime + m_freshness)
        {
            it = m_blocks.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

} // namespace scr
} // namespace ns3
