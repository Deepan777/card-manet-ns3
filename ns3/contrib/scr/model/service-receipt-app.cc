/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * SCR — destination service receipt application. Traceability M02, M03 (E1/E2).
 */

#include "service-receipt-app.h"

#include "no-discovery-tag.h"
#include "service-headers.h"

#include "ns3/boolean.h"
#include "ns3/inet-socket-address.h"
#include "ns3/ipv4-routing-protocol.h"
#include "ns3/ipv4.h"
#include "ns3/log.h"
#include "ns3/packet.h"
#include "ns3/simulator.h"
#include "ns3/udp-socket-factory.h"
#include "ns3/uinteger.h"

namespace ns3
{
namespace scr
{

NS_LOG_COMPONENT_DEFINE("ScrServiceReceiptApp");
NS_OBJECT_ENSURE_REGISTERED(ServiceReceiptApp);

namespace
{
/**
 * E2 closure offset.
 *
 * The specification requires the block to close "just after all same-time
 * receive callbacks at c_b", implemented "using a one-nanosecond closure offset".
 *
 * Why this matters: a packet whose arrival is exactly at the deadline boundary
 * (r_p - g_p == D) is ON TIME by E1, and its receive callback executes at
 * simulation time c_b. Closing the block at exactly c_b would race that
 * callback and could drop a legitimately on-time packet from the mask,
 * understating delivered service and biasing every SCR confirmation decision.
 * The +1 ns offset makes the ordering deterministic and inclusive.
 */
const Time kClosureOffset = NanoSeconds(1);
} // namespace

TypeId
ServiceReceiptApp::GetTypeId()
{
    static TypeId tid =
        TypeId("ns3::scr::ServiceReceiptApp")
            .SetParent<Application>()
            .SetGroupName("Scr")
            .AddConstructor<ServiceReceiptApp>()
            .AddAttribute("Port",
                          "UDP port on which telemetry is received",
                          UintegerValue(6000),
                          MakeUintegerAccessor(&ServiceReceiptApp::m_port),
                          MakeUintegerChecker<uint16_t>())
            .AddAttribute("ReportPort",
                          "UDP port to which receipts are sent at the source",
                          UintegerValue(6001),
                          MakeUintegerAccessor(&ServiceReceiptApp::m_reportPort),
                          MakeUintegerChecker<uint16_t>())
            .AddAttribute("IgnoreDeadline",
                          "NO-DEADLINE ablation: score an evidence block on arrival "
                          "alone, ignoring the deadline. The measured endpoint "
                          "(ontime_true) is unaffected.",
                          BooleanValue(false),
                          MakeBooleanAccessor(&ServiceReceiptApp::m_ignoreDeadline),
                          MakeBooleanChecker())
            .AddAttribute("ClockOffset",
                          "C3: error in the receiver clock used for deadline "
                          "decisions. True simulator time is still recorded, so "
                          "observed and true decisions can be compared.",
                          TimeValue(Seconds(0)),
                          MakeTimeAccessor(&ServiceReceiptApp::m_clockOffset),
                          MakeTimeChecker())
            .AddTraceSource("Rx",
                            "A telemetry packet arrived for the first time",
                            MakeTraceSourceAccessor(&ServiceReceiptApp::m_rxTrace),
                            "ns3::scr::ServiceReceiptApp::RxTracedCallback")
            .AddTraceSource("Receipt",
                            "A block was closed and a receipt was emitted or suppressed",
                            MakeTraceSourceAccessor(&ServiceReceiptApp::m_receiptTrace),
                            "ns3::scr::ServiceReceiptApp::ReceiptTracedCallback");
    return tid;
}

ServiceReceiptApp::ServiceReceiptApp()
    : m_port(6000),
      m_reportPort(6001),
      m_haveSource(false),
      m_received(0),
      m_duplicates(0),
      m_late(0),
      m_clockOffset(Seconds(0)),
      m_ignoreDeadline(false),
      m_ontimeObserved(0),
      m_ontimeTrue(0),
      m_clockDisagreements(0),
      m_reportsSent(0),
      m_reportsNoRoute(0),
      m_malformed(0)
{
}

ServiceReceiptApp::~ServiceReceiptApp()
{
}

void
ServiceReceiptApp::DoDispose()
{
    m_socket = nullptr;
    m_reportSocket = nullptr;
    Application::DoDispose();
}

void
ServiceReceiptApp::StartApplication()
{
    NS_LOG_FUNCTION(this);
    if (!m_socket)
    {
        m_socket = Socket::CreateSocket(GetNode(), UdpSocketFactory::GetTypeId());
        m_socket->Bind(InetSocketAddress(Ipv4Address::GetAny(), m_port));
        m_socket->SetRecvCallback(MakeCallback(&ServiceReceiptApp::HandleRead, this));
    }
    if (!m_reportSocket)
    {
        m_reportSocket = Socket::CreateSocket(GetNode(), UdpSocketFactory::GetTypeId());
        m_reportSocket->Bind();
    }
}

void
ServiceReceiptApp::StopApplication()
{
    NS_LOG_FUNCTION(this);
    for (auto& kv : m_blocks)
    {
        if (kv.second.closureEvent.IsPending())
        {
            Simulator::Cancel(kv.second.closureEvent);
        }
    }
    if (m_socket)
    {
        m_socket->Close();
    }
    if (m_reportSocket)
    {
        m_reportSocket->Close();
    }
}

void
ServiceReceiptApp::HandleRead(Ptr<Socket> socket)
{
    Ptr<Packet> packet;
    Address from;
    while ((packet = socket->RecvFrom(from)))
    {
        TelemetryHeader hdr;
        uint32_t consumed = packet->RemoveHeader(hdr);
        if (consumed == 0 || !hdr.IsValid())
        {
            // Checked deserialization rejected the message. Count it; never
            // guess at the intended contents.
            ++m_malformed;
            NS_LOG_WARN("malformed telemetry header rejected");
            continue;
        }

        if (InetSocketAddress::IsMatchingType(from))
        {
            m_sourceAddress = InetSocketAddress::ConvertFrom(from).GetIpv4();
            m_haveSource = true;
        }

        const uint32_t M = hdr.GetBlockSize();
        if (M == 0 || M > ServiceReceiptHeader::MAX_BLOCK_SIZE)
        {
            ++m_malformed;
            NS_LOG_WARN("telemetry block size out of range: " << M);
            continue;
        }

        const uint64_t seq = hdr.GetSequence();
        const uint64_t blockId = seq / M;
        const uint32_t idx = static_cast<uint32_t>(seq % M);

        // g_p = t0 + p*I, derived from the serialized schedule.
        const Time gen = hdr.GetStartTime() + hdr.GetInterval() * static_cast<int64_t>(seq);
        const Time now = Simulator::Now();
        const Time delay = now - gen;

        BlockState& bs = m_blocks[blockId];
        if (bs.blockSize == 0)
        {
            bs.blockSize = M;
            bs.sessionId = hdr.GetSessionId();
        }

        const uint64_t bit = (1ull << idx);
        if (bs.seenMask & bit)
        {
            // E1: duplicate arrivals never increase y_p.
            ++m_duplicates;
            NS_LOG_DEBUG("duplicate seq " << seq << " ignored");
            continue;
        }
        bs.seenMask |= bit;
        ++m_received;

        // M19/R024: trace EVERY first arrival, here, before any branch can skip
        // it. The first version fired this after the closed-block exit below,
        // so packets arriving after their block was finalised were counted in
        // m_received but never traced: 1,038 of 6,964 arrivals went missing in
        // a 120 s test. Those are by construction the SLOWEST arrivals, so the
        // omission truncated the delay tail and would have understated p95
        // delay -- a bias in the flattering direction. On-time status is judged
        // on TRUE time here, so a C3 clock offset never alters what recovery
        // analysis is scored against.
        {
            const bool arrivalOnTimeTrue =
                (delay >= Time(0)) && (delay <= hdr.GetDeadline());
            m_rxTrace(hdr.GetSessionId(), seq, gen.GetNanoSeconds(),
                      now.GetNanoSeconds(), arrivalOnTimeTrue);
        }

        if (bs.closed)
        {
            // Arrived after its block was finalized; it cannot contribute.
            ++m_late;
            continue;
        }

        // E1: on time iff 0 <= r_p - g_p <= D. Equality at the boundary is
        // ON TIME, so the comparison is inclusive on both ends.
        //
        // C3 clock sensitivity: the receiver decides on ITS OWN clock, which
        // may be wrong by m_clockOffset. The decision that drives the receipt
        // is the observed one -- that is the point of the experiment. True
        // simulator time is evaluated alongside it and recorded, so the
        // manuscript can report how often a wrong clock changed the verdict
        // instead of asserting that it did not. With the default zero offset
        // the two are identical by construction.
        const Time observedDelay = delay + m_clockOffset;
        // R037. The ablation relaxes only what counts as GOOD for the evidence
        // block. onTimeTrue below is the study's primary endpoint and must stay
        // deadline-qualified in every arm, or the ablation would change the
        // measurement rather than the mechanism.
        const bool withinDeadline = m_ignoreDeadline || (observedDelay <= hdr.GetDeadline());
        const bool onTime = (observedDelay >= Time(0)) && withinDeadline;
        const bool onTimeTrue = (delay >= Time(0)) && (delay <= hdr.GetDeadline());
        m_ontimeObserved += onTime ? 1 : 0;
        m_ontimeTrue += onTimeTrue ? 1 : 0;
        if (onTime != onTimeTrue)
        {
            ++m_clockDisagreements;
        }
        if (onTime)
        {
            bs.ontimeMask |= static_cast<uint32_t>(bit);
        }
        else
        {
            ++m_late;
        }

        if (!bs.closureScheduled)
        {
            // c_b = e_b + D, where e_b = t0 + ((b+1)M - 1) * I.
            const Time e_b = hdr.GetStartTime() +
                             hdr.GetInterval() * static_cast<int64_t>((blockId + 1) * M - 1);
            const Time c_b = e_b + hdr.GetDeadline();
            const Time closureAt = c_b + kClosureOffset;

            if (closureAt > now)
            {
                bs.closureScheduled = true;
                bs.closureEvent = Simulator::Schedule(closureAt - now,
                                                      &ServiceReceiptApp::CloseBlock,
                                                      this,
                                                      blockId);
            }
            else
            {
                // First arrival already past closure: the block cannot be
                // reported (E2 requires at least one packet observed BEFORE
                // closure). Mark closed so nothing accumulates.
                bs.closed = true;
            }
        }
    }
}

void
ServiceReceiptApp::CloseBlock(uint64_t blockId)
{
    auto it = m_blocks.find(blockId);
    if (it == m_blocks.end())
    {
        return; // never observed; E2 forbids reporting an entirely unseen block
    }
    BlockState& bs = it->second;
    if (bs.closed)
    {
        return;
    }
    bs.closed = true;

    // The mask is finalized as-is: bits for missing or late packets stay zero.
    // No packet is ever fabricated for a missing sequence (E2).
    ServiceReceiptHeader rh;
    rh.SetSessionId(bs.sessionId); // taken from the serialized telemetry schedule
    rh.SetBlockId(blockId);
    rh.SetCloseTime(Simulator::Now() - kClosureOffset); // report c_b, not c_b + 1 ns
    rh.SetOntimeMask(bs.ontimeMask);
    rh.SetBlockSize(bs.blockSize);
    rh.SetDestination(Ipv4Address::GetAny());
    rh.SetSource(m_haveSource ? m_sourceAddress : Ipv4Address::GetAny());

    bool sent = false;
    if (m_haveSource)
    {
        // The receiver checks route usability before sending. The specification
        // is explicit that this check alone is NOT sufficient, because the route
        // can change between the check and actual output; the mandatory
        // routing-level NoDiscovery guard is implemented in the SCR fork
        // (component 7) and is what actually prevents a reverse-route RREQ.
        //
        // NOTE: until that guard lands, this application must not be used in a
        // scientific run, because a missing route here could still trigger an
        // unbudgeted reverse discovery and silently corrupt cost accounting.
        Ptr<Packet> report = Create<Packet>(0);
        report->AddHeader(rh);

        // M16: mark the feedback so the SCR fork refuses to originate a
        // reverse-route discovery for it. Without this tag a missing reverse
        // route would trigger an unbudgeted RREQ and silently corrupt the cost
        // accounting that the whole study measures.
        NoDiscoveryTag ndTag;
        report->AddPacketTag(ndTag);

        int rc = m_reportSocket->SendTo(
            report, 0, InetSocketAddress(m_sourceAddress, m_reportPort));
        if (rc >= 0)
        {
            ++m_reportsSent;
            sent = true;
        }
        else
        {
            // No usable reverse route: drop this feedback locally and record it.
            // Never discover a route for feedback (E2 / section 7).
            ++m_reportsNoRoute;
            NS_LOG_INFO("block " << blockId << ": no reverse route, feedback dropped locally");
        }
    }
    else
    {
        ++m_reportsNoRoute;
    }

    m_receiptTrace(bs.sessionId, blockId, bs.ontimeMask, bs.blockSize, sent);
}

} // namespace scr
} // namespace ns3
