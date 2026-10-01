/*
 * Copyright (c) 2009 IITP RAS
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
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


#ifndef SCR_DPD_H
#define SCR_DPD_H

#include "scr-id-cache.h"

#include "ns3/ipv4-header.h"
#include "ns3/nstime.h"
#include "ns3/packet.h"

namespace ns3
{
namespace scr
{
/**
 * @ingroup scr
 *
 * @brief Helper class used to remember already seen packets and detect duplicates.
 *
 * Currently duplicate detection is based on unique packet ID given by Packet::GetUid ()
 * This approach is known to be weak (ns3::Packet UID is an internal identifier and not intended for
 * logical uniqueness in models) and should be changed.
 */
class DuplicatePacketDetection
{
  public:
    /**
     * Constructor
     * @param lifetime the lifetime for added entries
     */
    DuplicatePacketDetection(Time lifetime)
        : m_idCache(lifetime)
    {
    }

    /**
     * Check if the packet is a duplicate. If not, save information about this packet.
     * @param p the packet to check
     * @param header the IP header to check
     * @returns true if duplicate
     */
    bool IsDuplicate(Ptr<const Packet> p, const Ipv4Header& header);
    /**
     * Set duplicate record lifetime
     * @param lifetime the lifetime for duplicate records
     */
    void SetLifetime(Time lifetime);
    /**
     * Get duplicate record lifetime
     * @returns the duplicate record lifetime
     */
    Time GetLifetime() const;

  private:
    /// Impl
    IdCache m_idCache;
};

} // namespace scr
} // namespace ns3

#endif /* SCR_DPD_H */
