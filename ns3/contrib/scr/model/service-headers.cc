/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Service-Confirmed Repair (SCR) — serialized service headers.
 * Traceability: M04. Validating fixture: T11.
 */

#include "service-headers.h"

#include "ns3/log.h"

#include <bitset>

namespace ns3
{
namespace scr
{

NS_LOG_COMPONENT_DEFINE("ScrServiceHeaders");

// Compile-time proof that the declared wire sizes equal the sum of the fields.
// If a field is added or resized, the build fails here rather than silently
// producing a different wire format than the specification records.
static_assert(TelemetryHeader::WIRE_SIZE ==
                  sizeof(uint8_t) + sizeof(uint8_t) + sizeof(uint16_t) + sizeof(uint64_t) * 5 +
                      sizeof(uint32_t) * 3,
              "TelemetryHeader field sum must be exactly 56 bytes");

static_assert(ServiceReceiptHeader::WIRE_SIZE ==
                  sizeof(uint8_t) + sizeof(uint8_t) + sizeof(uint16_t) + sizeof(uint64_t) * 3 +
                      sizeof(uint32_t) * 4,
              "ServiceReceiptHeader field sum must be exactly 44 bytes");

// ---------------------------------------------------------------------------
// TelemetryHeader
// ---------------------------------------------------------------------------

NS_OBJECT_ENSURE_REGISTERED(TelemetryHeader);

TelemetryHeader::TelemetryHeader()
    : m_version(SCR_SERVICE_VERSION),
      m_type(SCR_MSG_TELEMETRY),
      m_reserved(0),
      m_sessionId(0),
      m_sequence(0),
      m_startTimeNs(0),
      m_intervalNs(0),
      m_deadlineNs(0),
      m_blockSize(0),
      m_payloadBytes(0),
      m_reserved2(0),
      m_valid(true)
{
}

TypeId
TelemetryHeader::GetTypeId()
{
    static TypeId tid = TypeId("ns3::scr::TelemetryHeader")
                            .SetParent<Header>()
                            .SetGroupName("Scr")
                            .AddConstructor<TelemetryHeader>();
    return tid;
}

TypeId
TelemetryHeader::GetInstanceTypeId() const
{
    return GetTypeId();
}

uint32_t
TelemetryHeader::GetSerializedSize() const
{
    return WIRE_SIZE;
}

void
TelemetryHeader::Serialize(Buffer::Iterator start) const
{
    // Network byte order throughout (specification section 8).
    start.WriteU8(m_version);
    start.WriteU8(m_type);
    start.WriteHtonU16(m_reserved);
    start.WriteHtonU64(m_sessionId);
    start.WriteHtonU64(m_sequence);
    start.WriteHtonU64(m_startTimeNs);
    start.WriteHtonU64(m_intervalNs);
    start.WriteHtonU64(m_deadlineNs);
    start.WriteHtonU32(m_blockSize);
    start.WriteHtonU32(m_payloadBytes);
    start.WriteHtonU32(m_reserved2);
}

uint32_t
TelemetryHeader::Deserialize(Buffer::Iterator start)
{
    Buffer::Iterator i = start;
    m_valid = false;

    m_version = i.ReadU8();
    m_type = i.ReadU8();

    // Checked deserialization: reject anything that is not a version-1 telemetry
    // header. Returning 0 is the ns-3 convention for "not consumed / invalid",
    // and callers must treat a 0 return as a malformed message rather than
    // silently accepting default-constructed fields.
    if (m_version != SCR_SERVICE_VERSION || m_type != SCR_MSG_TELEMETRY)
    {
        NS_LOG_WARN("TelemetryHeader rejected: version=" << +m_version << " type=" << +m_type);
        return 0;
    }

    m_reserved = i.ReadNtohU16();
    m_sessionId = i.ReadNtohU64();
    m_sequence = i.ReadNtohU64();
    m_startTimeNs = i.ReadNtohU64();
    m_intervalNs = i.ReadNtohU64();
    m_deadlineNs = i.ReadNtohU64();
    m_blockSize = i.ReadNtohU32();
    m_payloadBytes = i.ReadNtohU32();
    m_reserved2 = i.ReadNtohU32();

    uint32_t consumed = i.GetDistanceFrom(start);
    NS_ASSERT_MSG(consumed == WIRE_SIZE, "TelemetryHeader consumed unexpected byte count");

    m_valid = true;
    return consumed;
}

void
TelemetryHeader::Print(std::ostream& os) const
{
    os << "TelemetryHeader[v=" << +m_version << " session=" << m_sessionId
       << " seq=" << m_sequence << " start=" << m_startTimeNs << "ns"
       << " interval=" << m_intervalNs << "ns"
       << " deadline=" << m_deadlineNs << "ns"
       << " blockSize=" << m_blockSize << " payload=" << m_payloadBytes << "B]";
}

// ---------------------------------------------------------------------------
// ServiceReceiptHeader
// ---------------------------------------------------------------------------

NS_OBJECT_ENSURE_REGISTERED(ServiceReceiptHeader);

ServiceReceiptHeader::ServiceReceiptHeader()
    : m_version(SCR_SERVICE_VERSION),
      m_type(SCR_MSG_RECEIPT),
      m_reserved(0),
      m_sessionId(0),
      m_blockId(0),
      m_closeTimeNs(0),
      m_ontimeMask(0),
      m_blockSize(0),
      m_source(0),
      m_destination(0),
      m_valid(true)
{
}

TypeId
ServiceReceiptHeader::GetTypeId()
{
    static TypeId tid = TypeId("ns3::scr::ServiceReceiptHeader")
                            .SetParent<Header>()
                            .SetGroupName("Scr")
                            .AddConstructor<ServiceReceiptHeader>();
    return tid;
}

TypeId
ServiceReceiptHeader::GetInstanceTypeId() const
{
    return GetTypeId();
}

uint32_t
ServiceReceiptHeader::GetSerializedSize() const
{
    return WIRE_SIZE;
}

void
ServiceReceiptHeader::Serialize(Buffer::Iterator start) const
{
    start.WriteU8(m_version);
    start.WriteU8(m_type);
    start.WriteHtonU16(m_reserved);
    start.WriteHtonU64(m_sessionId);
    start.WriteHtonU64(m_blockId);
    start.WriteHtonU64(m_closeTimeNs);
    start.WriteHtonU32(m_ontimeMask);
    start.WriteHtonU32(m_blockSize);
    start.WriteHtonU32(m_source);
    start.WriteHtonU32(m_destination);
}

uint32_t
ServiceReceiptHeader::Deserialize(Buffer::Iterator start)
{
    Buffer::Iterator i = start;
    m_valid = false;

    m_version = i.ReadU8();
    m_type = i.ReadU8();

    if (m_version != SCR_SERVICE_VERSION || m_type != SCR_MSG_RECEIPT)
    {
        NS_LOG_WARN("ServiceReceiptHeader rejected: version=" << +m_version
                                                              << " type=" << +m_type);
        return 0;
    }

    m_reserved = i.ReadNtohU16();
    m_sessionId = i.ReadNtohU64();
    m_blockId = i.ReadNtohU64();
    m_closeTimeNs = i.ReadNtohU64();
    m_ontimeMask = i.ReadNtohU32();
    m_blockSize = i.ReadNtohU32();
    m_source = i.ReadNtohU32();
    m_destination = i.ReadNtohU32();

    // A block larger than the mask width cannot be represented. Reject rather
    // than silently truncating, which would understate delivered service and
    // corrupt the E2 confirmation decision.
    if (m_blockSize > MAX_BLOCK_SIZE)
    {
        NS_LOG_WARN("ServiceReceiptHeader rejected: blockSize=" << m_blockSize << " exceeds "
                                                                << MAX_BLOCK_SIZE);
        return 0;
    }

    uint32_t consumed = i.GetDistanceFrom(start);
    NS_ASSERT_MSG(consumed == WIRE_SIZE, "ServiceReceiptHeader consumed unexpected byte count");

    m_valid = true;
    return consumed;
}

uint32_t
ServiceReceiptHeader::CountOntime() const
{
    if (m_blockSize == 0 || m_blockSize > MAX_BLOCK_SIZE)
    {
        return 0;
    }
    // Only the low block_size bits are meaningful; bits beyond the block are
    // ignored rather than counted, so a malformed mask cannot inflate service.
    uint32_t mask = (m_blockSize == 32) ? 0xFFFFFFFFu : ((1u << m_blockSize) - 1u);
    return static_cast<uint32_t>(std::bitset<32>(m_ontimeMask & mask).count());
}

void
ServiceReceiptHeader::Print(std::ostream& os) const
{
    os << "ServiceReceiptHeader[v=" << +m_version << " session=" << m_sessionId
       << " block=" << m_blockId << " close=" << m_closeTimeNs << "ns"
       << " mask=0x" << std::hex << m_ontimeMask << std::dec << " blockSize=" << m_blockSize
       << " ontime=" << CountOntime() << " src=" << Ipv4Address(m_source)
       << " dst=" << Ipv4Address(m_destination) << "]";
}

} // namespace scr
} // namespace ns3
