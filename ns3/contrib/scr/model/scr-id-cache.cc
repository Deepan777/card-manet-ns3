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

#include "scr-id-cache.h"

#include <algorithm>

namespace ns3
{
namespace scr
{
bool
IdCache::IsDuplicate(Ipv4Address addr, uint32_t id)
{
    Purge();
    for (auto i = m_idCache.begin(); i != m_idCache.end(); ++i)
    {
        if (i->m_context == addr && i->m_id == id)
        {
            return true;
        }
    }
    UniqueId uniqueId = {addr, id, m_lifetime + Simulator::Now()};
    m_idCache.push_back(uniqueId);
    return false;
}

void
IdCache::Purge()
{
    m_idCache.erase(remove_if(m_idCache.begin(), m_idCache.end(), IsExpired()), m_idCache.end());
}

uint32_t
IdCache::GetSize()
{
    Purge();
    return m_idCache.size();
}

} // namespace scr
} // namespace ns3
