/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Service-Confirmed Repair (SCR) — deadline telemetry source application.
 *
 * Specification E1 (deadline-qualified delivery):
 *   g_p = t0 + p*I, sequence numbers start at zero.
 *   Block b contains p = b*M ... (b+1)*M - 1.
 *   Generation counts include EVERY scheduled application packet, even if its
 *   socket send fails.
 *
 * Traceability: M01. Validating fixtures: T12 (generation/arrival join),
 * T2 (deadline boundary, via the receiver).
 *
 * State location: source node only. This application never reads another node's
 * state and never receives protocol feedback directly; feedback is handled by
 * ServiceEvidence on the source routing module.
 */

#ifndef SCR_DEADLINE_TELEMETRY_APP_H
#define SCR_DEADLINE_TELEMETRY_APP_H

#include "ns3/application.h"
#include "ns3/event-id.h"
#include "ns3/ipv4-address.h"
#include "ns3/nstime.h"
#include "ns3/socket.h"
#include "ns3/traced-callback.h"

#include <cstdint>

namespace ns3
{
namespace scr
{

/**
 * @ingroup scr
 * @brief Periodic telemetry source with a fixed local schedule and deadline.
 *
 * Emits real UDP packets carrying a serialized 56-byte TelemetryHeader plus the
 * configured scientific payload. The schedule is entirely local: the application
 * does not consult routing state and does not change rate in response to loss,
 * so offered demand is identical across every algorithm under comparison.
 */
class DeadlineTelemetryApp : public Application
{
  public:
    static TypeId GetTypeId();

    DeadlineTelemetryApp();
    ~DeadlineTelemetryApp() override;

    /// Generation event: (session, sequence, generation time, send accepted).
    typedef void (*GenerateTracedCallback)(uint64_t, uint64_t, Time, bool);

    uint64_t GetSessionId() const { return m_sessionId; }
    uint64_t GetGeneratedCount() const { return m_generated; }
    uint64_t GetSendErrorCount() const { return m_sendErrors; }
    /// True if a flow was marked with a transport integration error.
    bool HasTransportIntegrationError() const { return m_transportError; }
    /// R017: sends abandoned because no route existed. A routing outcome scored
    /// as loss, deliberately NOT a transport integration error -- see the note
    /// in RetrySend for why conflating them biased the comparison.
    uint64_t GetNoRouteDrops() const { return m_noRouteDrops; }

    /// First generation time of block b: t0 + b*M*I.
    Time BlockFirstGenerationTime(uint64_t block) const;
    /// Last generation time of block b: e_b = t0 + ((b+1)M - 1) * I.
    Time BlockLastGenerationTime(uint64_t block) const;
    /// Closing time of block b: c_b = e_b + D.
    Time BlockCloseTime(uint64_t block) const;

  protected:
    void DoDispose() override;

  private:
    void StartApplication() override;
    void StopApplication() override;

    /// Emit the next scheduled packet and reschedule.
    void Generate();
    /// Retry an immediate socket failure (100 ms, at most 3 attempts).
    void RetrySend(Ptr<Packet> packet, uint64_t sequence, uint32_t attempt);

    Ptr<Socket> m_socket;
    Ipv4Address m_peerAddress;
    uint16_t m_peerPort;

    uint64_t m_sessionId;
    Time m_interval;   //!< I
    Time m_deadline;   //!< D
    uint32_t m_blockSize;    //!< M
    uint32_t m_payloadBytes; //!< scientific payload, additional to the header

    Time m_t0;              //!< first generation time, fixed at StartApplication
    uint64_t m_sequence;    //!< next sequence number, starts at zero
    uint64_t m_generated;   //!< every scheduled packet, regardless of send outcome
    uint64_t m_sendErrors;  //!< immediate socket failures observed
    bool m_transportError;  //!< set when retries are exhausted for a NON-routing reason
    uint64_t m_noRouteDrops; //!< R017: retries exhausted because there was no route
    bool m_running;
    EventId m_generateEvent;

    TracedCallback<uint64_t, uint64_t, Time, bool> m_generateTrace;
};

} // namespace scr
} // namespace ns3

#endif /* SCR_DEADLINE_TELEMETRY_APP_H */
