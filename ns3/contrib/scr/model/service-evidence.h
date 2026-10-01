/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Service-Confirmed Repair (SCR) — source-side service evidence (E2).
 *
 * Specification E2:
 *   q_b = (sum of y_p over block b) / M ; good_b = 1{q_b >= theta}
 *   The sender accepts a report only if the session, destination, block,
 *   schedule and mask match local registration; all claimed packets were
 *   generated; the block has closed; and the report arrives by c_b + F.
 *   Duplicate block reports are ignored. Future blocks, impossible masks and
 *   mismatched sessions are rejected and logged.
 *   A confirmation at source time t requires K consecutive good block IDs, all
 *   fresh at t, with each block's first generation time at or after the current
 *   episode start a_e.
 *   Primary theta=0.8, K=2, F=2 s. F must be at least (K-1)*M*I.
 *
 * Traceability: M05. Validating fixture: T2.
 *
 * State location: source node only. This class never reads receiver state; it
 * consumes only deserialized receipt headers that arrived as real UDP packets.
 */

#ifndef SCR_SERVICE_EVIDENCE_H
#define SCR_SERVICE_EVIDENCE_H

#include "service-headers.h"

#include "ns3/ipv4-address.h"
#include "ns3/nstime.h"
#include "ns3/object.h"
#include "ns3/traced-callback.h"

#include <cstdint>
#include <map>
#include <string>

namespace ns3
{
namespace scr
{

/// Reason a report was rejected. Every rejection is logged with one of these.
enum ReportRejectReason : uint8_t
{
    REJECT_NONE = 0,
    REJECT_SESSION_MISMATCH,
    REJECT_SCHEDULE_MISMATCH,
    REJECT_BLOCK_NOT_CLOSED,   //!< future block, or block not yet closed at source
    REJECT_STALE,              //!< arrived after c_b + F
    REJECT_DUPLICATE,
    REJECT_IMPOSSIBLE_MASK,    //!< mask claims packets never generated
    REJECT_MALFORMED,
};

const char* ReportRejectReasonName(ReportRejectReason r);

/**
 * @ingroup scr
 * @brief Validates remote service receipts and evaluates the E2 confirmation rule.
 *
 * The block map is bounded and keyed by block id, NOT by arrival order, because
 * E2 explicitly permits reports to arrive out of order and requires K
 * *consecutive block ids* rather than an arrival-order streak.
 */
class ServiceEvidence : public Object
{
  public:
    static TypeId GetTypeId();

    ServiceEvidence();
    ~ServiceEvidence() override;

    /**
     * Register the local schedule this source is generating. Reports are checked
     * against this registration; nothing else is trusted.
     */
    void Register(uint64_t sessionId,
                  Ipv4Address destination,
                  Time t0,
                  Time interval,
                  Time deadline,
                  uint32_t blockSize);

    /// Highest sequence number generated so far, used to reject impossible masks.
    void SetHighestGeneratedSequence(uint64_t seq) { m_highestGenerated = seq; }

    /**
     * Validate and store one received receipt.
     * @return true if accepted; false if rejected (reason reported via trace).
     */
    bool AcceptReport(const ServiceReceiptHeader& rh, Time now);

    /**
     * E2 confirmation test at source time t for the episode that started at a_e.
     * @return true if K consecutive good blocks are all fresh at t and all begin
     *         at or after a_e.
     */
    bool CanConfirm(Time now, Time episodeStart) const;

    /// c_b for block b, derived from the registered schedule.
    Time BlockCloseTime(uint64_t block) const;
    /// t0 + b*M*I, the first generation time of block b.
    Time BlockFirstGenerationTime(uint64_t block) const;

    void SetTheta(double v) { m_theta = v; }
    void SetK(uint32_t v) { m_k = v; }
    void SetFreshness(Time v) { m_freshness = v; }

    double GetTheta() const { return m_theta; }
    uint32_t GetK() const { return m_k; }
    Time GetFreshness() const { return m_freshness; }

    uint64_t GetAcceptedCount() const { return m_accepted; }
    uint64_t GetRejectedCount() const { return m_rejected; }
    ReportRejectReason GetLastRejectReason() const { return m_lastReject; }

    /**
     * Structural validation required by E2: F >= (K-1)*M*I, so that K successive
     * reports can be simultaneously fresh even on a low-delay path.
     * @return true if the configuration is admissible.
     */
    bool ValidateConfiguration(std::string& why) const;

    /// Drop stored blocks that can no longer be fresh, keeping the map bounded.
    void PruneExpired(Time now);

  private:
    struct BlockRecord
    {
        bool good = false;
        Time closeTime;      //!< c_b
        Time acceptedAt;
        uint32_t ontime = 0;
        uint32_t blockSize = 0;
    };

    bool m_registered;
    uint64_t m_sessionId;
    Ipv4Address m_destination;
    Time m_t0;
    Time m_interval;
    Time m_deadline;
    uint32_t m_blockSize;
    uint64_t m_highestGenerated;

    double m_theta;
    uint32_t m_k;
    Time m_freshness; //!< F

    std::map<uint64_t, BlockRecord> m_blocks;
    uint64_t m_accepted;
    uint64_t m_rejected;
    ReportRejectReason m_lastReject;

    /// (block id, accepted, reason)
    TracedCallback<uint64_t, bool, uint8_t> m_reportTrace;
};

} // namespace scr
} // namespace ns3

#endif /* SCR_SERVICE_EVIDENCE_H */
