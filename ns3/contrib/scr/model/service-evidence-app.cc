/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * SCR — source-side receipt listener. Traceability M05 / M10 (E2, E3).
 */

#include "service-evidence-app.h"

#include "scr-routing-protocol.h"
#include "service-headers.h"

#include "ns3/double.h"
#include "ns3/inet-socket-address.h"
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

NS_LOG_COMPONENT_DEFINE("ScrServiceEvidenceApp");
NS_OBJECT_ENSURE_REGISTERED(ServiceEvidenceApp);

TypeId
ServiceEvidenceApp::GetTypeId()
{
    static TypeId tid = TypeId("ns3::scr::ServiceEvidenceApp")
                            .SetParent<Application>()
                            .SetGroupName("Scr")
                            .AddConstructor<ServiceEvidenceApp>()
                            .AddAttribute("Port",
                                          "UDP port on which receipts arrive",
                                          UintegerValue(6001),
                                          MakeUintegerAccessor(&ServiceEvidenceApp::m_port),
                                          MakeUintegerChecker<uint16_t>())
                            .AddAttribute("FeedbackLoss",
                                          "C3: probability that an arriving "
                                          "receipt is discarded at the source "
                                          "routing guard. Data is unaffected.",
                                          DoubleValue(0.0),
                                          MakeDoubleAccessor(&ServiceEvidenceApp::m_feedbackLoss),
                                          MakeDoubleChecker<double>(0.0, 1.0));
    return tid;
}

ServiceEvidenceApp::ServiceEvidenceApp()
    : m_port(6001),
      m_confirmations(0),
      m_feedbackLoss(0.0),
      m_feedbackDropped(0),
      m_lastConfirmEpisodeStart(Seconds(-1))
{
    m_evidence = CreateObject<ServiceEvidence>();
}

ServiceEvidenceApp::~ServiceEvidenceApp()
{
}

void
ServiceEvidenceApp::DoDispose()
{
    m_socket = nullptr;
    m_evidence = nullptr;
    Application::DoDispose();
}

void
ServiceEvidenceApp::Configure(uint64_t sessionId,
                              Ipv4Address destination,
                              Time t0,
                              Time interval,
                              Time deadline,
                              uint32_t blockSize,
                              double theta,
                              uint32_t k,
                              Time freshness)
{
    m_destination = destination;
    m_evidence->Register(sessionId, destination, t0, interval, deadline, blockSize);
    m_evidence->SetTheta(theta);
    m_evidence->SetK(k);
    m_evidence->SetFreshness(freshness);

    // E2's structural condition F >= (K-1)*M*I must hold, otherwise confirmation
    // would be unreachable by construction and SCR would appear never to confirm
    // for a configuration reason rather than a network reason. Abort loudly.
    std::string why;
    NS_ABORT_MSG_UNLESS(m_evidence->ValidateConfiguration(why),
                        "invalid E2 configuration: " << why);
}

void
ServiceEvidenceApp::SetHighestGeneratedSequence(uint64_t seq)
{
    m_evidence->SetHighestGeneratedSequence(seq);
}

uint64_t
ServiceEvidenceApp::GetAccepted() const
{
    return m_evidence->GetAcceptedCount();
}

uint64_t
ServiceEvidenceApp::GetRejected() const
{
    return m_evidence->GetRejectedCount();
}

void
ServiceEvidenceApp::StartApplication()
{
    if (!m_socket)
    {
        m_socket = Socket::CreateSocket(GetNode(), UdpSocketFactory::GetTypeId());
        m_socket->Bind(InetSocketAddress(Ipv4Address::GetAny(), m_port));
        m_socket->SetRecvCallback(MakeCallback(&ServiceEvidenceApp::HandleRead, this));
    }
}

void
ServiceEvidenceApp::StopApplication()
{
    if (m_socket)
    {
        m_socket->Close();
    }
}

int64_t
ServiceEvidenceApp::AssignStreams(int64_t stream)
{
    // Create the variable ONLY when the impairment is active. At loss=0 no
    // stream is consumed, so enabling this feature cannot shift any other
    // component's random draws and the primary condition stays bit-identical
    // to a build without it. That property is what lets C3 be compared against
    // the frozen C2 results.
    if (m_feedbackLoss <= 0.0)
    {
        return 0;
    }
    if (!m_lossRv)
    {
        m_lossRv = CreateObject<UniformRandomVariable>();
        m_lossRv->SetAttribute("Min", DoubleValue(0.0));
        m_lossRv->SetAttribute("Max", DoubleValue(1.0));
    }
    m_lossRv->SetStream(stream);
    return 1;
}

void
ServiceEvidenceApp::HandleRead(Ptr<Socket> socket)
{
    Ptr<Packet> packet;
    Address from;
    while ((packet = socket->RecvFrom(from)))
    {
        // C3 feedback impairment, applied at the source routing guard BEFORE
        // the receipt is parsed or examined. Dropping here (rather than inside
        // the evidence logic) is what makes this a channel-side impairment
        // rather than a change to the acceptance rule under test.
        if (m_lossRv && m_lossRv->GetValue() < m_feedbackLoss)
        {
            ++m_feedbackDropped;
            continue;
        }

        ServiceReceiptHeader rh;
        uint32_t consumed = packet->RemoveHeader(rh);
        const Time now = Simulator::Now();

        if (consumed == 0 || !rh.IsValid())
        {
            NS_LOG_WARN("malformed receipt discarded");
            continue;
        }

        if (!m_evidence->AcceptReport(rh, now))
        {
            // Rejections are counted and traced inside ServiceEvidence. A
            // rejected report must never influence any decision.
            continue;
        }

        // Keep the block map bounded: anything past c_b + F can no longer
        // contribute to a confirmation.
        m_evidence->PruneExpired(now);

        Ptr<Ipv4> ipv4 = GetNode()->GetObject<Ipv4>();
        Ptr<RoutingProtocol> rp = DynamicCast<RoutingProtocol>(ipv4->GetRoutingProtocol());
        if (!rp)
        {
            // OLSR/DSDV references run the same feedback apps but have no SCR
            // ledger to notify. The receipts still cost real bytes, which is the
            // point of running them.
            continue;
        }

        const Time episodeStart = rp->ScrGetEpisodeStart(m_destination);
        if (m_evidence->CanConfirm(now, episodeStart))
        {
            // Only confirm once per episode; E3 renews exactly once.
            if (episodeStart != m_lastConfirmEpisodeStart)
            {
                m_lastConfirmEpisodeStart = episodeStart;
                ++m_confirmations;
                rp->ScrOnServiceConfirmed(m_destination);
            }
        }
    }
}

} // namespace scr
} // namespace ns3
