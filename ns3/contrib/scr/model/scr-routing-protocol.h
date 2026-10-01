/*
 * Copyright (c) 2009 IITP RAS
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Based on
 *      NS-2 AODV model developed by the CMU/MONARCH group and optimized and
 *      tuned by Samir Das and Mahesh Marina, University of Cincinnati;
 *
 *      AODV-UU implementation by Erik Nordström of Uppsala University
 *      https://web.archive.org/web/20100527072022/http://core.it.uu.se/core/index.php/AODV-UU
 *
 * Authors: Elena Buchatskaia <borovkovaes@iitp.ru>
 *          Pavel Boyko <boyko@iitp.ru>
 */
/*
 * ---------------------------------------------------------------------------
 * SCR FORK NOTICE
 *
 * This file is a fork of the ns-3.46.1 AODV module (src/aodv), taken verbatim
 * and then modified for the Service-Confirmed Repair (SCR) study. The upstream
 * copyright, licence and authorship above are those of the original AODV
 * implementation and are reproduced unchanged.
 *
 * Upstream source : ns-3.46.1, commit 51387bce7e5f5c57aa080612a4ed690bf33d0c92
 * Fork created    : 2026-09-07
 * Fork namespace  : ns3::scr  (upstream ns3::aodv remains present and untouched,
 *                   so that AODV-STOCK is a genuine native reference)
 *
 * SCR-specific modifications are marked with "SCR:" comments. Where no such
 * marker appears, the logic is upstream AODV behaviour.
 * ---------------------------------------------------------------------------
 */

#ifndef SCRROUTINGPROTOCOL_H
#define SCRROUTINGPROTOCOL_H

#include "scr-dpd.h"
#include "scr-neighbor.h"
#include "scr-packet.h"
#include "scr-rqueue.h"
#include "scr-rtable.h"

#include <vector>
#include <deque>
#include <map>
#include <set>
#include <string>
#include "repair-ledger.h"
#include "origin-scheduler.h"
#include "service-evidence.h"

#include "ns3/ipv4-interface.h"
#include "ns3/ipv4-l3-protocol.h"
#include "ns3/ipv4-routing-protocol.h"
#include "ns3/node.h"
#include "ns3/output-stream-wrapper.h"
#include "ns3/random-variable-stream.h"
#include "ns3/vector.h"
#include "ns3/wifi-mac.h"
#include "ns3/wifi-phy.h"

#include <map>

namespace ns3
{

class WifiMpdu;
enum WifiMacDropReason : uint8_t; // opaque enum declaration

namespace scr
{
/**
 * @ingroup scr
 *
 * @brief SCR routing protocol
 */
class RoutingProtocol : public Ipv4RoutingProtocol
{
  public:
    /**
     * @brief Get the type ID.
     * @return the object TypeId
     */
    static TypeId GetTypeId();
    static const uint32_t SCR_PORT;

    /// constructor
    RoutingProtocol();
    ~RoutingProtocol() override;
    void DoDispose() override;

    // Inherited from Ipv4RoutingProtocol
    Ptr<Ipv4Route> RouteOutput(Ptr<Packet> p,
                               const Ipv4Header& header,
                               Ptr<NetDevice> oif,
                               Socket::SocketErrno& sockerr) override;
    bool RouteInput(Ptr<const Packet> p,
                    const Ipv4Header& header,
                    Ptr<const NetDevice> idev,
                    const UnicastForwardCallback& ucb,
                    const MulticastForwardCallback& mcb,
                    const LocalDeliverCallback& lcb,
                    const ErrorCallback& ecb) override;
    void NotifyInterfaceUp(uint32_t interface) override;
    void NotifyInterfaceDown(uint32_t interface) override;
    void NotifyAddAddress(uint32_t interface, Ipv4InterfaceAddress address) override;
    void NotifyRemoveAddress(uint32_t interface, Ipv4InterfaceAddress address) override;
    void SetIpv4(Ptr<Ipv4> ipv4) override;
    void PrintRoutingTable(Ptr<OutputStreamWrapper> stream,
                           Time::Unit unit = Time::S) const override;

    // Handle protocol parameters
    /**
     * Get maximum queue time
     * @returns the maximum queue time
     */
    Time GetMaxQueueTime() const
    {
        return m_maxQueueTime;
    }

    /**
     * Set the maximum queue time
     * @param t the maximum queue time
     */
    void SetMaxQueueTime(Time t);

    /**
     * Get the maximum queue length
     * @returns the maximum queue length
     */
    uint32_t GetMaxQueueLen() const
    {
        return m_maxQueueLen;
    }

    /**
     * Set the maximum queue length
     * @param len the maximum queue length
     */
    void SetMaxQueueLen(uint32_t len);

    /**
     * Get destination only flag
     * @returns the destination only flag
     */
    bool GetDestinationOnlyFlag() const
    {
        return m_destinationOnly;
    }

    /**
     * Set destination only flag
     * @param f the destination only flag
     */
    void SetDestinationOnlyFlag(bool f)
    {
        m_destinationOnly = f;
    }

    /**
     * Get gratuitous reply flag
     * @returns the gratuitous reply flag
     */
    bool GetGratuitousReplyFlag() const
    {
        return m_gratuitousReply;
    }

    /**
     * Set gratuitous reply flag
     * @param f the gratuitous reply flag
     */
    void SetGratuitousReplyFlag(bool f)
    {
        m_gratuitousReply = f;
    }

    /**
     * Set hello enable
     * @param f the hello enable flag
     */
    void SetHelloEnable(bool f)
    {
        m_enableHello = f;
    }

    /**
     * Get hello enable flag
     * @returns the enable hello flag
     */
    bool GetHelloEnable() const
    {
        return m_enableHello;
    }

    /**
     * Set broadcast enable flag
     * @param f enable broadcast flag
     */
    void SetBroadcastEnable(bool f)
    {
        m_enableBroadcast = f;
    }

    /**
     * Get broadcast enable flag
     * @returns the broadcast enable flag
     */
    bool GetBroadcastEnable() const
    {
        return m_enableBroadcast;
    }

    /**
     * Assign a fixed random variable stream number to the random variables
     * used by this model.  Return the number of streams (possibly zero) that
     * have been assigned.
     *
     * @param stream first stream index to use
     * @return the number of stream indices assigned by this model
     */
    int64_t AssignStreams(int64_t stream);

    /**
     * D003: pin the dedicated scheduler-jitter stream.
     *
     * Deliberately NOT folded into AssignStreams. The helper advances its
     * cursor by that function's return value, so raising it from 1 to 2 would
     * shift every later node's routing stream and change the results of EVERY
     * algorithm, including the native references D002 requires be left alone.
     * The jitter therefore takes a disjoint base of its own.
     */
    void AssignJitterStream(int64_t stream);

  protected:
    void DoInitialize() override;

  private:
    /**
     * Notify that an MPDU was dropped.
     *
     * @param reason the reason why the MPDU was dropped
     * @param mpdu the dropped MPDU
     */
    void NotifyTxError(WifiMacDropReason reason, Ptr<const WifiMpdu> mpdu);

    // ---- CARD (R053): break-cause classification ----
    //
    // The detecting node sees the neighbour it just lost; the source does not.
    // CARD therefore classifies at the detecting node and propagates the verdict
    // in the RERR. The evidence is the neighbour's received signal: a neighbour
    // that is moving out of range was last heard near the edge of reception with
    // a falling signal. Signal level alone is not enough, because minimum-hop
    // routes deliberately use long links, so static links near the edge are
    // common; the falling trend is what separates a departure from a loss.

    /// Per-neighbour received-signal record, fed by the PHY monitor trace.
    struct CardLinkObs
    {
        double curDbm{0.0};    //!< most recent frame
        Time curT;
        double anchorDbm{0.0}; //!< sample opened the current >= 1 s window
        Time anchorT;
        double prevDbm{0.0};   //!< anchor of the previous window
        Time prevT;
        bool hasPrev{false};
    };
    /// Break verdicts, also used in the CardBreak log.
    enum CardCause : uint8_t
    {
        CARD_TRANSIENT = 0, //!< no sign that the neighbour left
        CARD_TOPOLOGY = 1,  //!< neighbour near the reception edge with a falling signal
        CARD_UNKNOWN = 2,   //!< no signal record for the neighbour
    };
    /// PHY monitor callback. Only CARD modes do any work here.
    void CardSnifferRx(Ptr<const Packet> packet,
                       uint16_t channelFreqMhz,
                       WifiTxVector txVector,
                       MpduInfo aMpdu,
                       SignalNoiseDbm signalNoise,
                       uint16_t staId);
    /// Classify the loss of a neighbour. Fills the evidence for the log.
    CardCause CardClassify(Ipv4Address neighbor, double& dbm, double& trendDb, double& ageS);
    /// Whether a break with this verdict is reported as a topology change.
    bool CardReportsTopology(CardCause c) const;
    /// Record the reported cause of a destination's most recent invalidation.
    void CardRecordCause(Ipv4Address dst, bool topology);

    std::map<Mac48Address, CardLinkObs> m_cardObs;
    /// Neighbour IP -> MAC, learned from the AODV broadcasts each neighbour
    /// sends. Needed because a neighbour known only from its hellos has no ARP
    /// entry, so the ARP cache alone left 41% of breaks unclassifiable in the
    /// first smoke run.
    std::map<Ipv4Address, Mac48Address> m_cardIpMac;
    /// dst -> (reported as topology change, when). Erased when the source
    /// installs a new route to dst.
    std::map<Ipv4Address, std::pair<bool, Time>> m_cardCause;
    double m_cardEdgeDbm{-79.0}; //!< CardEdgeDbm attribute
    double m_cardTrendDb{0.1};   //!< CardTrendDb attribute
    uint64_t m_cardBreakTopology{0};
    uint64_t m_cardBreakTransient{0};
    uint64_t m_cardBreakUnknown{0};
    uint64_t m_cardRerrTopologyRx{0};
    uint64_t m_cardExemptAdmitted{0};
    Callback<void, Ipv4Address, Ipv4Address, double, double, double, uint8_t> m_cardBreakLog;

    // ---- CLAF-AODV baseline (R057) ------------------------------------------
    // Reimplementation of Safari et al., IEEE Access 11 (2023) 50805-50822: a
    // relay forwards a RREQ with a probability from a two-level Mamdani fuzzy
    // system (node quality from queue, idle channel, contention window, energy
    // and received signal; then forwarding probability from node degree, node
    // quality and the path quality carried in the RREQ), falls back to a
    // counter-based rebroadcast, and the destination replies to the best of the
    // copies it collects. Every constant is from the paper (tables and figures
    // transcribed in RESEARCH_LEDGER R057) except those the paper leaves open,
    // which are attributes below.
    bool IsClaf() const
    {
        return IsClafMode(m_algo); // CLAF-AODV, and CARD-CLAF's relays (R060)
    }
    void ClafSample();
    void ClafPhyState(Time start, Time duration, WifiPhyState state);
    double ClafRssDbm(Ipv4Address src);
    double ClafNodeQuality(double rssDbm);
    double ClafForwardProbability(uint32_t nd, double nqm, double pqm);
    void RebroadcastRreq(RreqHeader rreqHeader,
                         uint8_t ttl,
                         const TaaodvExtHeader* taa = nullptr);
    void ClafPhase3Expire(std::pair<Ipv4Address, uint32_t> key);
    void ClafDestExpire(std::pair<Ipv4Address, uint32_t> key);
    struct ClafPending
    {
        RreqHeader hdr;
        uint8_t ttl{0};
        uint32_t copies{0};
    };
    struct ClafCopy
    {
        Ipv4Address src;
        uint8_t hop{0};
        double pqm{0.0};
        RreqHeader hdr;
    };
    std::map<std::pair<Ipv4Address, uint32_t>, ClafPending> m_clafPending;
    std::map<std::pair<Ipv4Address, uint32_t>, std::vector<ClafCopy>> m_clafDest;
    Ptr<WifiMac> m_clafMac;
    Ptr<WifiPhy> m_clafPhy;
    Ptr<UniformRandomVariable> m_clafRng;
    bool m_clafSamplerStarted{false};
    double m_clafQueueAvg{0.0};
    double m_clafCwAvg{0.0};
    bool m_clafCwInit{false};
    double m_clafIdleFrac{1.0};
    Time m_clafIdleAccum{Seconds(0)};
    Time m_clafWindowStart{Seconds(0)};
    uint32_t m_clafCounterC{3};
    double m_clafEwmaAlpha{0.5};
    Time m_clafDestTimer{MilliSeconds(40)};
    uint32_t m_clafNdThreshold{6};
    uint64_t m_clafSparseForward{0};
    uint64_t m_clafProbForward{0};
    uint64_t m_clafSuppressed{0};
    uint64_t m_clafPhase3Sent{0};
    uint64_t m_clafPhase3Cancelled{0};
    uint64_t m_clafPhase1Drop{0};
    uint64_t m_clafDestReplies{0};

    // ---- TAAODV baseline (R058) ---------------------------------------------
    // Reimplementation of Li, Li, Yu and Li, IEEE T-ITS 26 (2025) 17255-17268.
    // A node sending a RREQ ranks its neighbours by stability with TOPSIS over
    // three models -- V: overlap of the neighbour's own neighbour set across one
    // HELLO period (Eq. 2); N: its degree over the two-hop population (Eq. 3);
    // S: link lifetime from relative position and velocity (Eq. 12) -- weighted
    // by the coefficient-of-variation method (Eqs. 14-16), and names only the
    // best 3 (4-6 neighbours) or half (more than 6) as forwarders; with 3 or
    // fewer it broadcasts as AODV does. The RREQ accumulates a pheromone
    // (Pr/Pt) E / Cn per hop (Eqs. 23, 25, 26), and the source keeps, among the
    // replies of one HELLO interval, the path with the most pheromone per hop
    // (Eq. 27). HELLOs carry position, velocity and the neighbour list. The
    // choices where the paper is silent are listed in RESEARCH_LEDGER R058.
    bool IsTaaodv() const
    {
        return m_algo == ALGO_TAAODV;
    }
    /// What a node knows about one neighbour from its HELLOs.
    struct TaaNbr
    {
        bool seen{false};
        Time lastHello;
        Vector pos; //!< advertised position at posT
        Vector vel; //!< advertised velocity
        Time posT;
        std::set<Ipv4Address> cur;  //!< neighbour list in the latest HELLO
        std::set<Ipv4Address> prev; //!< neighbour list in the HELLO before it
        bool hasPrev{false};
    };
    /// Source-side path choice for one discovery (Eq. 27).
    struct TaaSel
    {
        double ratio{0.0};
        Time until;
        uint32_t seq{0};
    };
    void TaaSample();
    void TaaOnHello(Ipv4Address from, const TaaodvExtHeader& ext);
    std::vector<Ipv4Address> TaaLiveNeighbors() const;
    std::vector<Ipv4Address> TaaSelectForwarders(Ipv4Address prevHop);
    double TaaPheromoneDelta(Ipv4Address prevHop);
    void TaaMyKinematics(Vector& pos, Vector& vel) const;
    std::map<Ipv4Address, TaaNbr> m_taaNbr;
    std::map<Ipv4Address, TaaSel> m_taaSel;
    /// Requests this node has rebroadcast or answered; a later copy that names
    /// it is then ignored (lifetime set to the RREQ id cache's in SetAlgorithm).
    IdCache m_taaFwdCache{Seconds(5.6)};
    std::deque<double> m_taaQueueSamples;
    double m_taaQmax{500.0};
    bool m_taaSamplerStarted{false};
    double m_taaRange{141.0};
    Time m_taaSampleInterval{MilliSeconds(100)};
    uint32_t m_taaSamples{10};
    uint64_t m_taaFloodForward{0};      //!< 3 or fewer candidates: plain broadcast
    uint64_t m_taaRestrictedForward{0}; //!< RREQ sent naming its forwarders
    uint64_t m_taaForwardersNamed{0};   //!< sum of the forwarder list lengths
    uint64_t m_taaNotSelected{0};       //!< relay not named: processed, not rebroadcast
    uint64_t m_taaReselect{0};          //!< source moved to a later, better path
    uint64_t m_taaKept{0};              //!< later reply in the window not better
    uint64_t m_taaRssUnknown{0};        //!< no received power for the previous hop
    uint64_t m_taaLateNamed{0};         //!< rebroadcast on a later copy that named this node

    // Protocol parameters.
    uint32_t m_rreqRetries; ///< Maximum number of retransmissions of RREQ with TTL = NetDiameter to
                            ///< discover a route
    uint16_t m_ttlStart;    ///< Initial TTL value for RREQ.
    uint16_t m_ttlIncrement; ///< TTL increment for each attempt using the expanding ring search for
                             ///< RREQ dissemination.
    uint16_t m_ttlThreshold; ///< Maximum TTL value for expanding ring search, TTL = NetDiameter is
                             ///< used beyond this value.
    uint16_t m_timeoutBuffer;  ///< Provide a buffer for the timeout.
    uint16_t m_rreqRateLimit;  ///< Maximum number of RREQ per second.
    uint16_t m_rerrRateLimit;  ///< Maximum number of REER per second.
    Time m_activeRouteTimeout; ///< Period of time during which the route is considered to be valid.
    uint32_t m_netDiameter; ///< Net diameter measures the maximum possible number of hops between
                            ///< two nodes in the network
    /**
     * NodeTraversalTime is a conservative estimate of the average one hop traversal time for
     * packets and should include queuing delays, interrupt processing times and transfer times.
     */
    Time m_nodeTraversalTime;
    Time m_netTraversalTime;  ///< Estimate of the average net traversal time.
    Time m_pathDiscoveryTime; ///< Estimate of maximum time needed to find route in network.
    Time m_myRouteTimeout;    ///< Value of lifetime field in RREP generating by this node.
    /**
     * Every HelloInterval the node checks whether it has sent a broadcast  within the last
     * HelloInterval. If it has not, it MAY broadcast a  Hello message
     */
    Time m_helloInterval;
    uint32_t m_allowedHelloLoss; ///< Number of hello messages which may be loss for valid link
    /**
     * DeletePeriod is intended to provide an upper bound on the time for which an upstream node A
     * can have a neighbor B as an active next hop for destination D, while B has invalidated the
     * route to D.
     */
    Time m_deletePeriod;
    Time m_nextHopWait;      ///< Period of our waiting for the neighbour's RREP_ACK
    Time m_blackListTimeout; ///< Time for which the node is put into the blacklist
    uint32_t m_maxQueueLen;  ///< The maximum number of packets that we allow a routing protocol to
                             ///< buffer.
    Time m_maxQueueTime;     ///< The maximum period of time that a routing protocol is allowed to
                             ///< buffer a packet for.
    bool m_destinationOnly;  ///< Indicates only the destination may respond to this RREQ.
    bool m_gratuitousReply;  ///< Indicates whether a gratuitous RREP should be unicast to the node
                             ///< originated route discovery.
    bool m_enableHello;      ///< Indicates whether a hello messages enable
    bool m_enableBroadcast;  ///< Indicates whether a a broadcast data packets forwarding enable

    /// IP protocol
    Ptr<Ipv4> m_ipv4;
    /// Raw unicast socket per each IP interface, map socket -> iface address (IP + mask)
    std::map<Ptr<Socket>, Ipv4InterfaceAddress> m_socketAddresses;
    /// Raw subnet directed broadcast socket per each IP interface, map socket -> iface address (IP
    /// + mask)
    std::map<Ptr<Socket>, Ipv4InterfaceAddress> m_socketSubnetBroadcastAddresses;
    /// Loopback device used to defer RREQ until packet will be fully formed
    Ptr<NetDevice> m_lo;

    /// Routing table
    RoutingTable m_routingTable;
    /// A "drop-front" queue used by the routing layer to buffer packets to which it does not have a
    /// route.
    RequestQueue m_queue;
    /// Broadcast ID
    uint32_t m_requestId;
    /// Request sequence number
    uint32_t m_seqNo;
    /// Handle duplicated RREQ
    IdCache m_rreqIdCache;
    /// Handle duplicated broadcast/multicast packets
    DuplicatePacketDetection m_dpd;
    /// Handle neighbors
    Neighbors m_nb;
    /// Number of RREQs used for RREQ rate control
    uint16_t m_rreqCount;
    /// Number of RERRs used for RERR rate control
    uint16_t m_rerrCount;

  private:
    /// Start protocol operation
    void Start();
    /**
     * Queue packet and send route request
     *
     * @param p the packet to route
     * @param header the IP header
     * @param ucb the UnicastForwardCallback function
     * @param ecb the ErrorCallback function
     */
    void DeferredRouteOutput(Ptr<const Packet> p,
                             const Ipv4Header& header,
                             UnicastForwardCallback ucb,
                             ErrorCallback ecb);
    /**
     * If route exists and is valid, forward packet.
     *
     * @param p the packet to route
     * @param header the IP header
     * @param ucb the UnicastForwardCallback function
     * @param ecb the ErrorCallback function
     * @returns true if forwarded
     */
    bool Forwarding(Ptr<const Packet> p,
                    const Ipv4Header& header,
                    UnicastForwardCallback ucb,
                    ErrorCallback ecb);
    /**
     * Repeated attempts by a source node at route discovery for a single destination
     * use the expanding ring search technique.
     * @param dst the destination IP address
     */
    void ScheduleRreqRetry(Ipv4Address dst);
    /**
     * Set lifetime field in routing table entry to the maximum of existing lifetime and lt, if the
     * entry exists
     * @param addr destination address
     * @param lt proposed time for lifetime field in routing table entry for destination with
     * address addr.
     * @return true if route to destination address addr exist
     */
    bool UpdateRouteLifeTime(Ipv4Address addr, Time lt);
    /**
     * Update neighbor record.
     * @param receiver is supposed to be my interface
     * @param sender is supposed to be IP address of my neighbor.
     */
    void UpdateRouteToNeighbor(Ipv4Address sender, Ipv4Address receiver);
    /**
     * Test whether the provided address is assigned to an interface on this node
     * @param src the source IP address
     * @returns true if the IP address is the node's IP address
     */
    bool IsMyOwnAddress(Ipv4Address src);
    /**
     * Find unicast socket with local interface address iface
     *
     * @param iface the interface
     * @returns the socket associated with the interface
     */
    Ptr<Socket> FindSocketWithInterfaceAddress(Ipv4InterfaceAddress iface) const;
    /**
     * Find subnet directed broadcast socket with local interface address iface
     *
     * @param iface the interface
     * @returns the socket associated with the interface
     */
    Ptr<Socket> FindSubnetBroadcastSocketWithInterfaceAddress(Ipv4InterfaceAddress iface) const;
    /**
     * Process hello message
     *
     * @param rrepHeader RREP message header
     * @param receiverIfaceAddr receiver interface IP address
     */
    void ProcessHello(const RrepHeader& rrepHeader, Ipv4Address receiverIfaceAddr);
    /**
     * Create loopback route for given header
     *
     * @param header the IP header
     * @param oif the output interface net device
     * @returns the route
     */
    Ptr<Ipv4Route> LoopbackRoute(const Ipv4Header& header, Ptr<NetDevice> oif) const;

    /**
     * @name Receive control packets
     * @{
     */
    /**
     * Receive and process control packet
     * @param socket input socket
     */
    void RecvScr(Ptr<Socket> socket);
    /**
     * Receive RREQ
     * @param p packet
     * @param receiver receiver address
     * @param src sender address
     */
    void RecvRequest(Ptr<Packet> p, Ipv4Address receiver, Ipv4Address src);
    /**
     * Receive RREP
     * @param p packet
     * @param my destination address
     * @param src sender address
     */
    void RecvReply(Ptr<Packet> p, Ipv4Address my, Ipv4Address src);
    /**
     * Receive RREP_ACK
     * @param neighbor neighbor address
     */
    void RecvReplyAck(Ipv4Address neighbor);
    /**
     * Receive RERR
     * @param p packet
     * @param src sender address
     */
    /// Receive  from node with address src
    void RecvError(Ptr<Packet> p, Ipv4Address src);
    /** @} */

    /**
     * @name Send
     * @{
     */
    /** Forward packet from route request queue
     * @param dst destination address
     * @param route route to use
     */
    void SendPacketFromQueue(Ipv4Address dst, Ptr<Ipv4Route> route);
    /// Send hello
    void SendHello();
    /** Send RREQ
     * @param dst destination address
     */
    void SendRequest(Ipv4Address dst);
    /** Send RREP
     * @param rreqHeader route request header
     * @param toOrigin routing table entry to originator
     * @param taa TAAODV (R058) extension to append, or nullptr in every other mode
     */
    void SendReply(const RreqHeader& rreqHeader,
                   const RoutingTableEntry& toOrigin,
                   const TaaodvExtHeader* taa = nullptr);
    /** Send RREP by intermediate node
     * @param toDst routing table entry to destination
     * @param toOrigin routing table entry to originator
     * @param gratRep indicates whether a gratuitous RREP should be unicast to destination
     * @param taa TAAODV (R058) extension for the reply to the originator, or nullptr
     */
    void SendReplyByIntermediateNode(RoutingTableEntry& toDst,
                                     RoutingTableEntry& toOrigin,
                                     bool gratRep,
                                     const TaaodvExtHeader* taa = nullptr);
    /** Send RREP_ACK
     * @param neighbor neighbor address
     */
    void SendReplyAck(Ipv4Address neighbor);
    /** Initiate RERR
     * @param nextHop next hop address
     */
    void SendRerrWhenBreaksLinkToNextHop(Ipv4Address nextHop);
    /** Forward RERR
     * @param packet packet
     * @param precursors list of addresses of the visited nodes
     */
    void SendRerrMessage(Ptr<Packet> packet, std::vector<Ipv4Address> precursors);
    /**
     * Send RERR message when no route to forward input packet. Unicast if there is reverse route to
     * originating node, broadcast otherwise.
     * @param dst destination node IP address
     * @param dstSeqNo destination node sequence number
     * @param origin originating node IP address
     */
    void SendRerrWhenNoRouteToForward(Ipv4Address dst, uint32_t dstSeqNo, Ipv4Address origin);
    /** @} */

    /**
     * Send packet to destination socket
     * @param socket destination node socket
     * @param packet packet to send
     * @param destination destination node IP address
     */
    void SendTo(Ptr<Socket> socket, Ptr<Packet> packet, Ipv4Address destination);

    /// Hello timer
    Timer m_htimer;
    /// Schedule next send of hello message
    void HelloTimerExpire();
    /// RREQ rate limit timer
    // ---------------- SCR additions (E3-E6) ----------------
    // SCR: when false, every SCR path is bypassed and this fork must behave as
    // stock AODV. Fixture T9 asserts that toggling this actually changes routing
    // behaviour; T10 asserts that no RREQ reaches the wire without an admission.
    bool m_scrEnabled;
    /// Registered algorithm mode, emitted from C++ rather than inferred.
    AlgorithmMode m_algo;
    /// E5/E6 configuration applied to every pair ledger on this node.
    uint32_t m_scrB;
    double m_scrRho;
    double m_scrBucketRefill;

    /// SCR: per source-destination pair ledger (E3/E4/E5). Never erased: a
    /// tombstone must survive route deletion and application restart.
    std::map<Ipv4Address, RepairLedger> m_scrLedgers;

    /// SCR: node-wide origination budget shared across destinations (E6).
    OriginScheduler m_scrScheduler;

    /// SCR: per-destination remote service evidence (E2).
    std::map<Ipv4Address, Ptr<ServiceEvidence>> m_scrEvidence;

    /// SCR: destinations with an admitted-but-not-yet-handed-off origination,
    /// and the decision they were admitted under. Used to debit exactly once at
    /// the socket handoff boundary (E6), never at admission time.
    std::map<Ipv4Address, AdmitDecision> m_scrPendingSend;

    /// SCR: destinations with live application demand, set by the data path.
    std::set<Ipv4Address> m_scrDemand;

    /// SCR: at most ONE pending re-evaluation per destination.
    ///
    /// Without this, every token-starved denial scheduled a fresh ScrReconsider.
    /// Data demand arrives at (rate x flows) calls per second and each starved
    /// reconsider scheduled another, so the event queue grew without bound and
    /// runs slowed by more than 10x until they timed out. One pending event per
    /// destination is sufficient: it fires when a token is next available, and
    /// re-arms only if still starved.
    std::map<Ipv4Address, EventId> m_scrReconsiderEvent;

    /// SCR: dedicated RNG stream for scheduler jitter (D003). Replaces the
    /// native 0-10 ms draw so jitter is not applied twice.
    Ptr<UniformRandomVariable> m_scrJitter;
    /// D003 validating test: every value drawn from the dedicated jitter
    /// stream, in order, capped so a long run cannot grow it without bound.
    /// Observation only -- it consumes no randomness and changes no behaviour.
    std::vector<uint32_t> m_jitterDraws;
    /// D003: the stream index reserved for m_scrJitter, held until first use.
    /// -1 means AssignJitterStream was never called, which is an error under SCR.
    int64_t m_scrJitterStream;

    uint64_t m_scrAdmitted;      ///< SCR: originations admitted
    uint64_t m_scrDenied;        ///< SCR: origination attempts denied
    uint64_t m_scrWireRreq;      ///< SCR: RREQs that reached socket handoff
    uint64_t m_scrHandoffFailed; ///< SCR: immediate socket failures at handoff
    /// R014: retries the scheduler reported as due "now" while reporting no
    /// token. Clamped to one time unit to preserve liveness. A nonzero value is
    /// benign but must stay small; a large value means the node budget is being
    /// polled at time-unit granularity and the run is not trustworthy.
    uint64_t m_scrZeroDelayClamped;
    /// R028: entries to RouteRequestTimerExpire observed with RreqCnt already
    /// PAST m_rreqRetries. Under the fixed `>=` give-up test this can never
    /// happen, so the value must be zero in every run; it is nonzero only when
    /// RreqCapLegacyEquality is deliberately set to reproduce the pre-fix
    /// behaviour for blast-radius measurement.
    uint64_t m_rreqCapOvershot;
    /// R028: RREQ retry backoff shifts bounded at SCR_MAX_BACKOFF_FACTOR. Must
    /// stay zero once the cap bypass is fixed; nonzero means a route search ran
    /// away far enough to reach an absurd backoff.
    uint64_t m_rreqBackoffClamped;
    /// R028: largest RreqCnt this node ever observed at a give-up test. With
    /// the fix this is bounded by m_rreqRetries; it is the direct evidence of
    /// whether a run exercised the runaway path.
    uint32_t m_rreqMaxCnt;
    /// R032: admission denials broken down by reason. scr_denied is their sum.
    /// Observation only. Added because SCR-2 -- which cannot deny for allowance
    /// -- was still denied 31,497 times per run, and the total alone could not
    /// say why.
    uint64_t m_denyNoDemand;
    uint64_t m_denyInFlight;
    uint64_t m_denyNotEligible;
    /// Subset of m_denyNotEligible: the pair's outstanding request had ALREADY
    /// been answered, so the only thing holding it is the reply-timeout window
    /// set at send time. E4 bounds the wait FOR a reply; it specifies no
    /// cooldown after one arrives.
    uint64_t m_denyCooldownAfterReply;
    uint64_t m_denyNoAllowance;
    uint64_t m_denyNoNodeToken;
    uint64_t m_denyFifo;
    /// D002/R029: the interval most recently armed on the RREQ retry timer, and
    /// the TTL it was armed for. This timer IS the fork's reply timeout, so
    /// these two make T_wait observable to a fixture instead of only inferable
    /// from packet timing. D002 registered a validating assertion on exactly
    /// this quantity in 2026-09-07; it was never written, and the fork shipped
    /// with upstream's binary exponential backoff still in place.
    Time m_lastRreqRetryDelay;
    uint16_t m_lastRreqRetryTtl;
    /// R028 measurement switch. false (default) = fixed `>=` give-up test.
    /// true = the pre-fix `==` test, kept ONLY so an already-completed run can
    /// be replayed bit-for-bit to find out whether it hit the bug. It is
    /// reported in effective_parameters, so no run can use it unnoticed.
    bool m_rreqCapLegacyEquality;
    /// SCR (T10): RREQs that reached the send loop WITHOUT a matching
    /// admission record. Must be exactly zero in every SCR run; any nonzero
    /// value means an uncharged origination path still exists.
    uint64_t m_scrUnchargedRreq;
    /// Total source RREQ originations that reached the send loop, counted
    /// in BOTH modes so SCR and stock AODV control cost are comparable.
    uint64_t m_totalRreqOriginated;

    // ---- M20: all-hop control measurement (E10) ----
    // Source origination cost alone is NOT total control overhead. E10 and the
    // E8 network-multiplier argument both need relay-level counts, so every
    // control packet this node puts on the wire is counted by type, whether it
    // originated here or is being forwarded for someone else.
    uint64_t m_ctrlRreqRelayed;   //!< RREQs re-broadcast on behalf of others
    uint64_t m_ctrlRrepSent;      //!< RREPs sent (originated or forwarded)
    uint64_t m_ctrlRerrSent;      //!< RERRs sent
    uint64_t m_ctrlHelloSent;     //!< Hello broadcasts
    uint64_t m_ctrlBytesSent;     //!< serialized control bytes handed to sockets

    /// SCR: the single admission point. Every source origination path must call
    /// this and no other. Returns true if a request was handed to the socket.
    bool ScrTryDiscovery(Ipv4Address dst);

    /// SCR: get or create the ledger for a destination.
    RepairLedger& ScrLedger(Ipv4Address dst);

    /// SCR: build and hand off one RREQ with an explicit TTL. Debits the pair
    /// and node budgets atomically on successful handoff only.
    bool ScrOriginate(Ipv4Address dst, uint16_t ttl, AdmitDecision kind);

    /// SCR: re-evaluate a destination when it may have become eligible.
    void ScrReconsider(Ipv4Address dst);

  public:
    /// SCR: enable or disable the proposed mechanism.
    void SetScrEnabled(bool v) { m_scrEnabled = v; }
    /// Set the registered algorithm mode. Applies to all pairs on this node.
    void SetAlgorithm(AlgorithmMode a);
    AlgorithmMode GetAlgorithm() const { return m_algo; }
    /// Emit the instantiated mode and every effective parameter, from C++.
    std::string DumpEffectiveParameters() const;
    bool GetScrEnabled() const { return m_scrEnabled; }
    /// SCR: register live application demand for a destination.
    void ScrSetDemand(Ipv4Address dst, bool hasDemand);
    /// SCR: deliver a validated service confirmation for a destination.
    void ScrOnServiceConfirmed(Ipv4Address dst);
    /// Episode start for a destination, used to gate E2 confirmations.
    Time ScrGetEpisodeStart(Ipv4Address dst);
    /// Per-pair counters for reconciliation and traceability.
    uint64_t ScrGetRenewals(Ipv4Address dst);
    uint64_t ScrGetProbesSpent(Ipv4Address dst);
    uint64_t ScrGetAdmitted() const { return m_scrAdmitted; }
    uint64_t ScrGetDenied() const { return m_scrDenied; }
    uint64_t ScrGetWireRreq() const { return m_scrWireRreq; }
    /// SCR (T10): must be zero. Nonzero proves an unadmitted RREQ path.
    uint64_t ScrGetUnchargedRreq() const { return m_scrUnchargedRreq; }
    uint64_t ScrGetHandoffFailed() const { return m_scrHandoffFailed; }
    uint64_t ScrGetZeroDelayClamped() const { return m_scrZeroDelayClamped; }
    /// R028: must be zero unless RreqCapLegacyEquality was set on purpose.
    uint64_t GetRreqCapOvershot() const { return m_rreqCapOvershot; }
    /// R028: must be zero in a healthy run.
    uint64_t GetRreqBackoffClamped() const { return m_rreqBackoffClamped; }
    /// R028: largest RREQ retry count seen at a give-up test.
    uint32_t GetRreqMaxCnt() const { return m_rreqMaxCnt; }
    /// R032: denial breakdown, in order: no_demand, in_flight, not_eligible,
    /// cooldown_after_reply (subset of not_eligible), no_allowance,
    /// no_node_token, fifo.
    std::vector<uint64_t> GetDenyBreakdown() const
    {
        return {m_denyNoDemand, m_denyInFlight, m_denyNotEligible, m_denyCooldownAfterReply,
                m_denyNoAllowance, m_denyNoNodeToken, m_denyFifo};
    }
    /// D002: interval last armed on the RREQ retry timer (the fork's T_wait).
    Time GetLastRreqRetryDelay() const { return m_lastRreqRetryDelay; }
    /// D002: the TTL that interval was armed for.
    uint16_t GetLastRreqRetryTtl() const { return m_lastRreqRetryTtl; }
    /// D003: the dedicated-stream jitter values drawn so far, in order.
    const std::vector<uint32_t>& GetJitterDraws() const { return m_jitterDraws; }
    /// Source RREQ originations, valid in both SCR and stock modes.
    uint64_t GetTotalRreqOriginated() const { return m_totalRreqOriginated; }
    /// M20: control packets this node placed on the wire, by type.
    uint64_t GetCtrlRreqRelayed() const { return m_ctrlRreqRelayed; }
    uint64_t GetCtrlRrepSent() const { return m_ctrlRrepSent; }
    uint64_t GetCtrlRerrSent() const { return m_ctrlRerrSent; }
    uint64_t GetCtrlHelloSent() const { return m_ctrlHelloSent; }
    /// M20: total serialized control bytes handed to sockets by this node.
    uint64_t GetCtrlBytesSent() const { return m_ctrlBytesSent; }
    /// CARD (R053): break verdicts made at this node, in order: topology,
    /// transient, unknown; then RERR destinations received with the
    /// topology bit, and originations admitted as exempt.
    std::vector<uint64_t> GetCardCounters() const
    {
        return {m_cardBreakTopology, m_cardBreakTransient, m_cardBreakUnknown,
                m_cardRerrTopologyRx, m_cardExemptAdmitted};
    }
    /// CARD (R053): per-break log hook (self, neighbour, last dBm, trend dB,
    /// age s, verdict). Used by the scenarios to score the classifier against
    /// true geometry; it never influences the protocol.
    void SetCardBreakLog(Callback<void, Ipv4Address, Ipv4Address, double, double, double, uint8_t> cb)
    {
        m_cardBreakLog = cb;
    }
    /// CLAF-AODV (R057): pin the stream its forwarding draws use. Required in
    /// CLAF-AODV mode; creating the variable only here keeps every other mode's
    /// global stream allocation unchanged (the D003 lesson).
    void AssignClafStream(int64_t stream);
    /// CLAF-AODV (R057) counters, in order: sparse forward (ND below threshold),
    /// probabilistic forward, suppressed, phase-III rebroadcast, phase-III
    /// cancelled, phase-I drop, destination replies.
    std::vector<uint64_t> GetClafCounters() const
    {
        return {m_clafSparseForward, m_clafProbForward, m_clafSuppressed, m_clafPhase3Sent,
                m_clafPhase3Cancelled, m_clafPhase1Drop, m_clafDestReplies};
    }
    /// TAAODV (R058) counters, in order: plain-broadcast RREQs, RREQs naming
    /// forwarders, forwarders named in total, relays not named, source path
    /// switches, later replies kept out, previous hops with no received power,
    /// rebroadcasts on a later copy that named this node.
    std::vector<uint64_t> GetTaaodvCounters() const
    {
        return {m_taaFloodForward, m_taaRestrictedForward, m_taaForwardersNamed, m_taaNotSelected,
                m_taaReselect, m_taaKept, m_taaRssUnknown, m_taaLateNamed};
    }

  private:
    Timer m_rreqRateLimitTimer;
    /// Reset RREQ count and schedule RREQ rate limit timer with delay 1 sec.
    void RreqRateLimitTimerExpire();
    /// RERR rate limit timer
    Timer m_rerrRateLimitTimer;
    /// Reset RERR count and schedule RERR rate limit timer with delay 1 sec.
    void RerrRateLimitTimerExpire();
    /// Map IP address + RREQ timer.
    std::map<Ipv4Address, Timer> m_addressReqTimer;
    /**
     * Handle route discovery process
     * @param dst the destination IP address
     */
    void RouteRequestTimerExpire(Ipv4Address dst);
    /**
     * Mark link to neighbor node as unidirectional for blacklistTimeout
     *
     * @param neighbor the IP address of the neighbor node
     * @param blacklistTimeout the black list timeout time
     */
    void AckTimerExpire(Ipv4Address neighbor, Time blacklistTimeout);

    /// Provides uniform random variables.
    Ptr<UniformRandomVariable> m_uniformRandomVariable;
    /// Keep track of the last bcast time
    Time m_lastBcastTime;
};

} // namespace scr
} // namespace ns3

#endif /* SCRROUTINGPROTOCOL_H */
