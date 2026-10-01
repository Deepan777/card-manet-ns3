/*
 * Copyright (c) 2009 IITP RAS
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Based on
 *      NS-2 AODV model developed by the CMU/MONARCH group and optimized and
 *      tuned by Samir Das and Mahesh Marina, University of Cincinnati;
 *
 *      AODV-UU implementation by Erik Nordström of Uppsala University
 *      https://web.archive.org/web/20100527072022/http://core.it.uu.se/core/index.php/AODV-UU
 *
 * Authors: Elena Buchatskaia <borovkovaes@iitp.ru>
 *          Pavel Boyko <boyko@iitp.ru>
 */
/*
 * ---------------------------------------------------------------------------
 * SCR FORK NOTICE
 *
 * This file is a fork of the ns-3.46.1 AODV module (src/aodv), taken verbatim
 * and then modified for the Service-Confirmed Repair (SCR) study. The upstream
 * copyright, licence and authorship above are those of the original AODV
 * implementation and are reproduced unchanged.
 *
 * Upstream source : ns-3.46.1, commit 51387bce7e5f5c57aa080612a4ed690bf33d0c92
 * Fork created    : 2026-09-07
 * Fork namespace  : ns3::scr  (upstream ns3::aodv remains present and untouched,
 *                   so that AODV-STOCK is a genuine native reference)
 *
 * SCR-specific modifications are marked with "SCR:" comments. Where no such
 * marker appears, the logic is upstream AODV behaviour.
 * ---------------------------------------------------------------------------
 */

#include "scr-rqueue.h"

#include "ns3/ipv4-route.h"
#include "ns3/log.h"
#include "ns3/socket.h"

#include <algorithm>
#include <functional>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("ScrRequestQueue");

namespace scr
{
uint32_t
RequestQueue::GetSize()
{
    Purge();
    return m_queue.size();
}

bool
RequestQueue::Enqueue(QueueEntry& entry)
{
    Purge();
    for (auto i = m_queue.begin(); i != m_queue.end(); ++i)
    {
        if ((i->GetPacket()->GetUid() == entry.GetPacket()->GetUid()) &&
            (i->GetIpv4Header().GetDestination() == entry.GetIpv4Header().GetDestination()))
        {
            return false;
        }
    }
    entry.SetExpireTime(m_queueTimeout);
    if (m_queue.size() == m_maxLen)
    {
        Drop(m_queue.front(), "Drop the most aged packet"); // Drop the most aged packet
        m_queue.erase(m_queue.begin());
    }
    m_queue.push_back(entry);
    return true;
}

void
RequestQueue::DropPacketWithDst(Ipv4Address dst)
{
    NS_LOG_FUNCTION(this << dst);
    Purge();
    for (auto i = m_queue.begin(); i != m_queue.end(); ++i)
    {
        if (i->GetIpv4Header().GetDestination() == dst)
        {
            Drop(*i, "DropPacketWithDst ");
        }
    }
    auto new_end = std::remove_if(m_queue.begin(), m_queue.end(), [&](const QueueEntry& en) {
        return en.GetIpv4Header().GetDestination() == dst;
    });
    m_queue.erase(new_end, m_queue.end());
}

bool
RequestQueue::Dequeue(Ipv4Address dst, QueueEntry& entry)
{
    Purge();
    for (auto i = m_queue.begin(); i != m_queue.end(); ++i)
    {
        if (i->GetIpv4Header().GetDestination() == dst)
        {
            entry = *i;
            m_queue.erase(i);
            return true;
        }
    }
    return false;
}

bool
RequestQueue::Find(Ipv4Address dst)
{
    for (auto i = m_queue.begin(); i != m_queue.end(); ++i)
    {
        if (i->GetIpv4Header().GetDestination() == dst)
        {
            return true;
        }
    }
    return false;
}

/**
 * @brief IsExpired structure
 */
struct IsExpired
{
    /**
     * Check if the entry is expired
     *
     * @param e QueueEntry entry
     * @return true if expired, false otherwise
     */
    bool operator()(const QueueEntry& e) const
    {
        return (e.GetExpireTime().IsStrictlyNegative());
    }
};

void
RequestQueue::Purge()
{
    IsExpired pred;
    for (auto i = m_queue.begin(); i != m_queue.end(); ++i)
    {
        if (pred(*i))
        {
            Drop(*i, "Drop outdated packet ");
        }
    }
    m_queue.erase(std::remove_if(m_queue.begin(), m_queue.end(), pred), m_queue.end());
}

void
RequestQueue::Drop(QueueEntry en, std::string reason)
{
    NS_LOG_LOGIC(reason << en.GetPacket()->GetUid() << " " << en.GetIpv4Header().GetDestination());
    en.GetErrorCallback()(en.GetPacket(), en.GetIpv4Header(), Socket::ERROR_NOROUTETOHOST);
}

} // namespace scr
} // namespace ns3
