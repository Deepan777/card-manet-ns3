/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * SCR — source-side receipt listener.
 *
 * Closes the evidence loop: receives real UDP receipts, validates them through
 * ServiceEvidence (E2), and on a valid confirmation notifies the local SCR
 * routing protocol (E3).
 *
 * This app deliberately holds NO pointer to the destination's application. It
 * sees only deserialized bytes that arrived over the radio, which is what makes
 * the evidence genuinely remote rather than a shared-memory shortcut.
 *
 * Traceability: M05 / M10. Validating fixture: T2, T12.
 */

#ifndef SCR_SERVICE_EVIDENCE_APP_H
#define SCR_SERVICE_EVIDENCE_APP_H

#include "service-evidence.h"

#include "ns3/application.h"
#include "ns3/random-variable-stream.h"
#include "ns3/ipv4-address.h"
#include "ns3/socket.h"

namespace ns3
{
namespace scr
{

class RoutingProtocol;

/**
 * @ingroup scr
 * @brief Listens for service receipts at the source and drives E2/E3.
 */
class ServiceEvidenceApp : public Application
{
  public:
    static TypeId GetTypeId();

    ServiceEvidenceApp();
    ~ServiceEvidenceApp() override;

    /// Register the local schedule that receipts are validated against.
    void Configure(uint64_t sessionId,
                   Ipv4Address destination,
                   Time t0,
                   Time interval,
                   Time deadline,
                   uint32_t blockSize,
                   double theta,
                   uint32_t k,
                   Time freshness);

    /// Track the highest generated sequence so impossible masks are rejected.
    void SetHighestGeneratedSequence(uint64_t seq);

    uint64_t GetAccepted() const;
    uint64_t GetRejected() const;
    uint64_t GetConfirmations() const { return m_confirmations; }

    /// C3 feedback impairment. Receipts are discarded at the SOURCE routing
    /// guard with this probability, before any evidence is examined. Data is
    /// untouched and no other routing packet is affected, so this isolates
    /// sensitivity to feedback loss rather than modelling an asymmetric
    /// channel. Default zero, and at zero no random variable is created and no
    /// RNG stream is consumed, so the primary condition is bit-identical to a
    /// build without this feature.
    uint64_t GetFeedbackDropped() const { return m_feedbackDropped; }

    /// Assign the impairment stream. C3 gives it a range disjoint from
    /// mobility, PHY and routing so feedback loss cannot shift the exogenous
    /// inputs that the paired design depends on. Returns streams consumed.
    int64_t AssignStreams(int64_t stream);

    Ptr<ServiceEvidence> GetEvidence() const { return m_evidence; }

  protected:
    void DoDispose() override;

  private:
    void StartApplication() override;
    void StopApplication() override;
    void HandleRead(Ptr<Socket> socket);

    Ptr<Socket> m_socket;
    uint16_t m_port;
    Ptr<ServiceEvidence> m_evidence;
    Ipv4Address m_destination;
    uint64_t m_confirmations;
    double m_feedbackLoss;              //!< C3 Bernoulli drop probability
    Ptr<UniformRandomVariable> m_lossRv; //!< created only when m_feedbackLoss > 0
    uint64_t m_feedbackDropped;
    /// Episode start last used for a confirmation test, to avoid re-confirming.
    Time m_lastConfirmEpisodeStart;
};

} // namespace scr
} // namespace ns3

#endif /* SCR_SERVICE_EVIDENCE_APP_H */
