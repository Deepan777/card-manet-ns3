/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * T11 — receipt serialization and exact wire cost.
 *
 * Specification section 8 requires: exact GetSerializedSize (56 / 44 bytes),
 * network byte order, nanosecond time, checked Deserialize, version = 1.
 *
 * This fixture asserts the wire sizes against literal constants rather than
 * against the class's own WIRE_SIZE, so that changing the constant cannot make
 * the test agree with a wrong implementation.
 */

#include "ns3/packet.h"
#include "ns3/service-headers.h"
#include "ns3/test.h"

using namespace ns3;
using namespace ns3::scr;

namespace
{

/// Exact serialized size of the telemetry data header, per specification.
constexpr uint32_t kExpectedTelemetryBytes = 56;
/// Exact serialized size of the service receipt header, per specification.
constexpr uint32_t kExpectedReceiptBytes = 44;

} // namespace

// ---------------------------------------------------------------------------

class TelemetryHeaderRoundTripTest : public TestCase
{
  public:
    TelemetryHeaderRoundTripTest()
        : TestCase("TelemetryHeader: exact 56-byte size and lossless round trip")
    {
    }

  private:
    void DoRun() override
    {
        TelemetryHeader out;
        out.SetSessionId(0x0123456789ABCDEFull);
        out.SetSequence(0xFEDCBA9876543210ull);
        out.SetStartTime(NanoSeconds(20123456789ull));
        out.SetInterval(NanoSeconds(50000000ull)); // 20 packets/s
        out.SetDeadline(MilliSeconds(250));
        out.SetBlockSize(10);
        out.SetPayloadBytes(512);

        NS_TEST_ASSERT_MSG_EQ(out.GetSerializedSize(),
                              kExpectedTelemetryBytes,
                              "TelemetryHeader must serialize to exactly 56 bytes");

        Ptr<Packet> p = Create<Packet>(0);
        p->AddHeader(out);
        NS_TEST_ASSERT_MSG_EQ(p->GetSize(),
                              kExpectedTelemetryBytes,
                              "packet carrying only the data header must be 56 bytes");

        TelemetryHeader in;
        uint32_t consumed = p->RemoveHeader(in);
        NS_TEST_ASSERT_MSG_EQ(consumed, kExpectedTelemetryBytes, "must consume exactly 56 bytes");
        NS_TEST_ASSERT_MSG_EQ(in.IsValid(), true, "round-tripped header must be valid");

        NS_TEST_ASSERT_MSG_EQ(in.GetSessionId(), out.GetSessionId(), "session_id round trip");
        NS_TEST_ASSERT_MSG_EQ(in.GetSequence(), out.GetSequence(), "sequence round trip");
        NS_TEST_ASSERT_MSG_EQ(in.GetStartTime(), out.GetStartTime(), "start_time round trip");
        NS_TEST_ASSERT_MSG_EQ(in.GetInterval(), out.GetInterval(), "interval round trip");
        NS_TEST_ASSERT_MSG_EQ(in.GetDeadline(), out.GetDeadline(), "deadline round trip");
        NS_TEST_ASSERT_MSG_EQ(in.GetBlockSize(), out.GetBlockSize(), "block_size round trip");
        NS_TEST_ASSERT_MSG_EQ(in.GetPayloadBytes(),
                              out.GetPayloadBytes(),
                              "payload_bytes round trip");
        NS_TEST_ASSERT_MSG_EQ(p->GetSize(), 0, "no bytes left after removing the header");
    }
};

// ---------------------------------------------------------------------------

class ReceiptHeaderRoundTripTest : public TestCase
{
  public:
    ReceiptHeaderRoundTripTest()
        : TestCase("ServiceReceiptHeader: exact 44-byte size and lossless round trip")
    {
    }

  private:
    void DoRun() override
    {
        ServiceReceiptHeader out;
        out.SetSessionId(0x1122334455667788ull);
        out.SetBlockId(42);
        out.SetCloseTime(NanoSeconds(123456789012345ull));
        out.SetOntimeMask(0x000003FFu); // low 10 bits set
        out.SetBlockSize(10);
        out.SetSource(Ipv4Address("10.1.0.7"));
        out.SetDestination(Ipv4Address("10.1.0.53"));

        NS_TEST_ASSERT_MSG_EQ(out.GetSerializedSize(),
                              kExpectedReceiptBytes,
                              "ServiceReceiptHeader must serialize to exactly 44 bytes");
        NS_TEST_ASSERT_MSG_EQ(out.CountOntime(), 10, "all ten block packets marked on time");

        Ptr<Packet> p = Create<Packet>(0);
        p->AddHeader(out);
        NS_TEST_ASSERT_MSG_EQ(p->GetSize(), kExpectedReceiptBytes, "receipt packet is 44 bytes");

        ServiceReceiptHeader in;
        uint32_t consumed = p->RemoveHeader(in);
        NS_TEST_ASSERT_MSG_EQ(consumed, kExpectedReceiptBytes, "must consume exactly 44 bytes");
        NS_TEST_ASSERT_MSG_EQ(in.IsValid(), true, "round-tripped receipt must be valid");

        NS_TEST_ASSERT_MSG_EQ(in.GetSessionId(), out.GetSessionId(), "session_id round trip");
        NS_TEST_ASSERT_MSG_EQ(in.GetBlockId(), out.GetBlockId(), "block_id round trip");
        NS_TEST_ASSERT_MSG_EQ(in.GetCloseTime(), out.GetCloseTime(), "close_time round trip");
        NS_TEST_ASSERT_MSG_EQ(in.GetOntimeMask(), out.GetOntimeMask(), "ontime_mask round trip");
        NS_TEST_ASSERT_MSG_EQ(in.GetBlockSize(), out.GetBlockSize(), "block_size round trip");
        NS_TEST_ASSERT_MSG_EQ(in.GetSource(), out.GetSource(), "source address round trip");
        NS_TEST_ASSERT_MSG_EQ(in.GetDestination(),
                              out.GetDestination(),
                              "destination address round trip");
        NS_TEST_ASSERT_MSG_EQ(in.CountOntime(), 10, "on-time count survives the round trip");
    }
};

// ---------------------------------------------------------------------------

class ServiceHeaderNetworkByteOrderTest : public TestCase
{
  public:
    ServiceHeaderNetworkByteOrderTest()
        : TestCase("Service headers: fields are written in network byte order")
    {
    }

  private:
    void DoRun() override
    {
        ServiceReceiptHeader h;
        h.SetSessionId(0x0102030405060708ull);
        h.SetBlockSize(1);

        Ptr<Packet> p = Create<Packet>(0);
        p->AddHeader(h);

        uint8_t raw[kExpectedReceiptBytes];
        p->CopyData(raw, kExpectedReceiptBytes);

        // byte 0 version, byte 1 type, bytes 2-3 reserved, bytes 4-11 session_id
        NS_TEST_ASSERT_MSG_EQ(raw[0], 1, "version byte is 1");
        NS_TEST_ASSERT_MSG_EQ(raw[1], SCR_MSG_RECEIPT, "type byte is receipt");
        // Big-endian: most significant byte first.
        NS_TEST_ASSERT_MSG_EQ(raw[4], 0x01, "session_id MSB first (network byte order)");
        NS_TEST_ASSERT_MSG_EQ(raw[5], 0x02, "session_id byte 1");
        NS_TEST_ASSERT_MSG_EQ(raw[10], 0x07, "session_id byte 6");
        NS_TEST_ASSERT_MSG_EQ(raw[11], 0x08, "session_id LSB last");
    }
};

// ---------------------------------------------------------------------------

class ServiceHeaderMalformedInputTest : public TestCase
{
  public:
    ServiceHeaderMalformedInputTest()
        : TestCase("Service headers: checked Deserialize rejects malformed input")
    {
    }

  private:
    void DoRun() override
    {
        // 1) Wrong version must be rejected.
        {
            ServiceReceiptHeader good;
            good.SetBlockSize(4);
            Ptr<Packet> p = Create<Packet>(0);
            p->AddHeader(good);

            uint8_t raw[kExpectedReceiptBytes];
            p->CopyData(raw, kExpectedReceiptBytes);
            raw[0] = 99; // corrupt version
            Ptr<Packet> bad = Create<Packet>(raw, kExpectedReceiptBytes);

            ServiceReceiptHeader in;
            uint32_t consumed = bad->RemoveHeader(in);
            NS_TEST_ASSERT_MSG_EQ(consumed, 0, "wrong version must be rejected (0 consumed)");
            NS_TEST_ASSERT_MSG_EQ(in.IsValid(), false, "rejected header must report invalid");
        }

        // 2) Wrong message type must be rejected: a telemetry header must not be
        //    accepted as a receipt. This matters because accepting one as the
        //    other would fabricate service evidence.
        {
            TelemetryHeader t;
            t.SetBlockSize(10);
            Ptr<Packet> p = Create<Packet>(0);
            p->AddHeader(t);

            uint8_t raw[kExpectedTelemetryBytes];
            p->CopyData(raw, kExpectedTelemetryBytes);
            Ptr<Packet> mis = Create<Packet>(raw, kExpectedTelemetryBytes);

            ServiceReceiptHeader in;
            uint32_t consumed = mis->RemoveHeader(in);
            NS_TEST_ASSERT_MSG_EQ(consumed, 0, "telemetry must not deserialize as a receipt");
            NS_TEST_ASSERT_MSG_EQ(in.IsValid(), false, "type mismatch must report invalid");
        }

        // 3) block_size beyond the 32-bit mask width must be rejected rather
        //    than silently truncated.
        {
            ServiceReceiptHeader over;
            over.SetBlockSize(33);
            Ptr<Packet> p = Create<Packet>(0);
            p->AddHeader(over);

            ServiceReceiptHeader in;
            uint32_t consumed = p->RemoveHeader(in);
            NS_TEST_ASSERT_MSG_EQ(consumed, 0, "block_size > 32 must be rejected");
            NS_TEST_ASSERT_MSG_EQ(in.IsValid(), false, "oversized block must report invalid");
        }
    }
};

// ---------------------------------------------------------------------------

class ServiceHeaderOntimeMaskTest : public TestCase
{
  public:
    ServiceHeaderOntimeMaskTest()
        : TestCase("ServiceReceiptHeader: on-time mask is counted only within the block")
    {
    }

  private:
    void DoRun() override
    {
        ServiceReceiptHeader h;
        h.SetBlockSize(10);

        h.SetOntimeMask(0x00000000u);
        NS_TEST_ASSERT_MSG_EQ(h.CountOntime(), 0, "empty mask means zero on-time");

        h.SetOntimeMask(0x00000005u); // bits 0 and 2
        NS_TEST_ASSERT_MSG_EQ(h.CountOntime(), 2, "counts exactly the set bits in the block");

        // Bits above block_size must NOT be counted. Counting them would let a
        // malformed or hostile receipt inflate apparent service and wrongly
        // satisfy the E2 confirmation rule.
        h.SetOntimeMask(0xFFFFFFFFu);
        NS_TEST_ASSERT_MSG_EQ(h.CountOntime(), 10, "bits beyond block_size are ignored");

        h.SetBlockSize(32);
        h.SetOntimeMask(0xFFFFFFFFu);
        NS_TEST_ASSERT_MSG_EQ(h.CountOntime(), 32, "full 32-bit block counts all bits");

        h.SetBlockSize(0);
        NS_TEST_ASSERT_MSG_EQ(h.CountOntime(), 0, "zero-size block yields zero on-time");
    }
};

// ---------------------------------------------------------------------------

class ScrServiceHeadersTestSuite : public TestSuite
{
  public:
    ScrServiceHeadersTestSuite()
        : TestSuite("scr-service-headers", Type::UNIT)
    {
        AddTestCase(new TelemetryHeaderRoundTripTest, TestCase::Duration::QUICK);
        AddTestCase(new ReceiptHeaderRoundTripTest, TestCase::Duration::QUICK);
        AddTestCase(new ServiceHeaderNetworkByteOrderTest, TestCase::Duration::QUICK);
        AddTestCase(new ServiceHeaderMalformedInputTest, TestCase::Duration::QUICK);
        AddTestCase(new ServiceHeaderOntimeMaskTest, TestCase::Duration::QUICK);
    }
};

static ScrServiceHeadersTestSuite g_scrServiceHeadersTestSuite;
