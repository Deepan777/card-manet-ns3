/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Service-Confirmed Repair (SCR) — serialized service headers.
 *
 * Specification section 8 ("Serialized messages and generated dataset"):
 *   - All integer fields use network byte order.
 *   - Time is serialized in nanoseconds.
 *   - Headers have checked Deserialize methods, exact GetSerializedSize tests,
 *     and version = 1.
 *   - Session ID is uint64; IPv4 addresses are uint32.
 *
 * Traceability: M04 (E2 serialized remote evidence). Validating fixture: T11.
 */

#ifndef SCR_SERVICE_HEADERS_H
#define SCR_SERVICE_HEADERS_H

#include "ns3/header.h"
#include "ns3/ipv4-address.h"
#include "ns3/nstime.h"

#include <cstdint>

namespace ns3
{
namespace scr
{

/// Wire format version carried by every SCR service header.
constexpr uint8_t SCR_SERVICE_VERSION = 1;

/// Message type discriminators.
enum ServiceMessageType : uint8_t
{
    SCR_MSG_TELEMETRY = 1, //!< source -> destination data with schedule
    SCR_MSG_RECEIPT = 2,   //!< destination -> source per-block receipt
};

/**
 * @ingroup scr
 * @brief Telemetry data header. Exactly 56 bytes on the wire.
 *
 * Field layout (specification section 8):
 *   version uint8, type uint8, reserved uint16, session_id uint64,
 *   sequence uint64, start_time_ns uint64, interval_ns uint64,
 *   deadline_ns uint64, block_size uint32, payload_bytes uint32,
 *   reserved2 uint32
 *   => 1+1+2 + 8+8+8+8+8 + 4+4+4 = 56
 *
 * The 512-byte scientific payload is ADDITIONAL to this header. Every algorithm
 * under comparison carries the same data header, so the comparison is not
 * distorted by header-size differences.
 */
class TelemetryHeader : public Header
{
  public:
    TelemetryHeader();

    static TypeId GetTypeId();
    TypeId GetInstanceTypeId() const override;
    uint32_t GetSerializedSize() const override;
    void Serialize(Buffer::Iterator start) const override;
    uint32_t Deserialize(Buffer::Iterator start) override;
    void Print(std::ostream& os) const override;

    /// Wire size in bytes. Compile-time constant asserted by T11.
    static constexpr uint32_t WIRE_SIZE = 56;

    void SetSessionId(uint64_t v) { m_sessionId = v; }
    uint64_t GetSessionId() const { return m_sessionId; }

    void SetSequence(uint64_t v) { m_sequence = v; }
    uint64_t GetSequence() const { return m_sequence; }

    void SetStartTime(Time v) { m_startTimeNs = static_cast<uint64_t>(v.GetNanoSeconds()); }
    Time GetStartTime() const { return NanoSeconds(m_startTimeNs); }

    void SetInterval(Time v) { m_intervalNs = static_cast<uint64_t>(v.GetNanoSeconds()); }
    Time GetInterval() const { return NanoSeconds(m_intervalNs); }

    void SetDeadline(Time v) { m_deadlineNs = static_cast<uint64_t>(v.GetNanoSeconds()); }
    Time GetDeadline() const { return NanoSeconds(m_deadlineNs); }

    void SetBlockSize(uint32_t v) { m_blockSize = v; }
    uint32_t GetBlockSize() const { return m_blockSize; }

    void SetPayloadBytes(uint32_t v) { m_payloadBytes = v; }
    uint32_t GetPayloadBytes() const { return m_payloadBytes; }

    /// True if the last Deserialize accepted the input.
    bool IsValid() const { return m_valid; }

  private:
    uint8_t m_version;
    uint8_t m_type;
    uint16_t m_reserved;
    uint64_t m_sessionId;
    uint64_t m_sequence;
    uint64_t m_startTimeNs;
    uint64_t m_intervalNs;
    uint64_t m_deadlineNs;
    uint32_t m_blockSize;
    uint32_t m_payloadBytes;
    uint32_t m_reserved2;
    bool m_valid;
};

/**
 * @ingroup scr
 * @brief Per-block service receipt header. Exactly 44 bytes on the wire.
 *
 * Field layout (specification section 8):
 *   version uint8, type uint8, reserved uint16, session_id uint64,
 *   block_id uint64, close_time_ns uint64, ontime_mask uint32,
 *   block_size uint32, source_ipv4 uint32, destination_ipv4 uint32
 *   => 1+1+2 + 8+8+8 + 4+4+4+4 = 44
 *
 * The sender already owns the schedule and validates close_time against it.
 * UDP/IP/MAC overhead is additional and is charged separately as real bytes.
 */
class ServiceReceiptHeader : public Header
{
  public:
    ServiceReceiptHeader();

    static TypeId GetTypeId();
    TypeId GetInstanceTypeId() const override;
    uint32_t GetSerializedSize() const override;
    void Serialize(Buffer::Iterator start) const override;
    uint32_t Deserialize(Buffer::Iterator start) override;
    void Print(std::ostream& os) const override;

    static constexpr uint32_t WIRE_SIZE = 44;

    /// Maximum block size representable by the 32-bit on-time mask.
    static constexpr uint32_t MAX_BLOCK_SIZE = 32;

    void SetSessionId(uint64_t v) { m_sessionId = v; }
    uint64_t GetSessionId() const { return m_sessionId; }

    void SetBlockId(uint64_t v) { m_blockId = v; }
    uint64_t GetBlockId() const { return m_blockId; }

    void SetCloseTime(Time v) { m_closeTimeNs = static_cast<uint64_t>(v.GetNanoSeconds()); }
    Time GetCloseTime() const { return NanoSeconds(m_closeTimeNs); }

    void SetOntimeMask(uint32_t v) { m_ontimeMask = v; }
    uint32_t GetOntimeMask() const { return m_ontimeMask; }

    void SetBlockSize(uint32_t v) { m_blockSize = v; }
    uint32_t GetBlockSize() const { return m_blockSize; }

    void SetSource(Ipv4Address v) { m_source = v.Get(); }
    Ipv4Address GetSource() const { return Ipv4Address(m_source); }

    void SetDestination(Ipv4Address v) { m_destination = v.Get(); }
    Ipv4Address GetDestination() const { return Ipv4Address(m_destination); }

    /// Number of on-time packets indicated by the mask, restricted to block_size.
    uint32_t CountOntime() const;

    bool IsValid() const { return m_valid; }

  private:
    uint8_t m_version;
    uint8_t m_type;
    uint16_t m_reserved;
    uint64_t m_sessionId;
    uint64_t m_blockId;
    uint64_t m_closeTimeNs;
    uint32_t m_ontimeMask;
    uint32_t m_blockSize;
    uint32_t m_source;
    uint32_t m_destination;
    bool m_valid;
};

} // namespace scr
} // namespace ns3

#endif /* SCR_SERVICE_HEADERS_H */
