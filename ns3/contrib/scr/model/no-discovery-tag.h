/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * SCR — NoDiscovery local send tag.
 *
 * Specification section 7:
 *   "add a NoDiscovery local send attribute/tag recognized by the SCR fork at
 *    source route output. It causes an error return if there is no valid route
 *    and must never enqueue a reverse-route RREQ. The feedback payload is still
 *    serialized and consumes real UDP/IP/Wi-Fi bytes. The receiver checks route
 *    usability before sending, but the routing guard is mandatory because the
 *    route can change between checking and actual output."
 *
 * This is a LOCAL tag only. It never crosses a node boundary and never carries
 * remote protocol information: it is attached by the local feedback application
 * and read by the local routing protocol on the same node. The specification
 * permits local diagnostic tags only where they cannot convey free remote
 * information, and this one cannot - it carries a single local intent bit.
 *
 * Traceability: M16. Validating fixture: T6.
 */

#ifndef SCR_NO_DISCOVERY_TAG_H
#define SCR_NO_DISCOVERY_TAG_H

#include "ns3/tag.h"

namespace ns3
{
namespace scr
{

/**
 * @ingroup scr
 * @brief Marks a locally originated packet as forbidden to trigger discovery.
 */
class NoDiscoveryTag : public Tag
{
  public:
    static TypeId GetTypeId();
    TypeId GetInstanceTypeId() const override;
    uint32_t GetSerializedSize() const override;
    void Serialize(TagBuffer i) const override;
    void Deserialize(TagBuffer i) override;
    void Print(std::ostream& os) const override;
};

} // namespace scr
} // namespace ns3

#endif /* SCR_NO_DISCOVERY_TAG_H */
