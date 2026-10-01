/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Service-Confirmed Repair (SCR) — destination service receipt application.
 *
 * Specification E1/E2:
 *   y_p = 1{a first receiver arrival exists and 0 <= r_p - g_p <= D}
 *   Duplicate arrivals never increase y_p.
 *   Block b closes at c_b = e_b + D, "just after all same-time receive
 *   callbacks at c_b"; implemented with a one-nanosecond closure offset.
 *   The receiver sends ONE summary at c_b + 1 ns if it received at least one
 *   packet of that block before closure. It sends no report for an entirely
 *   unseen block and never fabricates packets for missing sequences.
 *
 * Traceability: M02 (deadline qualification and duplicates),
 *               M03 (fixed block closure).
 * Validating fixture: T2.
 *
 * State location: destination node only.
 */

#ifndef SCR_SERVICE_RECEIPT_APP_H
#define SCR_SERVICE_RECEIPT_APP_H

#include "ns3/application.h"
#include "ns3/event-id.h"
#include "ns3/ipv4-address.h"
#include "ns3/nstime.h"
#include "ns3/socket.h"
#include "ns3/traced-callback.h"

#include <cstdint>
#include <map>

namespace ns3
{
namespace scr
{

/**
 * @ingroup scr
 * @brief Receives telemetry, qualifies arrivals against the deadline, and emits
 *        one serialized receipt per observed block.
 *
 * The receiver derives the schedule from the serialized header rather than from
 * shared simulator state, so the evidence path is genuinely remote: nothing here
 * reads the source application's memory.
 */
class ServiceReceiptApp : public Application
{
  public:
    static TypeId GetTypeId();

    ServiceReceiptApp();
    ~ServiceReceiptApp() override;

    uint64_t GetReceivedCount() const { return m_received; }
    uint64_t GetDuplicateCount() const { return m_duplicates; }
    uint64_t GetLateCount() const { return m_late; }
    uint64_t GetReportsSent() const { return m_reportsSent; }
    uint64_t GetReportsSuppressedNoRoute() const { return m_reportsNoRoute; }
    uint64_t GetMalformedCount() const { return m_malformed; }

    /// C3 clock sensitivity. The receiver's own clock is offset by this amount
    /// when it decides whether a packet met its deadline. True simulator time
    /// is retained for the observer counters below, so the effect of a wrong
    /// clock can be measured rather than assumed. Default zero: with no offset
    /// the observed and true decisions are identical by construction.
    uint64_t GetOntimeObserved() const { return m_ontimeObserved; }
    uint64_t GetOntimeTrue() const { return m_ontimeTrue; }
    /// Packets the offset clock judged differently from true time.
    uint64_t GetClockDisagreements() const { return m_clockDisagreements; }

  protected:
    void DoDispose() override;

  private:
    void StartApplication() override;
    void StopApplication() override;

    void HandleRead(Ptr<Socket> socket);
    void CloseBlock(uint64_t blockId);

    /// Per-block accumulation state, created on first arrival of that block.
    struct BlockState
    {
        uint64_t sessionId = 0;   //!< session from the serialized schedule
        uint32_t ontimeMask = 0;  //!< bit i set iff packet b*M+i arrived on time
        uint32_t blockSize = 0;   //!< M, taken from the serialized schedule
        uint64_t seenMask = 0;    //!< first-arrival tracking for duplicate rejection
        bool closed = false;
        bool closureScheduled = false;
        EventId closureEvent;
    };

    Ptr<Socket> m_socket;       //!< inbound telemetry
    Ptr<Socket> m_reportSocket; //!< outbound receipts
    uint16_t m_port;
    uint16_t m_reportPort;

    std::map<uint64_t, BlockState> m_blocks; //!< block id -> state
    Ipv4Address m_sourceAddress;             //!< learned from the arriving packet
    bool m_haveSource;

    uint64_t m_received;
    uint64_t m_duplicates;
    uint64_t m_late;
    uint64_t m_reportsSent;
    uint64_t m_reportsNoRoute;
    uint64_t m_malformed;
    Time m_clockOffset;            //!< C3: receiver clock error, default 0
    uint64_t m_ontimeObserved;     //!< decisions made on the offset clock
    /// NO-DEADLINE ablation (R037): when true the receiver treats ANY arrival
    /// as good when scoring an evidence block. It never affects m_ontimeTrue,
    /// which is the measured endpoint and stays deadline-qualified.
    bool m_ignoreDeadline;
    uint64_t m_ontimeTrue;         //!< what true simulator time would have said
    uint64_t m_clockDisagreements; //!< observed != true

    /// (session, block, mask, blockSize, sent)
    TracedCallback<uint64_t, uint64_t, uint32_t, uint32_t, bool> m_receiptTrace;

    /// M19/R024: per-packet arrival record. The existing Receipt trace is
    /// BLOCK level, which cannot reconstruct outage episodes -- recovery
    /// analysis needs to know when each individual packet arrived and whether
    /// it met its deadline. Observer only: nothing here feeds a protocol
    /// decision.
    /// Arguments: sessionId, sequence, generationNs, arrivalNs, onTimeTrue.
    TracedCallback<uint64_t, uint64_t, int64_t, int64_t, bool> m_rxTrace;
};

} // namespace scr
} // namespace ns3

#endif /* SCR_SERVICE_RECEIPT_APP_H */
