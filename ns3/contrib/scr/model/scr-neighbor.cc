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


#include "scr-neighbor.h"

#include "ns3/log.h"
#include "ns3/wifi-mac-header.h"

#include <algorithm>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("ScrNeighbors");

namespace scr
{
Neighbors::Neighbors(Time delay)
    : m_ntimer(Timer::CANCEL_ON_DESTROY)
{
    m_ntimer.SetDelay(delay);
    m_ntimer.SetFunction(&Neighbors::Purge, this);
    m_txErrorCallback = MakeCallback(&Neighbors::ProcessTxError, this);
}

bool
Neighbors::IsNeighbor(Ipv4Address addr)
{
    Purge();
    for (auto i = m_nb.begin(); i != m_nb.end(); ++i)
    {
        if (i->m_neighborAddress == addr)
        {
            return true;
        }
    }
    return false;
}

Time
Neighbors::GetExpireTime(Ipv4Address addr)
{
    Purge();
    for (auto i = m_nb.begin(); i != m_nb.end(); ++i)
    {
        if (i->m_neighborAddress == addr)
        {
            return (i->m_expireTime - Simulator::Now());
        }
    }
    return Time(0);
}

void
Neighbors::Update(Ipv4Address addr, Time expire)
{
    for (auto i = m_nb.begin(); i != m_nb.end(); ++i)
    {
        if (i->m_neighborAddress == addr)
        {
            i->m_expireTime = std::max(expire + Simulator::Now(), i->m_expireTime);
            if (i->m_hardwareAddress == Mac48Address())
            {
                i->m_hardwareAddress = LookupMacAddress(i->m_neighborAddress);
            }
            return;
        }
    }

    NS_LOG_LOGIC("Open link to " << addr);
    Neighbor neighbor(addr, LookupMacAddress(addr), expire + Simulator::Now());
    m_nb.push_back(neighbor);
    Purge();
}

/**
 * @brief CloseNeighbor structure
 */
struct CloseNeighbor
{
    /**
     * Check if the entry is expired
     *
     * @param nb Neighbors::Neighbor entry
     * @return true if expired, false otherwise
     */
    bool operator()(const Neighbors::Neighbor& nb) const
    {
        return ((nb.m_expireTime < Simulator::Now()) || nb.close);
    }
};

void
Neighbors::Purge()
{
    if (m_nb.empty())
    {
        return;
    }

    CloseNeighbor pred;
    if (!m_handleLinkFailure.IsNull())
    {
        for (auto j = m_nb.begin(); j != m_nb.end(); ++j)
        {
            if (pred(*j))
            {
                NS_LOG_LOGIC("Close link to " << j->m_neighborAddress);
                m_handleLinkFailure(j->m_neighborAddress);
            }
        }
    }
    m_nb.erase(std::remove_if(m_nb.begin(), m_nb.end(), pred), m_nb.end());
    m_ntimer.Cancel();
    m_ntimer.Schedule();
}

void
Neighbors::ScheduleTimer()
{
    m_ntimer.Cancel();
    m_ntimer.Schedule();
}

void
Neighbors::AddArpCache(Ptr<ArpCache> a)
{
    m_arp.push_back(a);
}

void
Neighbors::DelArpCache(Ptr<ArpCache> a)
{
    m_arp.erase(std::remove(m_arp.begin(), m_arp.end(), a), m_arp.end());
}

Mac48Address
Neighbors::LookupMacAddress(Ipv4Address addr)
{
    Mac48Address hwaddr;
    for (auto i = m_arp.begin(); i != m_arp.end(); ++i)
    {
        ArpCache::Entry* entry = (*i)->Lookup(addr);
        if (entry != nullptr && (entry->IsAlive() || entry->IsPermanent()) && !entry->IsExpired())
        {
            hwaddr = Mac48Address::ConvertFrom(entry->GetMacAddress());
            break;
        }
    }
    return hwaddr;
}

void
Neighbors::ProcessTxError(const WifiMacHeader& hdr)
{
    Mac48Address addr = hdr.GetAddr1();

    for (auto i = m_nb.begin(); i != m_nb.end(); ++i)
    {
        if (i->m_hardwareAddress == addr)
        {
            i->close = true;
        }
    }
    Purge();
}

} // namespace scr
} // namespace ns3
