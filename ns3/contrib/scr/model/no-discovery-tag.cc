/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * SCR — NoDiscovery local send tag. Traceability M16.
 */

#include "no-discovery-tag.h"

namespace ns3
{
namespace scr
{

NS_OBJECT_ENSURE_REGISTERED(NoDiscoveryTag);

TypeId
NoDiscoveryTag::GetTypeId()
{
    static TypeId tid = TypeId("ns3::scr::NoDiscoveryTag")
                            .SetParent<Tag>()
                            .SetGroupName("Scr")
                            .AddConstructor<NoDiscoveryTag>();
    return tid;
}

TypeId
NoDiscoveryTag::GetInstanceTypeId() const
{
    return GetTypeId();
}

uint32_t
NoDiscoveryTag::GetSerializedSize() const
{
    // The tag carries no payload: its presence is the entire signal. It is
    // local-only and is never transmitted, so it adds no wire bytes and cannot
    // convey remote information.
    return 0;
}

void
NoDiscoveryTag::Serialize(TagBuffer) const
{
}

void
NoDiscoveryTag::Deserialize(TagBuffer)
{
}

void
NoDiscoveryTag::Print(std::ostream& os) const
{
    os << "NoDiscovery";
}

} // namespace scr
} // namespace ns3
