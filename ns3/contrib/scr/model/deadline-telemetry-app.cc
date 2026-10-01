/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * SCR — deadline telemetry source application. Traceability M01 (E1).
 */

#include "deadline-telemetry-app.h"

#include "service-headers.h"

#include "ns3/inet-socket-address.h"
#include "ns3/log.h"
#include "ns3/packet.h"
#include "ns3/simulator.h"
#include "ns3/udp-socket-factory.h"
#include "ns3/uinteger.h"

namespace ns3
{
namespace scr
{

NS_LOG_COMPONENT_DEFINE("ScrDeadlineTelemetryApp");
NS_OBJECT_ENSURE_REGISTERED(DeadlineTelemetryApp);

namespace
{
/// Immediate socket-failure retry delay (specification section 6).
const Time kSendRetryDelay = MilliSeconds(100);
/// Maximum retries for an immediate socket failure before the flow is marked.
constexpr uint32_t kMaxSendRetries = 3;
} // namespace

TypeId
DeadlineTelemetryApp::GetTypeId()
{
    static TypeId tid =
        TypeId("ns3::scr::DeadlineTelemetryApp")
            .SetParent<Application>()
            .SetGroupName("Scr")
            .AddConstructor<DeadlineTelemetryApp>()
            .AddAttribute("PeerAddress",
                          "Destination IPv4 address",
                          Ipv4AddressValue(),
                          MakeIpv4AddressAccessor(&DeadlineTelemetryApp::m_peerAddress),
                          MakeIpv4AddressChecker())
            .AddAttribute("PeerPort",
                          "Destination UDP port",
                          UintegerValue(6000),
                          MakeUintegerAccessor(&DeadlineTelemetryApp::m_peerPort),
                          MakeUintegerChecker<uint16_t>())
            .AddAttribute("SessionId",
                          "Session identifier carried in every packet",
                          UintegerValue(0),
                          MakeUintegerAccessor(&DeadlineTelemetryApp::m_sessionId),
                          MakeUintegerChecker<uint64_t>())
            .AddAttribute("Interval",
                          "I, the fixed generation interval",
                          TimeValue(MilliSeconds(50)),
                          MakeTimeAccessor(&DeadlineTelemetryApp::m_interval),
                          MakeTimeChecker(NanoSeconds(1)))
            .AddAttribute("Deadline",
                          "D, the delivery deadline",
                          TimeValue(MilliSeconds(250)),
                          MakeTimeAccessor(&DeadlineTelemetryApp::m_deadline),
                          MakeTimeChecker())
            .AddAttribute("BlockSize",
                          "M, packets per evidence block (1..32)",
                          UintegerValue(10),
                          MakeUintegerAccessor(&DeadlineTelemetryApp::m_blockSize),
                          MakeUintegerChecker<uint32_t>(1, 32))
            .AddAttribute("PayloadBytes",
                          "Scientific payload, additional to the 56-byte header",
                          UintegerValue(512),
                          MakeUintegerAccessor(&DeadlineTelemetryApp::m_payloadBytes),
                          MakeUintegerChecker<uint32_t>())
            .AddTraceSource("Generate",
                            "A scheduled packet was generated",
                            MakeTraceSourceAccessor(&DeadlineTelemetryApp::m_generateTrace),
                            "ns3::scr::DeadlineTelemetryApp::GenerateTracedCallback");
    return tid;
}

DeadlineTelemetryApp::DeadlineTelemetryApp()
    : m_peerPort(6000),
      m_sessionId(0),
      m_interval(MilliSeconds(50)),
      m_deadline(MilliSeconds(250)),
      m_blockSize(10),
      m_payloadBytes(512),
      m_t0(Seconds(0)),
      m_sequence(0),
      m_generated(0),
      m_sendErrors(0),
      m_transportError(false),
      m_noRouteDrops(0),
      m_running(false)
{
}

DeadlineTelemetryApp::~DeadlineTelemetryApp()
{
}

void
DeadlineTelemetryApp::DoDispose()
{
    m_socket = nullptr;
    Application::DoDispose();
}

Time
DeadlineTelemetryApp::BlockFirstGenerationTime(uint64_t block) const
{
    // t0 + b*M*I
    return m_t0 + m_interval * static_cast<int64_t>(block * m_blockSize);
}

Time
DeadlineTelemetryApp::BlockLastGenerationTime(uint64_t block) const
{
    // e_b = t0 + ((b+1)M - 1) * I
    return m_t0 + m_interval * static_cast<int64_t>((block + 1) * m_blockSize - 1);
}

Time
DeadlineTelemetryApp::BlockCloseTime(uint64_t block) const
{
    // c_b = e_b + D
    return BlockLastGenerationTime(block) + m_deadline;
}

void
DeadlineTelemetryApp::StartApplication()
{
    NS_LOG_FUNCTION(this);

    if (!m_socket)
    {
        m_socket = Socket::CreateSocket(GetNode(), UdpSocketFactory::GetTypeId());
        m_socket->Bind();
        m_socket->Connect(InetSocketAddress(m_peerAddress, m_peerPort));
        // The source never receives on this socket: feedback arrives at the
        // ServiceEvidence receiver port, keeping data and evidence paths distinct.
        m_socket->SetRecvCallback(MakeNullCallback<void, Ptr<Socket>>());
    }

    // The schedule origin is fixed once, here. Every later generation time is
    // derived arithmetically from t0 rather than from "now", so accumulated
    // scheduling jitter cannot drift the schedule and silently change offered
    // demand between algorithms.
    m_t0 = Simulator::Now();
    m_sequence = 0;
    m_running = true;
    Generate();
}

void
DeadlineTelemetryApp::StopApplication()
{
    NS_LOG_FUNCTION(this);
    m_running = false;
    if (m_generateEvent.IsPending())
    {
        Simulator::Cancel(m_generateEvent);
    }
    if (m_socket)
    {
        m_socket->Close();
    }
}

void
DeadlineTelemetryApp::Generate()
{
    if (!m_running)
    {
        return;
    }

    const uint64_t seq = m_sequence;
    const Time now = Simulator::Now();

    TelemetryHeader hdr;
    hdr.SetSessionId(m_sessionId);
    hdr.SetSequence(seq);
    hdr.SetStartTime(m_t0);
    hdr.SetInterval(m_interval);
    hdr.SetDeadline(m_deadline);
    hdr.SetBlockSize(m_blockSize);
    hdr.SetPayloadBytes(m_payloadBytes);

    Ptr<Packet> packet = Create<Packet>(m_payloadBytes);
    packet->AddHeader(hdr);

    // E1: "Generation counts include every scheduled application packet even if
    // its socket send fails." The counter and trace are therefore updated BEFORE
    // the send is attempted, and are never rolled back on failure.
    ++m_generated;

    int sent = m_socket->Send(packet);
    const bool accepted = (sent >= 0);
    if (!accepted)
    {
        ++m_sendErrors;
        NS_LOG_WARN("session " << m_sessionId << " seq " << seq
                               << ": immediate socket failure, scheduling retry 1");
        Simulator::Schedule(kSendRetryDelay,
                            &DeadlineTelemetryApp::RetrySend,
                            this,
                            packet->Copy(),
                            seq,
                            1u);
    }

    m_generateTrace(m_sessionId, seq, now, accepted);

    ++m_sequence;
    // Next generation time derived from t0, not from now.
    Time next = m_t0 + m_interval * static_cast<int64_t>(m_sequence);
    Time delay = (next > now) ? (next - now) : Time(0);
    m_generateEvent = Simulator::Schedule(delay, &DeadlineTelemetryApp::Generate, this);
}

void
DeadlineTelemetryApp::RetrySend(Ptr<Packet> packet, uint64_t sequence, uint32_t attempt)
{
    if (!m_running)
    {
        return;
    }

    int sent = m_socket->Send(packet);
    if (sent >= 0)
    {
        NS_LOG_INFO("session " << m_sessionId << " seq " << sequence << ": retry " << attempt
                               << " succeeded");
        return;
    }

    if (attempt >= kMaxSendRetries)
    {
        // R017. "No route to host" is a ROUTING OUTCOME, not a harness fault.
        // A proactive protocol that has not converged, or any protocol during a
        // partition, legitimately has nowhere to send; the packet is lost and
        // must be scored as lost.
        //
        // Conflating the two was a selection bug with a direction. The analysis
        // excludes any run flagged with a transport integration error, so
        // treating no-route as a harness fault deleted exactly those runs in
        // which a protocol failed to provide a route -- and deleted them from
        // that protocol's own column. Measured on pilot seed 101: OLSR produced
        // 12,758 such failures in 43,132 packets (30%) and the whole run was
        // being dropped from the comparison, flattering OLSR by removing its
        // worst case. AODV-family protocols queue during discovery and so were
        // never affected, which is why this only ever hit one family.
        //
        // The flag is now reserved for what it was meant for: retries exhausted
        // for a reason other than the absence of a route.
        if (m_socket->GetErrno() == Socket::ERROR_NOROUTETOHOST)
        {
            ++m_noRouteDrops;
            NS_LOG_INFO("session " << m_sessionId << " seq " << sequence
                                   << ": no route after " << attempt
                                   << " retries; scored as loss, not a harness fault");
            return;
        }
        m_transportError = true;
        NS_LOG_ERROR("session " << m_sessionId << " seq " << sequence
                                << ": TRANSPORT INTEGRATION ERROR after " << attempt
                                << " retries; flow marked");
        return;
    }

    Simulator::Schedule(kSendRetryDelay,
                        &DeadlineTelemetryApp::RetrySend,
                        this,
                        packet->Copy(),
                        sequence,
                        attempt + 1);
}

} // namespace scr
} // namespace ns3
