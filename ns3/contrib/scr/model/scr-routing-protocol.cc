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


#include "scr-routing-protocol.h"

#include "no-discovery-tag.h"

#include <sstream>

#include "ns3/adhoc-wifi-mac.h"
#include "ns3/boolean.h"
#include "ns3/double.h"
#include "ns3/inet-socket-address.h"
#include "ns3/ipv4-header.h"
#include "ns3/llc-snap-header.h"
#include "ns3/log.h"
#include "ns3/mobility-model.h"
#include "ns3/pointer.h"
#include "ns3/random-variable-stream.h"
#include "ns3/string.h"
#include "ns3/trace-source-accessor.h"
#include "ns3/udp-header.h"
#include "ns3/udp-l4-protocol.h"
#include "ns3/udp-socket-factory.h"
#include "ns3/txop.h"
#include "ns3/wifi-mac-header.h"
#include "ns3/wifi-mac-queue.h"
#include "ns3/wifi-mpdu.h"
#include "ns3/wifi-phy-state-helper.h"
#include "ns3/wifi-net-device.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#undef NS_LOG_APPEND_CONTEXT
#define NS_LOG_APPEND_CONTEXT                                                                      \
    if (m_ipv4)                                                                                    \
    {                                                                                              \
        std::clog << "[node " << m_ipv4->GetObject<Node>()->GetId() << "] ";                       \
    }

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("ScrRoutingProtocol");

namespace scr
{
NS_OBJECT_ENSURE_REGISTERED(RoutingProtocol);

/// UDP Port for SCR control traffic
const uint32_t RoutingProtocol::SCR_PORT = 654;

/**
 * @ingroup scr
 * @brief Tag used by SCR implementation
 */
class DeferredRouteOutputTag : public Tag
{
  public:
    /**
     * @brief Constructor
     * @param o the output interface
     */
    DeferredRouteOutputTag(int32_t o = -1)
        : Tag(),
          m_oif(o)
    {
    }

    /**
     * @brief Get the type ID.
     * @return the object TypeId
     */
    static TypeId GetTypeId()
    {
        static TypeId tid = TypeId("ns3::scr::DeferredRouteOutputTag")
                                .SetParent<Tag>()
                                .SetGroupName("Scr")
                                .AddConstructor<DeferredRouteOutputTag>();
        return tid;
    }

    TypeId GetInstanceTypeId() const override
    {
        return GetTypeId();
    }

    /**
     * @brief Get the output interface
     * @return the output interface
     */
    int32_t GetInterface() const
    {
        return m_oif;
    }

    /**
     * @brief Set the output interface
     * @param oif the output interface
     */
    void SetInterface(int32_t oif)
    {
        m_oif = oif;
    }

    uint32_t GetSerializedSize() const override
    {
        return sizeof(int32_t);
    }

    void Serialize(TagBuffer i) const override
    {
        i.WriteU32(m_oif);
    }

    void Deserialize(TagBuffer i) override
    {
        m_oif = i.ReadU32();
    }

    void Print(std::ostream& os) const override
    {
        os << "DeferredRouteOutputTag: output interface = " << m_oif;
    }

  private:
    /// Positive if output device is fixed in RouteOutput
    int32_t m_oif;
};

NS_OBJECT_ENSURE_REGISTERED(DeferredRouteOutputTag);

//-----------------------------------------------------------------------------
/// R029: see DumpEffectiveParameters. Bump on every behaviour-affecting change.
static constexpr uint32_t SCR_FORK_REVISION = 10;

RoutingProtocol::RoutingProtocol()
    : m_rreqRetries(2),
      m_ttlStart(1),
      m_ttlIncrement(2),
      m_ttlThreshold(7),
      m_timeoutBuffer(2),
      m_rreqRateLimit(10),
      m_rerrRateLimit(10),
      m_activeRouteTimeout(Seconds(3)),
      m_netDiameter(35),
      m_nodeTraversalTime(MilliSeconds(40)),
      m_netTraversalTime(Time((2 * m_netDiameter) * m_nodeTraversalTime)),
      m_pathDiscoveryTime(Time(2 * m_netTraversalTime)),
      m_myRouteTimeout(Time(2 * std::max(m_pathDiscoveryTime, m_activeRouteTimeout))),
      m_helloInterval(Seconds(1)),
      m_allowedHelloLoss(2),
      m_deletePeriod(Time(5 * std::max(m_activeRouteTimeout, m_helloInterval))),
      m_nextHopWait(m_nodeTraversalTime + MilliSeconds(10)),
      m_blackListTimeout(Time(m_rreqRetries * m_netTraversalTime)),
      m_maxQueueLen(64),
      m_maxQueueTime(Seconds(30)),
      m_destinationOnly(false),
      m_gratuitousReply(true),
      m_enableHello(false),
      m_routingTable(m_deletePeriod),
      m_queue(m_maxQueueLen, m_maxQueueTime),
      m_requestId(0),
      m_seqNo(0),
      m_rreqIdCache(m_pathDiscoveryTime),
      m_dpd(m_pathDiscoveryTime),
      m_nb(m_helloInterval),
      m_scrEnabled(false),
      m_algo(ALGO_SCR),
      m_scrB(4),
      m_scrRho(0.2),
      m_scrBucketRefill(0.5),
      m_scrAdmitted(0),
      m_scrDenied(0),
      m_scrWireRreq(0),
      m_scrHandoffFailed(0),
      m_scrZeroDelayClamped(0),
      m_rreqCapOvershot(0),
      m_rreqBackoffClamped(0),
      m_rreqMaxCnt(0),
      m_denyNoDemand(0),
      m_denyInFlight(0),
      m_denyNotEligible(0),
      m_denyCooldownAfterReply(0),
      m_denyNoAllowance(0),
      m_denyNoNodeToken(0),
      m_denyFifo(0),
      m_scrJitterStream(-1),
      m_lastRreqRetryDelay(Seconds(0)),
      m_lastRreqRetryTtl(0),
      m_rreqCapLegacyEquality(false),
      m_scrUnchargedRreq(0),
      m_totalRreqOriginated(0),
      m_ctrlRreqRelayed(0),
      m_ctrlRrepSent(0),
      m_ctrlRerrSent(0),
      m_ctrlHelloSent(0),
      m_ctrlBytesSent(0),
      m_rreqCount(0),
      m_rerrCount(0),
      m_htimer(Timer::CANCEL_ON_DESTROY),
      m_rreqRateLimitTimer(Timer::CANCEL_ON_DESTROY),
      m_rerrRateLimitTimer(Timer::CANCEL_ON_DESTROY),
      m_lastBcastTime()
{
    // D003: m_scrJitter is deliberately NOT constructed here. See
    // AssignJitterStream: constructing a RandomVariableStream consumes an index
    // from ns-3's global stream allocator, so creating one unconditionally
    // shifts the stream of every component created afterwards -- which changed
    // AODV-STOCK's results by 403 on-time packets in a single test cell and
    // would have violated D002's requirement that the native references be left
    // untouched. It is created only for the modes that use it.
    m_nb.SetCallback(MakeCallback(&RoutingProtocol::SendRerrWhenBreaksLinkToNextHop, this));
}

TypeId
RoutingProtocol::GetTypeId()
{
    static TypeId tid =
        TypeId("ns3::scr::RoutingProtocol")
            .SetParent<Ipv4RoutingProtocol>()
            .SetGroupName("Scr")
            .AddConstructor<RoutingProtocol>()
            .AddAttribute("HelloInterval",
                          "HELLO messages emission interval.",
                          TimeValue(Seconds(1)),
                          MakeTimeAccessor(&RoutingProtocol::m_helloInterval),
                          MakeTimeChecker())
            .AddAttribute("TtlStart",
                          "Initial TTL value for RREQ.",
                          UintegerValue(1),
                          MakeUintegerAccessor(&RoutingProtocol::m_ttlStart),
                          MakeUintegerChecker<uint16_t>())
            .AddAttribute("TtlIncrement",
                          "TTL increment for each attempt using the expanding ring search for RREQ "
                          "dissemination.",
                          UintegerValue(2),
                          MakeUintegerAccessor(&RoutingProtocol::m_ttlIncrement),
                          MakeUintegerChecker<uint16_t>())
            .AddAttribute("TtlThreshold",
                          "Maximum TTL value for expanding ring search, TTL = NetDiameter is used "
                          "beyond this value.",
                          UintegerValue(7),
                          MakeUintegerAccessor(&RoutingProtocol::m_ttlThreshold),
                          MakeUintegerChecker<uint16_t>())
            .AddAttribute("TimeoutBuffer",
                          "Provide a buffer for the timeout.",
                          UintegerValue(2),
                          MakeUintegerAccessor(&RoutingProtocol::m_timeoutBuffer),
                          MakeUintegerChecker<uint16_t>())
            .AddAttribute("RreqRetries",
                          "Maximum number of retransmissions of RREQ to discover a route",
                          UintegerValue(2),
                          MakeUintegerAccessor(&RoutingProtocol::m_rreqRetries),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute("RreqCapLegacyEquality",
                          "R028 MEASUREMENT ONLY. Restore the pre-fix `==` RREQ "
                          "give-up test so an already-completed run can be "
                          "replayed to determine whether it hit the runaway "
                          "backoff. Never set this for a run that produces "
                          "results.",
                          BooleanValue(false),
                          MakeBooleanAccessor(&RoutingProtocol::m_rreqCapLegacyEquality),
                          MakeBooleanChecker())
            .AddAttribute("RreqRateLimit",
                          "Maximum number of RREQ per second.",
                          UintegerValue(10),
                          MakeUintegerAccessor(&RoutingProtocol::m_rreqRateLimit),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute("RerrRateLimit",
                          "Maximum number of RERR per second.",
                          UintegerValue(10),
                          MakeUintegerAccessor(&RoutingProtocol::m_rerrRateLimit),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute("NodeTraversalTime",
                          "Conservative estimate of the average one hop traversal time for packets "
                          "and should include "
                          "queuing delays, interrupt processing times and transfer times.",
                          TimeValue(MilliSeconds(40)),
                          MakeTimeAccessor(&RoutingProtocol::m_nodeTraversalTime),
                          MakeTimeChecker())
            .AddAttribute(
                "NextHopWait",
                "Period of our waiting for the neighbour's RREP_ACK = 10 ms + NodeTraversalTime",
                TimeValue(MilliSeconds(50)),
                MakeTimeAccessor(&RoutingProtocol::m_nextHopWait),
                MakeTimeChecker())
            .AddAttribute("ActiveRouteTimeout",
                          "Period of time during which the route is considered to be valid",
                          TimeValue(Seconds(3)),
                          MakeTimeAccessor(&RoutingProtocol::m_activeRouteTimeout),
                          MakeTimeChecker())
            .AddAttribute("MyRouteTimeout",
                          "Value of lifetime field in RREP generating by this node = 2 * "
                          "max(ActiveRouteTimeout, PathDiscoveryTime)",
                          TimeValue(Seconds(11.2)),
                          MakeTimeAccessor(&RoutingProtocol::m_myRouteTimeout),
                          MakeTimeChecker())
            .AddAttribute("BlackListTimeout",
                          "Time for which the node is put into the blacklist = RreqRetries * "
                          "NetTraversalTime",
                          TimeValue(Seconds(5.6)),
                          MakeTimeAccessor(&RoutingProtocol::m_blackListTimeout),
                          MakeTimeChecker())
            .AddAttribute("DeletePeriod",
                          "DeletePeriod is intended to provide an upper bound on the time for "
                          "which an upstream node A "
                          "can have a neighbor B as an active next hop for destination D, while B "
                          "has invalidated the route to D."
                          " = 5 * max (HelloInterval, ActiveRouteTimeout)",
                          TimeValue(Seconds(15)),
                          MakeTimeAccessor(&RoutingProtocol::m_deletePeriod),
                          MakeTimeChecker())
            .AddAttribute("NetDiameter",
                          "Net diameter measures the maximum possible number of hops between two "
                          "nodes in the network",
                          UintegerValue(35),
                          MakeUintegerAccessor(&RoutingProtocol::m_netDiameter),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute(
                "NetTraversalTime",
                "Estimate of the average net traversal time = 2 * NodeTraversalTime * NetDiameter",
                TimeValue(Seconds(2.8)),
                MakeTimeAccessor(&RoutingProtocol::m_netTraversalTime),
                MakeTimeChecker())
            .AddAttribute(
                "PathDiscoveryTime",
                "Estimate of maximum time needed to find route in network = 2 * NetTraversalTime",
                TimeValue(Seconds(5.6)),
                MakeTimeAccessor(&RoutingProtocol::m_pathDiscoveryTime),
                MakeTimeChecker())
            .AddAttribute("MaxQueueLen",
                          "Maximum number of packets that we allow a routing protocol to buffer.",
                          UintegerValue(64),
                          MakeUintegerAccessor(&RoutingProtocol::SetMaxQueueLen,
                                               &RoutingProtocol::GetMaxQueueLen),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute("MaxQueueTime",
                          "Maximum time packets can be queued (in seconds)",
                          TimeValue(Seconds(30)),
                          MakeTimeAccessor(&RoutingProtocol::SetMaxQueueTime,
                                           &RoutingProtocol::GetMaxQueueTime),
                          MakeTimeChecker())
            .AddAttribute("AllowedHelloLoss",
                          "Number of hello messages which may be loss for valid link.",
                          UintegerValue(2),
                          MakeUintegerAccessor(&RoutingProtocol::m_allowedHelloLoss),
                          MakeUintegerChecker<uint16_t>())
            .AddAttribute("GratuitousReply",
                          "Indicates whether a gratuitous RREP should be unicast to the node "
                          "originated route discovery.",
                          BooleanValue(true),
                          MakeBooleanAccessor(&RoutingProtocol::SetGratuitousReplyFlag,
                                              &RoutingProtocol::GetGratuitousReplyFlag),
                          MakeBooleanChecker())
            .AddAttribute("DestinationOnly",
                          "Indicates only the destination may respond to this RREQ.",
                          BooleanValue(false),
                          MakeBooleanAccessor(&RoutingProtocol::SetDestinationOnlyFlag,
                                              &RoutingProtocol::GetDestinationOnlyFlag),
                          MakeBooleanChecker())
            .AddAttribute("EnableHello",
                          "Indicates whether a hello messages enable.",
                          BooleanValue(true),
                          MakeBooleanAccessor(&RoutingProtocol::SetHelloEnable,
                                              &RoutingProtocol::GetHelloEnable),
                          MakeBooleanChecker())
            .AddAttribute("EnableBroadcast",
                          "Indicates whether a broadcast data packets forwarding enable.",
                          BooleanValue(true),
                          MakeBooleanAccessor(&RoutingProtocol::SetBroadcastEnable,
                                              &RoutingProtocol::GetBroadcastEnable),
                          MakeBooleanChecker())
            .AddAttribute("CardEdgeDbm",
                          "CARD (R053): a lost neighbour last heard below this received "
                          "power, with a falling signal, is judged to have left.",
                          DoubleValue(-79.0),
                          MakeDoubleAccessor(&RoutingProtocol::m_cardEdgeDbm),
                          MakeDoubleChecker<double>())
            .AddAttribute("CardTrendDb",
                          "CARD (R053): minimum fall in received power (dB) over the last "
                          "window of at least one second that counts as a falling signal.",
                          DoubleValue(0.1),
                          MakeDoubleAccessor(&RoutingProtocol::m_cardTrendDb),
                          MakeDoubleChecker<double>(0.0))
            .AddAttribute("ClafCounterC",
                          "CLAF-AODV (R057): copies of a suppressed RREQ that cancel its "
                          "phase-III rebroadcast. Not given by Safari et al.; 3 is the usual "
                          "counter-based threshold.",
                          UintegerValue(3),
                          MakeUintegerAccessor(&RoutingProtocol::m_clafCounterC),
                          MakeUintegerChecker<uint32_t>(1))
            .AddAttribute("ClafEwmaAlpha",
                          "CLAF-AODV (R057): EWMA weight for queue and contention window "
                          "(alpha in (0,1), value not given by Safari et al.).",
                          DoubleValue(0.5),
                          MakeDoubleAccessor(&RoutingProtocol::m_clafEwmaAlpha),
                          MakeDoubleChecker<double>(0.0, 1.0))
            .AddAttribute("ClafDestTimer",
                          "CLAF-AODV (R057): how long the destination collects RREQ copies "
                          "before replying to the best (duration not given by Safari et al.; "
                          "one node traversal time).",
                          TimeValue(MilliSeconds(40)),
                          MakeTimeAccessor(&RoutingProtocol::m_clafDestTimer),
                          MakeTimeChecker())
            .AddAttribute("ClafNdThreshold",
                          "CLAF-AODV (R057): node degree below which a RREQ is always "
                          "forwarded (ND_thr = 6 in Safari et al.).",
                          UintegerValue(6),
                          MakeUintegerAccessor(&RoutingProtocol::m_clafNdThreshold),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute("TaaodvRange",
                          "TAAODV (R058): maximum communication distance R (m) in the link "
                          "lifetime model (Li et al., Eqs. 9-12); our range edge.",
                          DoubleValue(141.0),
                          MakeDoubleAccessor(&RoutingProtocol::m_taaRange),
                          MakeDoubleChecker<double>(1.0))
            .AddAttribute("TaaodvQueueSampleInterval",
                          "TAAODV (R058): period of the queue-occupancy measurements averaged "
                          "into the load ratio Cn (Eq. 20; not given by Li et al.).",
                          TimeValue(MilliSeconds(100)),
                          MakeTimeAccessor(&RoutingProtocol::m_taaSampleInterval),
                          MakeTimeChecker())
            .AddAttribute("TaaodvQueueSamples",
                          "TAAODV (R058): number N of measurements averaged into Cn "
                          "(Eq. 20; not given by Li et al.).",
                          UintegerValue(10),
                          MakeUintegerAccessor(&RoutingProtocol::m_taaSamples),
                          MakeUintegerChecker<uint32_t>(1))
            .AddAttribute("UniformRv",
                          "Access to the underlying UniformRandomVariable",
                          StringValue("ns3::UniformRandomVariable"),
                          MakePointerAccessor(&RoutingProtocol::m_uniformRandomVariable),
                          MakePointerChecker<UniformRandomVariable>());
    return tid;
}

void
RoutingProtocol::SetMaxQueueLen(uint32_t len)
{
    m_maxQueueLen = len;
    m_queue.SetMaxQueueLen(len);
}

void
RoutingProtocol::SetMaxQueueTime(Time t)
{
    m_maxQueueTime = t;
    m_queue.SetQueueTimeout(t);
}

RoutingProtocol::~RoutingProtocol()
{
}

void
RoutingProtocol::DoDispose()
{
    m_ipv4 = nullptr;
    for (auto iter = m_socketAddresses.begin(); iter != m_socketAddresses.end(); iter++)
    {
        iter->first->Close();
    }
    m_socketAddresses.clear();
    for (auto iter = m_socketSubnetBroadcastAddresses.begin();
         iter != m_socketSubnetBroadcastAddresses.end();
         iter++)
    {
        iter->first->Close();
    }
    m_socketSubnetBroadcastAddresses.clear();
    Ipv4RoutingProtocol::DoDispose();
}

void
RoutingProtocol::PrintRoutingTable(Ptr<OutputStreamWrapper> stream, Time::Unit unit) const
{
    *stream->GetStream() << "Node: " << m_ipv4->GetObject<Node>()->GetId()
                         << "; Time: " << Now().As(unit)
                         << ", Local time: " << m_ipv4->GetObject<Node>()->GetLocalTime().As(unit)
                         << ", SCR Routing table" << std::endl;

    m_routingTable.Print(stream, unit);
    *stream->GetStream() << std::endl;
}

int64_t
RoutingProtocol::AssignStreams(int64_t stream)
{
    NS_LOG_FUNCTION(this << stream);
    m_uniformRandomVariable->SetStream(stream);
    return 1;
}

void
RoutingProtocol::AssignJitterStream(int64_t stream)
{
    NS_LOG_FUNCTION(this << stream);

    // D003 + D002 together. The fork family gets a dedicated jitter stream; the
    // native references must be bit-for-bit unchanged. Those two requirements
    // collide in ns-3, because merely CONSTRUCTING a RandomVariableStream draws
    // the next index from the global allocator and shifts every later
    // allocation. Creating m_scrJitter for every mode therefore perturbed
    // AODV-STOCK and AODV-FB even though neither ever read it.
    //
    // So the object is created only when this node's mode will actually use it.
    // A native mode leaves the allocator untouched and reproduces its earlier
    // results exactly, which is what keeps the native-path runs already in the
    // campaign valid.
    // The stream is RESERVED here and the object created on first use, because
    // the algorithm mode is not yet set when the scenario assigns streams --
    // gating on m_scrEnabled here created nothing and the guard at the use site
    // fired, which is what that guard is for. Creating it lazily also keeps a
    // native-mode node from ever touching the global allocator.
    m_scrJitterStream = stream;
}

void
RoutingProtocol::Start()
{
    NS_LOG_FUNCTION(this);
    if (m_enableHello)
    {
        m_nb.ScheduleTimer();
    }
    m_rreqRateLimitTimer.SetFunction(&RoutingProtocol::RreqRateLimitTimerExpire, this);
    m_rreqRateLimitTimer.Schedule(Seconds(1));

    m_rerrRateLimitTimer.SetFunction(&RoutingProtocol::RerrRateLimitTimerExpire, this);
    m_rerrRateLimitTimer.Schedule(Seconds(1));
}

Ptr<Ipv4Route>
RoutingProtocol::RouteOutput(Ptr<Packet> p,
                             const Ipv4Header& header,
                             Ptr<NetDevice> oif,
                             Socket::SocketErrno& sockerr)
{
    NS_LOG_FUNCTION(this << header << (oif ? oif->GetIfIndex() : 0));
    if (!p)
    {
        NS_LOG_DEBUG("Packet is == 0");
        return LoopbackRoute(header, oif); // later
    }
    if (m_socketAddresses.empty())
    {
        sockerr = Socket::ERROR_NOROUTETOHOST;
        NS_LOG_LOGIC("No scr interfaces");
        Ptr<Ipv4Route> route;
        return route;
    }
    sockerr = Socket::ERROR_NOTERROR;
    Ptr<Ipv4Route> route;
    Ipv4Address dst = header.GetDestination();
    RoutingTableEntry rt;
    if (m_routingTable.LookupValidRoute(dst, rt))
    {
        route = rt.GetRoute();
        NS_ASSERT(route);
        NS_LOG_DEBUG("Exist route to " << route->GetDestination() << " from interface "
                                       << route->GetSource());
        if (oif && route->GetOutputDevice() != oif)
        {
            NS_LOG_DEBUG("Output device doesn't match. Dropped.");
            sockerr = Socket::ERROR_NOROUTETOHOST;
            return Ptr<Ipv4Route>();
        }
        UpdateRouteLifeTime(dst, m_activeRouteTimeout);
        UpdateRouteLifeTime(route->GetGateway(), m_activeRouteTimeout);
        return route;
    }

    // SCR (M16): a packet carrying NoDiscoveryTag must NEVER cause a reverse
    // route discovery. Feedback is allowed to use an already-valid route only.
    // The receiving application also checks route usability before sending, but
    // that check alone is insufficient because the route can change between the
    // check and actual output - so this routing-level guard is mandatory.
    {
        NoDiscoveryTag ndTag;
        if (p->PeekPacketTag(ndTag))
        {
            NS_LOG_LOGIC("SCR: NoDiscovery packet with no valid route; error return, "
                         "no reverse-route RREQ enqueued");
            sockerr = Socket::ERROR_NOROUTETOHOST;
            return Ptr<Ipv4Route>();
        }
    }

    // Valid route not found, in this case we return loopback.
    // Actual route request will be deferred until packet will be fully formed,
    // routed to loopback, received from loopback and passed to RouteInput (see below)
    uint32_t iif = (oif ? m_ipv4->GetInterfaceForDevice(oif) : -1);
    DeferredRouteOutputTag tag(iif);
    NS_LOG_DEBUG("Valid Route not found");
    if (!p->PeekPacketTag(tag))
    {
        p->AddPacketTag(tag);
    }
    return LoopbackRoute(header, oif);
}

void
RoutingProtocol::DeferredRouteOutput(Ptr<const Packet> p,
                                     const Ipv4Header& header,
                                     UnicastForwardCallback ucb,
                                     ErrorCallback ecb)
{
    NS_LOG_FUNCTION(this << p << header);
    NS_ASSERT(p && p != Ptr<Packet>());

    QueueEntry newEntry(p, header, ucb, ecb);
    bool result = m_queue.Enqueue(newEntry);
    if (result)
    {
        NS_LOG_LOGIC("Add packet " << p->GetUid() << " to queue. Protocol "
                                   << (uint16_t)header.GetProtocol());
        // SCR: the algorithm contract's OnDataDemand requires that EVERY data
        // demand with no valid route reaches TryDiscovery. The native IN_SEARCH
        // guard below is AODV's own duplicate suppression; under SCR that role
        // is played by the ledger's in_flight check, and leaving the native
        // guard in place would hide most demand from the scheduler - so probe
        // opportunities would be silently missed and E9's anti-lockout property
        // would not hold.
        if (m_scrEnabled)
        {
            m_scrDemand.insert(header.GetDestination());
            ScrTryDiscovery(header.GetDestination());
        }
        else
        {
            RoutingTableEntry rt;
            bool result = m_routingTable.LookupRoute(header.GetDestination(), rt);
            if (!result || ((rt.GetFlag() != IN_SEARCH) && result))
            {
                NS_LOG_LOGIC("Send new RREQ for outbound packet to "
                             << header.GetDestination());
                SendRequest(header.GetDestination());
            }
        }
    }
}

bool
RoutingProtocol::RouteInput(Ptr<const Packet> p,
                            const Ipv4Header& header,
                            Ptr<const NetDevice> idev,
                            const UnicastForwardCallback& ucb,
                            const MulticastForwardCallback& mcb,
                            const LocalDeliverCallback& lcb,
                            const ErrorCallback& ecb)
{
    NS_LOG_FUNCTION(this << p->GetUid() << header.GetDestination() << idev->GetAddress());
    if (m_socketAddresses.empty())
    {
        NS_LOG_LOGIC("No scr interfaces");
        return false;
    }
    NS_ASSERT(m_ipv4);
    NS_ASSERT(p);
    // Check if input device supports IP
    NS_ASSERT(m_ipv4->GetInterfaceForDevice(idev) >= 0);
    int32_t iif = m_ipv4->GetInterfaceForDevice(idev);

    Ipv4Address dst = header.GetDestination();
    Ipv4Address origin = header.GetSource();

    // Deferred route request
    if (idev == m_lo)
    {
        DeferredRouteOutputTag tag;
        if (p->PeekPacketTag(tag))
        {
            DeferredRouteOutput(p, header, ucb, ecb);
            return true;
        }
    }

    // Duplicate of own packet
    if (IsMyOwnAddress(origin))
    {
        return true;
    }

    // SCR is not a multicast routing protocol
    if (dst.IsMulticast())
    {
        return false;
    }

    // Broadcast local delivery/forwarding
    for (auto j = m_socketAddresses.begin(); j != m_socketAddresses.end(); ++j)
    {
        Ipv4InterfaceAddress iface = j->second;
        if (m_ipv4->GetInterfaceForAddress(iface.GetLocal()) == iif)
        {
            if (dst == iface.GetBroadcast() || dst.IsBroadcast())
            {
                if (m_dpd.IsDuplicate(p, header))
                {
                    NS_LOG_DEBUG("Duplicated packet " << p->GetUid() << " from " << origin
                                                      << ". Drop.");
                    return true;
                }
                UpdateRouteLifeTime(origin, m_activeRouteTimeout);
                Ptr<Packet> packet = p->Copy();
                if (!lcb.IsNull())
                {
                    NS_LOG_LOGIC("Broadcast local delivery to " << iface.GetLocal());
                    lcb(p, header, iif);
                    // Fall through to additional processing
                }
                else
                {
                    NS_LOG_ERROR("Unable to deliver packet locally due to null callback "
                                 << p->GetUid() << " from " << origin);
                    ecb(p, header, Socket::ERROR_NOROUTETOHOST);
                }
                if (!m_enableBroadcast)
                {
                    return true;
                }
                if (header.GetProtocol() == UdpL4Protocol::PROT_NUMBER)
                {
                    UdpHeader udpHeader;
                    p->PeekHeader(udpHeader);
                    if (udpHeader.GetDestinationPort() == SCR_PORT)
                    {
                        // SCR packets sent in broadcast are already managed
                        return true;
                    }
                }
                if (header.GetTtl() > 1)
                {
                    NS_LOG_LOGIC("Forward broadcast. TTL " << (uint16_t)header.GetTtl());
                    RoutingTableEntry toBroadcast;
                    if (m_routingTable.LookupRoute(dst, toBroadcast))
                    {
                        Ptr<Ipv4Route> route = toBroadcast.GetRoute();
                        ucb(route, packet, header);
                    }
                    else
                    {
                        NS_LOG_DEBUG("No route to forward broadcast. Drop packet " << p->GetUid());
                    }
                }
                else
                {
                    NS_LOG_DEBUG("TTL exceeded. Drop packet " << p->GetUid());
                }
                return true;
            }
        }
    }

    // Unicast local delivery
    if (m_ipv4->IsDestinationAddress(dst, iif))
    {
        UpdateRouteLifeTime(origin, m_activeRouteTimeout);
        RoutingTableEntry toOrigin;
        if (m_routingTable.LookupValidRoute(origin, toOrigin))
        {
            UpdateRouteLifeTime(toOrigin.GetNextHop(), m_activeRouteTimeout);
            m_nb.Update(toOrigin.GetNextHop(), m_activeRouteTimeout);
        }
        if (!lcb.IsNull())
        {
            NS_LOG_LOGIC("Unicast local delivery to " << dst);
            lcb(p, header, iif);
        }
        else
        {
            NS_LOG_ERROR("Unable to deliver packet locally due to null callback "
                         << p->GetUid() << " from " << origin);
            ecb(p, header, Socket::ERROR_NOROUTETOHOST);
        }
        return true;
    }

    // Check if input device supports IP forwarding
    if (!m_ipv4->IsForwarding(iif))
    {
        NS_LOG_LOGIC("Forwarding disabled for this interface");
        ecb(p, header, Socket::ERROR_NOROUTETOHOST);
        return true;
    }

    // Forwarding
    return Forwarding(p, header, ucb, ecb);
}

bool
RoutingProtocol::Forwarding(Ptr<const Packet> p,
                            const Ipv4Header& header,
                            UnicastForwardCallback ucb,
                            ErrorCallback ecb)
{
    NS_LOG_FUNCTION(this);
    Ipv4Address dst = header.GetDestination();
    Ipv4Address origin = header.GetSource();
    m_routingTable.Purge();
    RoutingTableEntry toDst;
    if (m_routingTable.LookupRoute(dst, toDst))
    {
        if (toDst.GetFlag() == VALID)
        {
            Ptr<Ipv4Route> route = toDst.GetRoute();
            NS_LOG_LOGIC(route->GetSource() << " forwarding to " << dst << " from " << origin
                                            << " packet " << p->GetUid());

            /*
             *  Each time a route is used to forward a data packet, its Active Route
             *  Lifetime field of the source, destination and the next hop on the
             *  path to the destination is updated to be no less than the current
             *  time plus ActiveRouteTimeout.
             */
            UpdateRouteLifeTime(origin, m_activeRouteTimeout);
            UpdateRouteLifeTime(dst, m_activeRouteTimeout);
            UpdateRouteLifeTime(route->GetGateway(), m_activeRouteTimeout);
            /*
             *  Since the route between each originator and destination pair is expected to be
             * symmetric, the Active Route Lifetime for the previous hop, along the reverse path
             * back to the IP source, is also updated to be no less than the current time plus
             * ActiveRouteTimeout
             */
            RoutingTableEntry toOrigin;
            m_routingTable.LookupRoute(origin, toOrigin);
            UpdateRouteLifeTime(toOrigin.GetNextHop(), m_activeRouteTimeout);

            m_nb.Update(route->GetGateway(), m_activeRouteTimeout);
            m_nb.Update(toOrigin.GetNextHop(), m_activeRouteTimeout);

            ucb(route, p, header);
            return true;
        }
        else
        {
            if (toDst.GetValidSeqNo())
            {
                SendRerrWhenNoRouteToForward(dst, toDst.GetSeqNo(), origin);
                NS_LOG_DEBUG("Drop packet " << p->GetUid() << " because no route to forward it.");
                return false;
            }
        }
    }
    NS_LOG_LOGIC("route not found to " << dst << ". Send RERR message.");
    NS_LOG_DEBUG("Drop packet " << p->GetUid() << " because no route to forward it.");
    SendRerrWhenNoRouteToForward(dst, 0, origin);
    return false;
}

void
RoutingProtocol::SetIpv4(Ptr<Ipv4> ipv4)
{
    NS_ASSERT(ipv4);
    NS_ASSERT(!m_ipv4);

    m_ipv4 = ipv4;

    // Create lo route. It is asserted that the only one interface up for now is loopback
    NS_ASSERT(m_ipv4->GetNInterfaces() == 1 &&
              m_ipv4->GetAddress(0, 0).GetLocal() == Ipv4Address("127.0.0.1"));
    m_lo = m_ipv4->GetNetDevice(0);
    NS_ASSERT(m_lo);
    // Remember lo route
    RoutingTableEntry rt(
        /*dev=*/m_lo,
        /*dst=*/Ipv4Address::GetLoopback(),
        /*vSeqNo=*/true,
        /*seqNo=*/0,
        /*iface=*/Ipv4InterfaceAddress(Ipv4Address::GetLoopback(), Ipv4Mask("255.0.0.0")),
        /*hops=*/1,
        /*nextHop=*/Ipv4Address::GetLoopback(),
        /*lifetime=*/Simulator::GetMaximumSimulationTime());
    m_routingTable.AddRoute(rt);

    Simulator::ScheduleNow(&RoutingProtocol::Start, this);
}

void
RoutingProtocol::NotifyInterfaceUp(uint32_t i)
{
    NS_LOG_FUNCTION(this << m_ipv4->GetAddress(i, 0).GetLocal());
    Ptr<Ipv4L3Protocol> l3 = m_ipv4->GetObject<Ipv4L3Protocol>();
    if (l3->GetNAddresses(i) > 1)
    {
        NS_LOG_WARN("SCR does not work with more then one address per each interface.");
    }
    Ipv4InterfaceAddress iface = l3->GetAddress(i, 0);
    if (iface.GetLocal() == Ipv4Address("127.0.0.1"))
    {
        return;
    }

    // Create a socket to listen only on this interface
    Ptr<Socket> socket = Socket::CreateSocket(GetObject<Node>(), UdpSocketFactory::GetTypeId());
    NS_ASSERT(socket);
    socket->SetRecvCallback(MakeCallback(&RoutingProtocol::RecvScr, this));
    socket->BindToNetDevice(l3->GetNetDevice(i));
    socket->Bind(InetSocketAddress(iface.GetLocal(), SCR_PORT));
    socket->SetAllowBroadcast(true);
    socket->SetIpRecvTtl(true);
    m_socketAddresses.insert(std::make_pair(socket, iface));

    // create also a subnet broadcast socket
    socket = Socket::CreateSocket(GetObject<Node>(), UdpSocketFactory::GetTypeId());
    NS_ASSERT(socket);
    socket->SetRecvCallback(MakeCallback(&RoutingProtocol::RecvScr, this));
    socket->BindToNetDevice(l3->GetNetDevice(i));
    socket->Bind(InetSocketAddress(iface.GetBroadcast(), SCR_PORT));
    socket->SetAllowBroadcast(true);
    socket->SetIpRecvTtl(true);
    m_socketSubnetBroadcastAddresses.insert(std::make_pair(socket, iface));

    // Add local broadcast record to the routing table
    Ptr<NetDevice> dev = m_ipv4->GetNetDevice(m_ipv4->GetInterfaceForAddress(iface.GetLocal()));
    RoutingTableEntry rt(/*dev=*/dev,
                         /*dst=*/iface.GetBroadcast(),
                         /*vSeqNo=*/true,
                         /*seqNo=*/0,
                         /*iface=*/iface,
                         /*hops=*/1,
                         /*nextHop=*/iface.GetBroadcast(),
                         /*lifetime=*/Simulator::GetMaximumSimulationTime());
    m_routingTable.AddRoute(rt);

    if (l3->GetInterface(i)->GetArpCache())
    {
        m_nb.AddArpCache(l3->GetInterface(i)->GetArpCache());
    }

    // Allow neighbor manager use this interface for layer 2 feedback if possible
    Ptr<WifiNetDevice> wifi = dev->GetObject<WifiNetDevice>();
    if (!wifi)
    {
        return;
    }
    Ptr<WifiMac> mac = wifi->GetMac();
    if (!mac)
    {
        return;
    }

    mac->TraceConnectWithoutContext("DroppedMpdu",
                                    MakeCallback(&RoutingProtocol::NotifyTxError, this));
    // CARD (R053): received-signal records for break classification. The
    // callback returns at once outside the CARD modes; it draws no random
    // numbers and changes no state there, so the other modes are unaffected.
    if (wifi->GetPhy())
    {
        wifi->GetPhy()->TraceConnectWithoutContext(
            "MonitorSnifferRx",
            MakeCallback(&RoutingProtocol::CardSnifferRx, this));
        // R057: CLAF-AODV's idle-channel input. Returns at once in other modes.
        wifi->GetPhy()->GetState()->TraceConnectWithoutContext(
            "State",
            MakeCallback(&RoutingProtocol::ClafPhyState, this));
        m_clafPhy = wifi->GetPhy();
    }
    m_clafMac = mac;
}

void
RoutingProtocol::NotifyTxError(WifiMacDropReason reason, Ptr<const WifiMpdu> mpdu)
{
    m_nb.GetTxErrorCallback()(mpdu->GetHeader());
}

void
RoutingProtocol::CardSnifferRx(Ptr<const Packet> packet,
                               uint16_t /* channelFreqMhz */,
                               WifiTxVector /* txVector */,
                               MpduInfo /* aMpdu */,
                               SignalNoiseDbm signalNoise,
                               uint16_t /* staId */)
{
    if (!IsCardMode(m_algo) && !IsClaf() && !IsTaaodv())
    {
        return;
    }
    WifiMacHeader hdr;
    if (packet->PeekHeader(hdr) == 0 || !(hdr.IsData() || hdr.IsMgt()))
    {
        // Control frames (ACK) carry no transmitter address.
        return;
    }
    const Mac48Address from = hdr.GetAddr2();
    const Time now = Simulator::Now();
    const double dbm = signalNoise.signal;
    if (hdr.IsData() && hdr.GetAddr1().IsBroadcast())
    {
        // Learn the transmitter's IP from its own AODV broadcasts (hello, RREQ).
        // Their IP source is the transmitting node, unlike forwarded data,
        // whose IP source is the flow's origin.
        Ptr<Packet> c = packet->Copy();
        WifiMacHeader h;
        LlcSnapHeader llc;
        Ipv4Header ip;
        UdpHeader udp;
        c->RemoveHeader(h);
        if (c->GetSize() >= llc.GetSerializedSize() && c->RemoveHeader(llc) &&
            llc.GetType() == Ipv4L3Protocol::PROT_NUMBER && c->GetSize() >= 20 &&
            c->RemoveHeader(ip) && ip.GetProtocol() == UdpL4Protocol::PROT_NUMBER &&
            c->GetSize() >= 8 && c->PeekHeader(udp) && udp.GetDestinationPort() == SCR_PORT)
        {
            m_cardIpMac[ip.GetSource()] = from;
        }
    }
    auto it = m_cardObs.find(from);
    if (it == m_cardObs.end())
    {
        CardLinkObs o;
        o.curDbm = o.anchorDbm = dbm;
        o.curT = o.anchorT = now;
        m_cardObs.emplace(from, o);
        return;
    }
    CardLinkObs& o = it->second;
    o.curDbm = dbm;
    o.curT = now;
    if (now - o.anchorT >= Seconds(1))
    {
        // Close a window of at least one second. The trend is read across the
        // previous anchor and the latest frame, so it always spans >= 1 s.
        o.prevDbm = o.anchorDbm;
        o.prevT = o.anchorT;
        o.hasPrev = true;
        o.anchorDbm = dbm;
        o.anchorT = now;
    }
}

RoutingProtocol::CardCause
RoutingProtocol::CardClassify(Ipv4Address neighbor, double& dbm, double& trendDb, double& ageS)
{
    dbm = std::nan("");
    trendDb = std::nan("");
    ageS = std::nan("");
    Mac48Address mac = m_nb.GetMacAddress(neighbor);
    if (mac == Mac48Address())
    {
        auto learned = m_cardIpMac.find(neighbor);
        if (learned != m_cardIpMac.end())
        {
            mac = learned->second;
        }
    }
    auto it = m_cardObs.find(mac);
    if (mac == Mac48Address() || it == m_cardObs.end())
    {
        return CARD_UNKNOWN;
    }
    const CardLinkObs& o = it->second;
    dbm = o.curDbm;
    ageS = (Simulator::Now() - o.curT).GetSeconds();
    if (!o.hasPrev)
    {
        return CARD_TRANSIENT;
    }
    trendDb = o.curDbm - o.prevDbm;
    if (o.curDbm < m_cardEdgeDbm && trendDb < -m_cardTrendDb)
    {
        return CARD_TOPOLOGY;
    }
    return CARD_TRANSIENT;
}

bool
RoutingProtocol::CardReportsTopology(CardCause c) const
{
    if (m_algo == ALGO_CARD_EXEMPT_ALL)
    {
        return true;
    }
    return CardUsesCause(m_algo) && c == CARD_TOPOLOGY;
}

void
RoutingProtocol::CardRecordCause(Ipv4Address dst, bool topology)
{
    // The most recent report wins: a transient break after a topology change
    // is rationed again.
    m_cardCause[dst] = std::make_pair(topology, Simulator::Now());
}

void
RoutingProtocol::NotifyInterfaceDown(uint32_t i)
{
    NS_LOG_FUNCTION(this << m_ipv4->GetAddress(i, 0).GetLocal());

    // Disable layer 2 link state monitoring (if possible)
    Ptr<Ipv4L3Protocol> l3 = m_ipv4->GetObject<Ipv4L3Protocol>();
    Ptr<NetDevice> dev = l3->GetNetDevice(i);
    Ptr<WifiNetDevice> wifi = dev->GetObject<WifiNetDevice>();
    if (wifi)
    {
        Ptr<WifiMac> mac = wifi->GetMac()->GetObject<AdhocWifiMac>();
        if (mac)
        {
            mac->TraceDisconnectWithoutContext("DroppedMpdu",
                                               MakeCallback(&RoutingProtocol::NotifyTxError, this));
            if (wifi->GetPhy())
            {
                // Mirror NotifyInterfaceUp, so an outage that cycles the
                // interface does not stack duplicate callbacks.
                wifi->GetPhy()->TraceDisconnectWithoutContext(
                    "MonitorSnifferRx",
                    MakeCallback(&RoutingProtocol::CardSnifferRx, this));
                wifi->GetPhy()->GetState()->TraceDisconnectWithoutContext(
                    "State",
                    MakeCallback(&RoutingProtocol::ClafPhyState, this));
            }
            m_nb.DelArpCache(l3->GetInterface(i)->GetArpCache());
        }
    }

    // Close socket
    Ptr<Socket> socket = FindSocketWithInterfaceAddress(m_ipv4->GetAddress(i, 0));
    NS_ASSERT(socket);
    socket->Close();
    m_socketAddresses.erase(socket);

    // Close socket
    socket = FindSubnetBroadcastSocketWithInterfaceAddress(m_ipv4->GetAddress(i, 0));
    NS_ASSERT(socket);
    socket->Close();
    m_socketSubnetBroadcastAddresses.erase(socket);

    if (m_socketAddresses.empty())
    {
        NS_LOG_LOGIC("No scr interfaces");
        m_htimer.Cancel();
        m_nb.Clear();
        m_routingTable.Clear();
        return;
    }
    m_routingTable.DeleteAllRoutesFromInterface(m_ipv4->GetAddress(i, 0));
}

void
RoutingProtocol::NotifyAddAddress(uint32_t i, Ipv4InterfaceAddress address)
{
    NS_LOG_FUNCTION(this << " interface " << i << " address " << address);
    Ptr<Ipv4L3Protocol> l3 = m_ipv4->GetObject<Ipv4L3Protocol>();
    if (!l3->IsUp(i))
    {
        return;
    }
    if (l3->GetNAddresses(i) == 1)
    {
        Ipv4InterfaceAddress iface = l3->GetAddress(i, 0);
        Ptr<Socket> socket = FindSocketWithInterfaceAddress(iface);
        if (!socket)
        {
            if (iface.GetLocal() == Ipv4Address("127.0.0.1"))
            {
                return;
            }
            // Create a socket to listen only on this interface
            Ptr<Socket> socket =
                Socket::CreateSocket(GetObject<Node>(), UdpSocketFactory::GetTypeId());
            NS_ASSERT(socket);
            socket->SetRecvCallback(MakeCallback(&RoutingProtocol::RecvScr, this));
            socket->BindToNetDevice(l3->GetNetDevice(i));
            socket->Bind(InetSocketAddress(iface.GetLocal(), SCR_PORT));
            socket->SetAllowBroadcast(true);
            m_socketAddresses.insert(std::make_pair(socket, iface));

            // create also a subnet directed broadcast socket
            socket = Socket::CreateSocket(GetObject<Node>(), UdpSocketFactory::GetTypeId());
            NS_ASSERT(socket);
            socket->SetRecvCallback(MakeCallback(&RoutingProtocol::RecvScr, this));
            socket->BindToNetDevice(l3->GetNetDevice(i));
            socket->Bind(InetSocketAddress(iface.GetBroadcast(), SCR_PORT));
            socket->SetAllowBroadcast(true);
            socket->SetIpRecvTtl(true);
            m_socketSubnetBroadcastAddresses.insert(std::make_pair(socket, iface));

            // Add local broadcast record to the routing table
            Ptr<NetDevice> dev =
                m_ipv4->GetNetDevice(m_ipv4->GetInterfaceForAddress(iface.GetLocal()));
            RoutingTableEntry rt(/*dev=*/dev,
                                 /*dst=*/iface.GetBroadcast(),
                                 /*vSeqNo=*/true,
                                 /*seqNo=*/0,
                                 /*iface=*/iface,
                                 /*hops=*/1,
                                 /*nextHop=*/iface.GetBroadcast(),
                                 /*lifetime=*/Simulator::GetMaximumSimulationTime());
            m_routingTable.AddRoute(rt);
        }
    }
    else
    {
        NS_LOG_LOGIC("SCR does not work with more then one address per each interface. Ignore "
                     "added address");
    }
}

void
RoutingProtocol::NotifyRemoveAddress(uint32_t i, Ipv4InterfaceAddress address)
{
    NS_LOG_FUNCTION(this);
    Ptr<Socket> socket = FindSocketWithInterfaceAddress(address);
    if (socket)
    {
        m_routingTable.DeleteAllRoutesFromInterface(address);
        socket->Close();
        m_socketAddresses.erase(socket);

        Ptr<Socket> unicastSocket = FindSubnetBroadcastSocketWithInterfaceAddress(address);
        if (unicastSocket)
        {
            unicastSocket->Close();
            m_socketAddresses.erase(unicastSocket);
        }

        Ptr<Ipv4L3Protocol> l3 = m_ipv4->GetObject<Ipv4L3Protocol>();
        if (l3->GetNAddresses(i))
        {
            Ipv4InterfaceAddress iface = l3->GetAddress(i, 0);
            // Create a socket to listen only on this interface
            Ptr<Socket> socket =
                Socket::CreateSocket(GetObject<Node>(), UdpSocketFactory::GetTypeId());
            NS_ASSERT(socket);
            socket->SetRecvCallback(MakeCallback(&RoutingProtocol::RecvScr, this));
            // Bind to any IP address so that broadcasts can be received
            socket->BindToNetDevice(l3->GetNetDevice(i));
            socket->Bind(InetSocketAddress(iface.GetLocal(), SCR_PORT));
            socket->SetAllowBroadcast(true);
            socket->SetIpRecvTtl(true);
            m_socketAddresses.insert(std::make_pair(socket, iface));

            // create also a unicast socket
            socket = Socket::CreateSocket(GetObject<Node>(), UdpSocketFactory::GetTypeId());
            NS_ASSERT(socket);
            socket->SetRecvCallback(MakeCallback(&RoutingProtocol::RecvScr, this));
            socket->BindToNetDevice(l3->GetNetDevice(i));
            socket->Bind(InetSocketAddress(iface.GetBroadcast(), SCR_PORT));
            socket->SetAllowBroadcast(true);
            socket->SetIpRecvTtl(true);
            m_socketSubnetBroadcastAddresses.insert(std::make_pair(socket, iface));

            // Add local broadcast record to the routing table
            Ptr<NetDevice> dev =
                m_ipv4->GetNetDevice(m_ipv4->GetInterfaceForAddress(iface.GetLocal()));
            RoutingTableEntry rt(/*dev=*/dev,
                                 /*dst=*/iface.GetBroadcast(),
                                 /*vSeqNo=*/true,
                                 /*seqNo=*/0,
                                 /*iface=*/iface,
                                 /*hops=*/1,
                                 /*nextHop=*/iface.GetBroadcast(),
                                 /*lifetime=*/Simulator::GetMaximumSimulationTime());
            m_routingTable.AddRoute(rt);
        }
        if (m_socketAddresses.empty())
        {
            NS_LOG_LOGIC("No scr interfaces");
            m_htimer.Cancel();
            m_nb.Clear();
            m_routingTable.Clear();
            return;
        }
    }
    else
    {
        NS_LOG_LOGIC("Remove address not participating in SCR operation");
    }
}

bool
RoutingProtocol::IsMyOwnAddress(Ipv4Address src)
{
    NS_LOG_FUNCTION(this << src);
    for (auto j = m_socketAddresses.begin(); j != m_socketAddresses.end(); ++j)
    {
        Ipv4InterfaceAddress iface = j->second;
        if (src == iface.GetLocal())
        {
            return true;
        }
    }
    return false;
}

Ptr<Ipv4Route>
RoutingProtocol::LoopbackRoute(const Ipv4Header& hdr, Ptr<NetDevice> oif) const
{
    NS_LOG_FUNCTION(this << hdr);
    NS_ASSERT(m_lo);
    Ptr<Ipv4Route> rt = Create<Ipv4Route>();
    rt->SetDestination(hdr.GetDestination());
    //
    // Source address selection here is tricky.  The loopback route is
    // returned when SCR does not have a route; this causes the packet
    // to be looped back and handled (cached) in RouteInput() method
    // while a route is found. However, connection-oriented protocols
    // like TCP need to create an endpoint four-tuple (src, src port,
    // dst, dst port) and create a pseudo-header for checksumming.  So,
    // SCR needs to guess correctly what the eventual source address
    // will be.
    //
    // For single interface, single address nodes, this is not a problem.
    // When there are possibly multiple outgoing interfaces, the policy
    // implemented here is to pick the first available SCR interface.
    // If RouteOutput() caller specified an outgoing interface, that
    // further constrains the selection of source address
    //
    auto j = m_socketAddresses.begin();
    if (oif)
    {
        // Iterate to find an address on the oif device
        for (j = m_socketAddresses.begin(); j != m_socketAddresses.end(); ++j)
        {
            Ipv4Address addr = j->second.GetLocal();
            int32_t interface = m_ipv4->GetInterfaceForAddress(addr);
            if (oif == m_ipv4->GetNetDevice(static_cast<uint32_t>(interface)))
            {
                rt->SetSource(addr);
                break;
            }
        }
    }
    else
    {
        rt->SetSource(j->second.GetLocal());
    }
    NS_ASSERT_MSG(rt->GetSource() != Ipv4Address(), "Valid SCR source address not found");
    rt->SetGateway(Ipv4Address("127.0.0.1"));
    rt->SetOutputDevice(m_lo);
    return rt;
}

void
RoutingProtocol::SendRequest(Ipv4Address dst)
{
    NS_LOG_FUNCTION(this << dst);
    // A node SHOULD NOT originate more than RREQ_RATELIMIT RREQ messages per second.
    if (m_rreqCount == m_rreqRateLimit)
    {
        // SCR: the upstream code rescheduled SendRequest() directly here, which
        // re-entered the send path with NO admission decision and NO log row.
        // That is the "silent native retry fallback" / "second unlogged retry
        // scheduler" the specification forbids (see AODV_INTEGRATION_AUDIT 3.1).
        //
        // Under SCR the destination is handed back to the single scheduler,
        // which will re-evaluate it through ScrTryDiscovery like any other
        // origination. The native cap remains an inherited safety limiter but
        // never originates anything by itself.
        if (m_scrEnabled)
        {
            m_scrPendingSend.erase(dst);
            Simulator::Schedule(m_rreqRateLimitTimer.GetDelayLeft() + MicroSeconds(100),
                                &RoutingProtocol::ScrReconsider,
                                this,
                                dst);
            return;
        }
        Simulator::Schedule(m_rreqRateLimitTimer.GetDelayLeft() + MicroSeconds(100),
                            &RoutingProtocol::SendRequest,
                            this,
                            dst);
        return;
    }
    else
    {
        m_rreqCount++;
    }
    // Create RREQ header
    RreqHeader rreqHeader;
    rreqHeader.SetDst(dst);

    RoutingTableEntry rt;
    // Using the Hop field in Routing Table to manage the expanding ring search
    uint16_t ttl = m_ttlStart;
    // SCR (E4): TTL comes from the ledger's escalation index j, which persists
    // across route replacements within an episode. The native scheme stores ring
    // state in the routing-table hop field, which resets with the entry - exactly
    // the lifetime E4 rejects.
    bool scrTtlFixed = false;
    if (m_scrEnabled)
    {
        auto pend = m_scrPendingSend.find(dst);
        if (pend != m_scrPendingSend.end())
        {
            RepairLedger& led = ScrLedger(dst);
            ttl = (pend->second == ADMIT_PROBE) ? m_netDiameter : led.TtlForNextAttempt();
            scrTtlFixed = true;
        }
    }
    if (m_routingTable.LookupRoute(dst, rt))
    {
        // SCR: when the ledger has fixed the TTL, the native expanding-ring
        // computation is suppressed. Everything else about the entry update -
        // sequence numbers, IN_SEARCH flag, lifetime - is preserved unchanged,
        // so ordinary AODV acceptance rules still apply.
        if (!scrTtlFixed)
        {
            if (rt.GetFlag() != IN_SEARCH)
            {
                ttl = std::min<uint16_t>(rt.GetHop() + m_ttlIncrement, m_netDiameter);
            }
            else
            {
                ttl = rt.GetHop() + m_ttlIncrement;
                if (ttl > m_ttlThreshold)
                {
                    ttl = m_netDiameter;
                }
            }
        }
        if (ttl == m_netDiameter)
        {
            rt.IncrementRreqCnt();
        }
        if (rt.GetValidSeqNo())
        {
            rreqHeader.SetDstSeqno(rt.GetSeqNo());
        }
        else
        {
            rreqHeader.SetUnknownSeqno(true);
        }
        rt.SetHop(ttl);
        rt.SetFlag(IN_SEARCH);
        rt.SetLifeTime(m_pathDiscoveryTime);
        m_routingTable.Update(rt);
    }
    else
    {
        rreqHeader.SetUnknownSeqno(true);
        Ptr<NetDevice> dev = nullptr;
        RoutingTableEntry newEntry(/*dev=*/dev,
                                   /*dst=*/dst,
                                   /*vSeqNo=*/false,
                                   /*seqNo=*/0,
                                   /*iface=*/Ipv4InterfaceAddress(),
                                   /*hops=*/ttl,
                                   /*nextHop=*/Ipv4Address(),
                                   /*lifetime=*/m_pathDiscoveryTime);
        // Check if TtlStart == NetDiameter
        if (ttl == m_netDiameter)
        {
            newEntry.IncrementRreqCnt();
        }
        newEntry.SetFlag(IN_SEARCH);
        m_routingTable.AddRoute(newEntry);
    }

    if (m_gratuitousReply)
    {
        rreqHeader.SetGratuitousRrep(true);
    }
    if (m_destinationOnly)
    {
        rreqHeader.SetDestinationOnly(true);
    }

    m_seqNo++;
    rreqHeader.SetOriginSeqno(m_seqNo);
    m_requestId++;
    rreqHeader.SetId(m_requestId);

    // Counted in both modes: this is the control-cost quantity compared
    // between SCR and the native references.
    ++m_totalRreqOriginated;

    // SCR (T10): every RREQ that reaches this loop must have a matching
    // admission record. This is the direct assertion that no origination path -
    // including any native fallback - bypasses ScrTryDiscovery.
    if (m_scrEnabled && m_scrPendingSend.find(dst) == m_scrPendingSend.end())
    {
        ++m_scrUnchargedRreq;
        NS_LOG_ERROR("SCR T10 VIOLATION: RREQ to " << dst << " reached the wire with no "
                                                   << "admission record");
    }

    // R058: a TAAODV source names its forwarders as a relay does (no previous hop).
    TaaodvExtHeader taaOut;
    if (IsTaaodv())
    {
        taaOut.m_kind = TaaodvExtHeader::TAA_RREQ;
        taaOut.m_pheromone = 0.0F;
        taaOut.m_addrs = TaaSelectForwarders(Ipv4Address());
    }

    // Send RREQ as subnet directed broadcast from each interface used by scr
    for (auto j = m_socketAddresses.begin(); j != m_socketAddresses.end(); ++j)
    {
        Ptr<Socket> socket = j->first;
        Ipv4InterfaceAddress iface = j->second;

        rreqHeader.SetOrigin(iface.GetLocal());
        m_rreqIdCache.IsDuplicate(iface.GetLocal(), m_requestId);

        Ptr<Packet> packet = Create<Packet>();
        SocketIpTtlTag tag;
        tag.SetTtl(ttl);
        packet->AddPacketTag(tag);
        if (IsTaaodv())
        {
            packet->AddHeader(taaOut);
        }
        packet->AddHeader(rreqHeader);
        TypeHeader tHeader(SCRTYPE_RREQ);
        packet->AddHeader(tHeader);
        // Send to all-hosts broadcast if on /32 addr, subnet-directed otherwise
        Ipv4Address destination;
        if (iface.GetMask() == Ipv4Mask::GetOnes())
        {
            destination = Ipv4Address("255.255.255.255");
        }
        else
        {
            destination = iface.GetBroadcast();
        }
        NS_LOG_DEBUG("Send RREQ with id " << rreqHeader.GetId() << " to socket");
        m_lastBcastTime = Simulator::Now();
        // D003: ONE draw, from the dedicated stream under SCR. This site is the
        // SOURCE origination; the identical-looking draw in RecvRequest relays
        // another node's RREQ and is not a "newly ready request", so it keeps
        // the native stream. The native reference modes keep it here too, for
        // the reason D002 gives: references must not be altered to match the
        // fork.
        // A missing jitter stream under SCR means AssignJitterStream was never
        // called for this node. Falling back silently would reintroduce exactly
        // the defect D003 describes, so it is an assertion, not a default.
        if (m_scrEnabled && !m_scrJitter)
        {
            // Not a default: a missing reservation would silently reintroduce
            // the very defect D003 describes.
            NS_ASSERT_MSG(m_scrJitterStream >= 0,
                          "D003: SCR mode without a reserved jitter stream; "
                          "AssignJitterStream was not called for this node");
            m_scrJitter = CreateObject<UniformRandomVariable>();
            m_scrJitter->SetStream(m_scrJitterStream);
        }
        const uint32_t jitterMs = m_scrEnabled ? m_scrJitter->GetInteger(0, 10)
                                               : m_uniformRandomVariable->GetInteger(0, 10);
        // D003 validating test. Recording only: no randomness is consumed here,
        // so run summaries are unchanged -- verified byte for byte rather than
        // asserted.
        if (m_scrEnabled && m_jitterDraws.size() < 512)
        {
            m_jitterDraws.push_back(jitterMs);
        }
        Simulator::Schedule(MilliSeconds(jitterMs),
                            &RoutingProtocol::SendTo,
                            this,
                            socket,
                            packet,
                            destination);
    }
    ScheduleRreqRetry(dst);
}

void
RoutingProtocol::SendTo(Ptr<Socket> socket, Ptr<Packet> packet, Ipv4Address destination)
{
    // SCR (E6): "Define a source origination at this handoff boundary, not at a
    // receiver event." Upstream discarded this return value; SCR needs it to
    // distinguish an immediate socket failure (which costs nothing) from later
    // radio loss (which never refunds).
    // M20: control packets sent through this function are counted here, by
    // serialized size (routing header only, no UDP/IP). R058 correction: that
    // is NOT all control traffic. RREQs (originated and relayed), HELLOs and
    // the RERRs of SendRerrMessage pass through here; unicast RREPs (SendReply,
    // SendReplyByIntermediateNode, forwarded in RecvReply), RREP-ACKs and the
    // RERR that SendRerrWhenNoRouteToForward unicasts to a source call the
    // socket directly and are not counted. Left unchanged so every revision
    // measures the same quantity; the paper states the definition.
    m_ctrlBytesSent += packet->GetSize();
    {
        TypeHeader th(SCRTYPE_RREQ);
        Ptr<Packet> peek = packet->Copy();
        peek->RemoveHeader(th);
        if (th.IsValid())
        {
            switch (th.Get())
            {
            case SCRTYPE_RREP: ++m_ctrlRrepSent; break;
            case SCRTYPE_RERR: ++m_ctrlRerrSent; break;
            default: break;
            }
        }
    }

    int rc = socket->SendTo(packet, 0, InetSocketAddress(destination, SCR_PORT));
    if (m_scrEnabled && rc < 0)
    {
        ++m_scrHandoffFailed;
        NS_LOG_WARN("SCR: immediate socket handoff failure to " << destination
                                                                << "; allowances unchanged");
    }
}

void
RoutingProtocol::ScheduleRreqRetry(Ipv4Address dst)
{
    NS_LOG_FUNCTION(this << dst);
    if (m_addressReqTimer.find(dst) == m_addressReqTimer.end())
    {
        Timer timer(Timer::CANCEL_ON_DESTROY);
        m_addressReqTimer[dst] = timer;
    }
    m_addressReqTimer[dst].SetFunction(&RoutingProtocol::RouteRequestTimerExpire, this);
    m_addressReqTimer[dst].Cancel();
    m_addressReqTimer[dst].SetArguments(dst);
    RoutingTableEntry rt;
    m_routingTable.LookupRoute(dst, rt);
    Time retry;

    // R029 / D002. THIS TIMER IS THE FORK'S REPLY TIMEOUT.
    //
    // In SCR mode its expiry is what calls RepairLedger::OnReplyTimeout and then
    // ScrTryDiscovery, so whatever interval is scheduled here IS T_wait -- not
    // the value the ledger computes in ReplyTimeout(), which until now nothing
    // consulted when arming the timer.
    //
    // D002 registered, before any fork code existed, that SCR implements E4 as
    // written: T_wait = 2*NodeTraversalTime*(h+TimeoutBuffer) below full
    // diameter, and a FLAT NetTraversalTime at full diameter -- explicitly not
    // upstream's binary exponential backoff. Its stated reason was that
    // inheriting the native exponential "would silently add a second,
    // unregistered throttle and confound exactly the comparison the study is
    // designed to make -- in particular against PERSISTENT-BACKOFF, whose
    // registered schedule min(0.5*2^k,16) s is the intended backoff comparator."
    //
    // The fork shipped with the native exponential still here. Every SCR-family
    // run to date therefore carried that second throttle, and SCR pins TTL to
    // full diameter, so it took the exponential branch on essentially every
    // origination: T_wait grew 2.8 s, 5.6 s, 11.2 s, ... instead of staying flat
    // at 2.8 s. That is the confound D002 predicted, and R027's headline finding
    // was SCR and PERSISTENT-BACKOFF worst.
    //
    // D002 is equally explicit that AODV-STOCK and AODV-FB keep native behaviour
    // unchanged, so this branch is conditioned on the fork's own mode and the
    // native path below is left exactly as upstream wrote it.
    if (m_scrEnabled)
    {
        // rt.GetHop() is the TTL SendRequest just stored for this attempt.
        retry = ScrLedger(dst).ReplyTimeout(rt.GetHop());
        m_lastRreqRetryDelay = retry;
        m_lastRreqRetryTtl = rt.GetHop();
        m_addressReqTimer[dst].Schedule(retry);
        NS_LOG_LOGIC("Scheduled SCR reply timeout in " << retry.As(Time::S));
        return;
    }

    if (rt.GetHop() < m_netDiameter)
    {
        retry = 2 * m_nodeTraversalTime * (rt.GetHop() + m_timeoutBuffer);
    }
    else
    {
        NS_ABORT_MSG_UNLESS(rt.GetRreqCnt() > 0, "Unexpected value for GetRreqCount ()");
        uint32_t backoffFactor = rt.GetRreqCnt() - 1;
        // R028. `1 << backoffFactor` is an INT shift. At backoffFactor == 31 it
        // yields INT_MIN, so retry goes negative and ns-3 aborts the run with
        // "Schedule(): Negative delay". Seven C4 RREP-RESET runs died this way
        // on 2026-09-13, which silently removed that arm's worst runs from its
        // own column.
        //
        // Upstream AODV never reaches 31 because RouteRequestTimerExpire gives
        // up at the cap. This fork could step PAST the cap (fixed there), so the
        // counter ran away. That is the real defect; this clamp is the second
        // line of defence, because a routing protocol must not be able to kill
        // the simulator by arithmetic. With net_traversal_ms=2800 a factor of 16
        // is already ~50 hours, far beyond any run in this study, so a clamped
        // retry is indistinguishable from "never" inside the simulation.
        static constexpr uint32_t SCR_MAX_BACKOFF_FACTOR = 16;
        if (backoffFactor > SCR_MAX_BACKOFF_FACTOR)
        {
            backoffFactor = SCR_MAX_BACKOFF_FACTOR;
            ++m_rreqBackoffClamped;
        }
        NS_LOG_LOGIC("Applying binary exponential backoff factor " << backoffFactor);
        retry = m_netTraversalTime * (int64_t(1) << backoffFactor);
    }
    m_lastRreqRetryDelay = retry;
    m_lastRreqRetryTtl = rt.GetHop();
    m_addressReqTimer[dst].Schedule(retry);
    NS_LOG_LOGIC("Scheduled RREQ retry in " << retry.As(Time::S));
}

void
RoutingProtocol::RecvScr(Ptr<Socket> socket)
{
    NS_LOG_FUNCTION(this << socket);
    Address sourceAddress;
    Ptr<Packet> packet = socket->RecvFrom(sourceAddress);
    InetSocketAddress inetSourceAddr = InetSocketAddress::ConvertFrom(sourceAddress);
    Ipv4Address sender = inetSourceAddr.GetIpv4();
    Ipv4Address receiver;

    if (m_socketAddresses.find(socket) != m_socketAddresses.end())
    {
        receiver = m_socketAddresses[socket].GetLocal();
    }
    else if (m_socketSubnetBroadcastAddresses.find(socket) !=
             m_socketSubnetBroadcastAddresses.end())
    {
        receiver = m_socketSubnetBroadcastAddresses[socket].GetLocal();
    }
    else
    {
        NS_ASSERT_MSG(false, "Received a packet from an unknown socket");
    }
    NS_LOG_DEBUG("SCR node " << this << " received a SCR packet from " << sender << " to "
                              << receiver);

    UpdateRouteToNeighbor(sender, receiver);
    TypeHeader tHeader(SCRTYPE_RREQ);
    packet->RemoveHeader(tHeader);
    if (!tHeader.IsValid())
    {
        NS_LOG_DEBUG("SCR message " << packet->GetUid() << " with unknown type received: "
                                     << tHeader.Get() << ". Drop");
        return; // drop
    }
    switch (tHeader.Get())
    {
    case SCRTYPE_RREQ: {
        RecvRequest(packet, receiver, sender);
        break;
    }
    case SCRTYPE_RREP: {
        RecvReply(packet, receiver, sender);
        break;
    }
    case SCRTYPE_RERR: {
        RecvError(packet, sender);
        break;
    }
    case SCRTYPE_RREP_ACK: {
        RecvReplyAck(sender);
        break;
    }
    }
}

bool
RoutingProtocol::UpdateRouteLifeTime(Ipv4Address addr, Time lifetime)
{
    NS_LOG_FUNCTION(this << addr << lifetime);
    RoutingTableEntry rt;
    if (m_routingTable.LookupRoute(addr, rt))
    {
        if (rt.GetFlag() == VALID)
        {
            NS_LOG_DEBUG("Updating VALID route");
            rt.SetRreqCnt(0);
            rt.SetLifeTime(std::max(lifetime, rt.GetLifeTime()));
            m_routingTable.Update(rt);
            return true;
        }
    }
    return false;
}

void
RoutingProtocol::UpdateRouteToNeighbor(Ipv4Address sender, Ipv4Address receiver)
{
    NS_LOG_FUNCTION(this << "sender " << sender << " receiver " << receiver);
    RoutingTableEntry toNeighbor;
    if (!m_routingTable.LookupRoute(sender, toNeighbor))
    {
        Ptr<NetDevice> dev = m_ipv4->GetNetDevice(m_ipv4->GetInterfaceForAddress(receiver));
        RoutingTableEntry newEntry(
            /*dev=*/dev,
            /*dst=*/sender,
            /*vSeqNo=*/false,
            /*seqNo=*/0,
            /*iface=*/m_ipv4->GetAddress(m_ipv4->GetInterfaceForAddress(receiver), 0),
            /*hops=*/1,
            /*nextHop=*/sender,
            /*lifetime=*/m_activeRouteTimeout);
        m_routingTable.AddRoute(newEntry);
    }
    else
    {
        Ptr<NetDevice> dev = m_ipv4->GetNetDevice(m_ipv4->GetInterfaceForAddress(receiver));
        if (toNeighbor.GetValidSeqNo() && (toNeighbor.GetHop() == 1) &&
            (toNeighbor.GetOutputDevice() == dev))
        {
            toNeighbor.SetLifeTime(std::max(m_activeRouteTimeout, toNeighbor.GetLifeTime()));
        }
        else
        {
            RoutingTableEntry newEntry(
                /*dev=*/dev,
                /*dst=*/sender,
                /*vSeqNo=*/false,
                /*seqNo=*/0,
                /*iface=*/m_ipv4->GetAddress(m_ipv4->GetInterfaceForAddress(receiver), 0),
                /*hops=*/1,
                /*nextHop=*/sender,
                /*lifetime=*/std::max(m_activeRouteTimeout, toNeighbor.GetLifeTime()));
            m_routingTable.Update(newEntry);
        }
    }
}

void
RoutingProtocol::RecvRequest(Ptr<Packet> p, Ipv4Address receiver, Ipv4Address src)
{
    NS_LOG_FUNCTION(this);
    RreqHeader rreqHeader;
    p->RemoveHeader(rreqHeader);
    // R058: TAAODV's pheromone and forwarder list follow the RREQ header.
    TaaodvExtHeader taaIn;
    bool taaHas = false;
    if (IsTaaodv() && p->GetSize() > 0)
    {
        p->RemoveHeader(taaIn);
        taaHas = (taaIn.m_kind == TaaodvExtHeader::TAA_RREQ);
    }

    // A node ignores all RREQs received from any node in its blacklist
    RoutingTableEntry toPrev;
    if (m_routingTable.LookupRoute(src, toPrev))
    {
        if (toPrev.IsUnidirectional())
        {
            NS_LOG_DEBUG("Ignoring RREQ from node in blacklist");
            return;
        }
    }

    uint32_t id = rreqHeader.GetId();
    Ipv4Address origin = rreqHeader.GetOrigin();

    /*
     *  Node checks to determine whether it has received a RREQ with the same Originator IP Address
     * and RREQ ID. If such a RREQ has been received, the node silently discards the newly received
     * RREQ.
     */
    if (m_rreqIdCache.IsDuplicate(origin, id))
    {
        NS_LOG_DEBUG("Ignoring RREQ due to duplicate");
        if (IsClaf())
        {
            // R057: a suppressed relay counts the copies it hears (phase III),
            // and a destination still collecting records each copy's path.
            const auto key = std::make_pair(origin, id);
            auto pend = m_clafPending.find(key);
            if (pend != m_clafPending.end())
            {
                ++pend->second.copies;
            }
            auto col = m_clafDest.find(key);
            if (col != m_clafDest.end())
            {
                ClafCopy c;
                c.src = src;
                c.hop = rreqHeader.GetHopCount() + 1;
                c.pqm = rreqHeader.GetPathQuality();
                c.hdr = rreqHeader;
                c.hdr.SetHopCount(c.hop);
                col->second.push_back(c);
            }
        }
        if (IsTaaodv() && taaHas && !taaIn.m_addrs.empty() && !IsMyOwnAddress(origin) &&
            !IsMyOwnAddress(rreqHeader.GetDst()))
        {
            // R058: designation read as addressing. A node that the copies it
            // heard first did not name has, in that reading, not received the
            // request; the first copy that names it is the one it forwards.
            bool named = false;
            for (const auto& a : taaIn.m_addrs)
            {
                named = named || IsMyOwnAddress(a);
            }
            SocketIpTtlTag ttlTag;
            p->RemovePacketTag(ttlTag);
            if (named && ttlTag.GetTtl() >= 2 && !m_taaFwdCache.IsDuplicate(origin, id))
            {
                rreqHeader.SetHopCount(rreqHeader.GetHopCount() + 1);
                TaaodvExtHeader out;
                out.m_kind = TaaodvExtHeader::TAA_RREQ;
                out.m_pheromone =
                    static_cast<float>(taaIn.m_pheromone + TaaPheromoneDelta(src));
                out.m_addrs = TaaSelectForwarders(src);
                ++m_taaLateNamed;
                RebroadcastRreq(rreqHeader, ttlTag.GetTtl(), &out);
            }
        }
        return;
    }

    // Increment RREQ hop count
    uint8_t hop = rreqHeader.GetHopCount() + 1;
    rreqHeader.SetHopCount(hop);

    // R058: pheromone of the link just crossed, added to the path's (Eqs. 25-26).
    TaaodvExtHeader taaPath;
    if (IsTaaodv())
    {
        taaPath.m_kind = TaaodvExtHeader::TAA_RREP;
        taaPath.m_pheromone =
            static_cast<float>((taaHas ? taaIn.m_pheromone : 0.0F) + TaaPheromoneDelta(src));
    }

    /*
     *  When the reverse route is created or updated, the following actions on the route are also
     * carried out:
     *  1. the Originator Sequence Number from the RREQ is compared to the corresponding destination
     * sequence number in the route table entry and copied if greater than the existing value there
     *  2. the valid sequence number field is set to true;
     *  3. the next hop in the routing table becomes the node from which the  RREQ was received
     *  4. the hop count is copied from the Hop Count in the RREQ message;
     *  5. the Lifetime is set to be the maximum of (ExistingLifetime, MinimalLifetime), where
     *     MinimalLifetime = current time + 2*NetTraversalTime - 2*HopCount*NodeTraversalTime
     */
    RoutingTableEntry toOrigin;
    if (!m_routingTable.LookupRoute(origin, toOrigin))
    {
        Ptr<NetDevice> dev = m_ipv4->GetNetDevice(m_ipv4->GetInterfaceForAddress(receiver));
        RoutingTableEntry newEntry(
            /*dev=*/dev,
            /*dst=*/origin,
            /*vSeqNo=*/true,
            /*seqNo=*/rreqHeader.GetOriginSeqno(),
            /*iface=*/m_ipv4->GetAddress(m_ipv4->GetInterfaceForAddress(receiver), 0),
            /*hops=*/hop,
            /*nextHop=*/src,
            /*lifetime=*/Time(2 * m_netTraversalTime - 2 * hop * m_nodeTraversalTime));
        m_routingTable.AddRoute(newEntry);
    }
    else
    {
        if (toOrigin.GetValidSeqNo())
        {
            if (int32_t(rreqHeader.GetOriginSeqno()) - int32_t(toOrigin.GetSeqNo()) > 0)
            {
                toOrigin.SetSeqNo(rreqHeader.GetOriginSeqno());
            }
        }
        else
        {
            toOrigin.SetSeqNo(rreqHeader.GetOriginSeqno());
        }
        toOrigin.SetValidSeqNo(true);
        toOrigin.SetNextHop(src);
        toOrigin.SetOutputDevice(m_ipv4->GetNetDevice(m_ipv4->GetInterfaceForAddress(receiver)));
        toOrigin.SetInterface(m_ipv4->GetAddress(m_ipv4->GetInterfaceForAddress(receiver), 0));
        toOrigin.SetHop(hop);
        toOrigin.SetLifeTime(std::max(Time(2 * m_netTraversalTime - 2 * hop * m_nodeTraversalTime),
                                      toOrigin.GetLifeTime()));
        m_routingTable.Update(toOrigin);
        // m_nb.Update (src, Time (AllowedHelloLoss * HelloInterval));
    }

    RoutingTableEntry toNeighbor;
    if (!m_routingTable.LookupRoute(src, toNeighbor))
    {
        NS_LOG_DEBUG("Neighbor:" << src << " not found in routing table. Creating an entry");
        Ptr<NetDevice> dev = m_ipv4->GetNetDevice(m_ipv4->GetInterfaceForAddress(receiver));
        RoutingTableEntry newEntry(dev,
                                   src,
                                   false,
                                   rreqHeader.GetOriginSeqno(),
                                   m_ipv4->GetAddress(m_ipv4->GetInterfaceForAddress(receiver), 0),
                                   1,
                                   src,
                                   m_activeRouteTimeout);
        m_routingTable.AddRoute(newEntry);
    }
    else
    {
        toNeighbor.SetLifeTime(m_activeRouteTimeout);
        toNeighbor.SetValidSeqNo(false);
        toNeighbor.SetSeqNo(rreqHeader.GetOriginSeqno());
        toNeighbor.SetFlag(VALID);
        toNeighbor.SetOutputDevice(m_ipv4->GetNetDevice(m_ipv4->GetInterfaceForAddress(receiver)));
        toNeighbor.SetInterface(m_ipv4->GetAddress(m_ipv4->GetInterfaceForAddress(receiver), 0));
        toNeighbor.SetHop(1);
        toNeighbor.SetNextHop(src);
        m_routingTable.Update(toNeighbor);
    }
    m_nb.Update(src, Time(m_allowedHelloLoss * m_helloInterval));

    NS_LOG_LOGIC(receiver << " receive RREQ with hop count "
                          << static_cast<uint32_t>(rreqHeader.GetHopCount()) << " ID "
                          << rreqHeader.GetId() << " to destination " << rreqHeader.GetDst());

    //  A node generates a RREP if either:
    //  (i)  it is itself the destination,
    if (IsMyOwnAddress(rreqHeader.GetDst()))
    {
        if (IsClaf())
        {
            // R057: collect the copies of this request for ClafDestTimer and
            // reply along the best path priority factor (Safari et al., Eq. 12).
            const auto key = std::make_pair(origin, id);
            ClafCopy c;
            c.src = src;
            c.hop = hop;
            c.pqm = rreqHeader.GetPathQuality();
            c.hdr = rreqHeader;
            m_clafDest[key] = {c};
            Simulator::Schedule(m_clafDestTimer, &RoutingProtocol::ClafDestExpire, this, key);
            return;
        }
        m_routingTable.LookupRoute(origin, toOrigin);
        NS_LOG_DEBUG("Send reply since I am the destination");
        SendReply(rreqHeader, toOrigin, IsTaaodv() ? &taaPath : nullptr);
        return;
    }
    /*
     * (ii) or it has an active route to the destination, the destination sequence number in the
     * node's existing route table entry for the destination is valid and greater than or equal to
     * the Destination Sequence Number of the RREQ, and the "destination only" flag is NOT set.
     */
    RoutingTableEntry toDst;
    Ipv4Address dst = rreqHeader.GetDst();
    if (m_routingTable.LookupRoute(dst, toDst))
    {
        /*
         * Drop RREQ, This node RREP will make a loop.
         */
        if (toDst.GetNextHop() == src)
        {
            NS_LOG_DEBUG("Drop RREQ from " << src << ", dest next hop " << toDst.GetNextHop());
            return;
        }
        /*
         * The Destination Sequence number for the requested destination is set to the maximum of
         * the corresponding value received in the RREQ message, and the destination sequence value
         * currently maintained by the node for the requested destination. However, the forwarding
         * node MUST NOT modify its maintained value for the destination sequence number, even if
         * the value received in the incoming RREQ is larger than the value currently maintained by
         * the forwarding node.
         */
        if ((rreqHeader.GetUnknownSeqno() ||
             (int32_t(toDst.GetSeqNo()) - int32_t(rreqHeader.GetDstSeqno()) >= 0)) &&
            toDst.GetValidSeqNo())
        {
            if (!rreqHeader.GetDestinationOnly() && toDst.GetFlag() == VALID)
            {
                m_routingTable.LookupRoute(origin, toOrigin);
                if (IsTaaodv())
                {
                    m_taaFwdCache.IsDuplicate(origin, id); // answered: never forward it later
                }
                SendReplyByIntermediateNode(toDst,
                                            toOrigin,
                                            rreqHeader.GetGratuitousRrep(),
                                            IsTaaodv() ? &taaPath : nullptr);
                return;
            }
            rreqHeader.SetDstSeqno(toDst.GetSeqNo());
            rreqHeader.SetUnknownSeqno(false);
        }
    }

    SocketIpTtlTag tag;
    p->RemovePacketTag(tag);
    if (tag.GetTtl() < 2)
    {
        NS_LOG_DEBUG("TTL exceeded. Drop RREQ origin " << src << " destination " << dst);
        return;
    }

    if (IsTaaodv())
    {
        // R058: every receiver has processed the RREQ above (reverse route,
        // reply as destination or with a fresh route); only a named forwarder
        // rebroadcasts it (Li et al., Sec. IV-A and Fig. 4).
        bool named = !taaHas || taaIn.m_addrs.empty();
        for (const auto& a : taaIn.m_addrs)
        {
            if (!named && IsMyOwnAddress(a))
            {
                named = true;
            }
        }
        if (!named)
        {
            ++m_taaNotSelected;
            return;
        }
        m_taaFwdCache.IsDuplicate(origin, id); // record: forwarded
        TaaodvExtHeader out;
        out.m_kind = TaaodvExtHeader::TAA_RREQ;
        out.m_pheromone = taaPath.m_pheromone;
        out.m_addrs = TaaSelectForwarders(src);
        RebroadcastRreq(rreqHeader, tag.GetTtl(), &out);
        return;
    }

    if (IsClaf())
    {
        // R057: CLAF-AODV relay decision (Safari et al., Fig. 7).
        const double rss = ClafRssDbm(src);
        // Phase I: minimum requirements. RSSI_thr in the paper sits about 10 dB
        // below its range-edge power; mapped to ours it is below reception, and
        // our scenarios have no energy model (E_rem = 1), so both tests are
        // implemented but cannot fire here.
        if (!std::isnan(rss) && rss < -92.1)
        {
            ++m_clafPhase1Drop;
            return;
        }
        const uint32_t nd = m_nb.CountNeighbors();
        const double nqm = ClafNodeQuality(rss);
        const double pqmIn = rreqHeader.GetPathQuality();
        const double n = static_cast<double>(hop - 1); // hops before this node
        rreqHeader.SetPathQuality((n * pqmIn + nqm) / (n + 1.0)); // Eq. (10)
        if (nd < m_clafNdThreshold)
        {
            ++m_clafSparseForward;
            RebroadcastRreq(rreqHeader, tag.GetTtl());
            return;
        }
        NS_ASSERT_MSG(m_clafRng, "CLAF-AODV mode without an assigned stream");
        const double prob = ClafForwardProbability(nd, nqm, pqmIn);
        if (m_clafRng->GetValue(0.0, 1.0) < prob)
        {
            ++m_clafProbForward;
            RebroadcastRreq(rreqHeader, tag.GetTtl());
            return;
        }
        // Phase III: wait TMR = phi + NTT, phi ~ U(0, 60 ms) (Eq. 11), and
        // rebroadcast unless C copies were heard in the meantime.
        ++m_clafSuppressed;
        const auto key = std::make_pair(origin, id);
        ClafPending pend;
        pend.hdr = rreqHeader;
        pend.ttl = tag.GetTtl();
        m_clafPending[key] = pend;
        const Time wait = MilliSeconds(m_clafRng->GetValue(0.0, 60.0)) + m_nodeTraversalTime;
        Simulator::Schedule(wait, &RoutingProtocol::ClafPhase3Expire, this, key);
        return;
    }

    RebroadcastRreq(rreqHeader, tag.GetTtl());
}

void
RoutingProtocol::RebroadcastRreq(RreqHeader rreqHeader,
                                 uint8_t ttlIn,
                                 const TaaodvExtHeader* taa)
{
    SocketIpTtlTag tag;
    tag.SetTtl(ttlIn);

    // M20: this node is forwarding another node's RREQ. It is real control
    // airtime and is NOT charged to any source budget (E6 forbids that), so it
    // must be measured separately or total overhead is understated.
    ++m_ctrlRreqRelayed;

    for (auto j = m_socketAddresses.begin(); j != m_socketAddresses.end(); ++j)
    {
        Ptr<Socket> socket = j->first;
        Ipv4InterfaceAddress iface = j->second;
        Ptr<Packet> packet = Create<Packet>();
        SocketIpTtlTag ttl;
        ttl.SetTtl(tag.GetTtl() - 1);
        packet->AddPacketTag(ttl);
        if (taa)
        {
            packet->AddHeader(*taa);
        }
        packet->AddHeader(rreqHeader);
        TypeHeader tHeader(SCRTYPE_RREQ);
        packet->AddHeader(tHeader);
        // Send to all-hosts broadcast if on /32 addr, subnet-directed otherwise
        Ipv4Address destination;
        if (iface.GetMask() == Ipv4Mask::GetOnes())
        {
            destination = Ipv4Address("255.255.255.255");
        }
        else
        {
            destination = iface.GetBroadcast();
        }
        m_lastBcastTime = Simulator::Now();
        Simulator::Schedule(MilliSeconds(m_uniformRandomVariable->GetInteger(0, 10)),
                            &RoutingProtocol::SendTo,
                            this,
                            socket,
                            packet,
                            destination);
    }
}

void
RoutingProtocol::SendReply(const RreqHeader& rreqHeader,
                           const RoutingTableEntry& toOrigin,
                           const TaaodvExtHeader* taa)
{
    NS_LOG_FUNCTION(this << toOrigin.GetDestination());
    /*
     * Destination node MUST increment its own sequence number by one if the sequence number in the
     * RREQ packet is equal to that incremented value. Otherwise, the destination does not change
     * its sequence number before generating the  RREP message.
     */
    if (!rreqHeader.GetUnknownSeqno() && (rreqHeader.GetDstSeqno() == m_seqNo + 1))
    {
        m_seqNo++;
    }
    RrepHeader rrepHeader(/*prefixSize=*/0,
                          /*hopCount=*/0,
                          /*dst=*/rreqHeader.GetDst(),
                          /*dstSeqNo=*/m_seqNo,
                          /*origin=*/toOrigin.GetDestination(),
                          /*lifetime=*/m_myRouteTimeout);
    Ptr<Packet> packet = Create<Packet>();
    SocketIpTtlTag tag;
    tag.SetTtl(toOrigin.GetHop());
    packet->AddPacketTag(tag);
    if (taa)
    {
        packet->AddHeader(*taa);
    }
    packet->AddHeader(rrepHeader);
    TypeHeader tHeader(SCRTYPE_RREP);
    packet->AddHeader(tHeader);
    Ptr<Socket> socket = FindSocketWithInterfaceAddress(toOrigin.GetInterface());
    NS_ASSERT(socket);
    socket->SendTo(packet, 0, InetSocketAddress(toOrigin.GetNextHop(), SCR_PORT));
}

void
RoutingProtocol::SendReplyByIntermediateNode(RoutingTableEntry& toDst,
                                             RoutingTableEntry& toOrigin,
                                             bool gratRep,
                                             const TaaodvExtHeader* taa)
{
    NS_LOG_FUNCTION(this);
    RrepHeader rrepHeader(/*prefixSize=*/0,
                          /*hopCount=*/toDst.GetHop(),
                          /*dst=*/toDst.GetDestination(),
                          /*dstSeqNo=*/toDst.GetSeqNo(),
                          /*origin=*/toOrigin.GetDestination(),
                          /*lifetime=*/toDst.GetLifeTime());
    /* If the node we received a RREQ for is a neighbor we are
     * probably facing a unidirectional link... Better request a RREP-ack
     */
    if (toDst.GetHop() == 1)
    {
        rrepHeader.SetAckRequired(true);
        RoutingTableEntry toNextHop;
        m_routingTable.LookupRoute(toOrigin.GetNextHop(), toNextHop);
        toNextHop.m_ackTimer.SetFunction(&RoutingProtocol::AckTimerExpire, this);
        toNextHop.m_ackTimer.SetArguments(toNextHop.GetDestination(), m_blackListTimeout);
        toNextHop.m_ackTimer.SetDelay(m_nextHopWait);
    }
    toDst.InsertPrecursor(toOrigin.GetNextHop());
    toOrigin.InsertPrecursor(toDst.GetNextHop());
    m_routingTable.Update(toDst);
    m_routingTable.Update(toOrigin);

    Ptr<Packet> packet = Create<Packet>();
    SocketIpTtlTag tag;
    tag.SetTtl(toOrigin.GetHop());
    packet->AddPacketTag(tag);
    if (taa)
    {
        packet->AddHeader(*taa);
    }
    packet->AddHeader(rrepHeader);
    TypeHeader tHeader(SCRTYPE_RREP);
    packet->AddHeader(tHeader);
    Ptr<Socket> socket = FindSocketWithInterfaceAddress(toOrigin.GetInterface());
    NS_ASSERT(socket);
    socket->SendTo(packet, 0, InetSocketAddress(toOrigin.GetNextHop(), SCR_PORT));

    // Generating gratuitous RREPs
    if (gratRep)
    {
        RrepHeader gratRepHeader(/*prefixSize=*/0,
                                 /*hopCount=*/toOrigin.GetHop(),
                                 /*dst=*/toOrigin.GetDestination(),
                                 /*dstSeqNo=*/toOrigin.GetSeqNo(),
                                 /*origin=*/toDst.GetDestination(),
                                 /*lifetime=*/toOrigin.GetLifeTime());
        Ptr<Packet> packetToDst = Create<Packet>();
        SocketIpTtlTag gratTag;
        gratTag.SetTtl(toDst.GetHop());
        packetToDst->AddPacketTag(gratTag);
        packetToDst->AddHeader(gratRepHeader);
        TypeHeader type(SCRTYPE_RREP);
        packetToDst->AddHeader(type);
        Ptr<Socket> socket = FindSocketWithInterfaceAddress(toDst.GetInterface());
        NS_ASSERT(socket);
        NS_LOG_LOGIC("Send gratuitous RREP " << packet->GetUid());
        socket->SendTo(packetToDst, 0, InetSocketAddress(toDst.GetNextHop(), SCR_PORT));
    }
}

void
RoutingProtocol::SendReplyAck(Ipv4Address neighbor)
{
    NS_LOG_FUNCTION(this << " to " << neighbor);
    RrepAckHeader h;
    TypeHeader typeHeader(SCRTYPE_RREP_ACK);
    Ptr<Packet> packet = Create<Packet>();
    SocketIpTtlTag tag;
    tag.SetTtl(1);
    packet->AddPacketTag(tag);
    packet->AddHeader(h);
    packet->AddHeader(typeHeader);
    RoutingTableEntry toNeighbor;
    m_routingTable.LookupRoute(neighbor, toNeighbor);
    Ptr<Socket> socket = FindSocketWithInterfaceAddress(toNeighbor.GetInterface());
    NS_ASSERT(socket);
    socket->SendTo(packet, 0, InetSocketAddress(neighbor, SCR_PORT));
}

void
RoutingProtocol::RecvReply(Ptr<Packet> p, Ipv4Address receiver, Ipv4Address sender)
{
    NS_LOG_FUNCTION(this << " src " << sender);
    RrepHeader rrepHeader;
    p->RemoveHeader(rrepHeader);
    // R058: TAAODV HELLO fields, or the path pheromone of a reply.
    TaaodvExtHeader taaIn;
    bool taaHas = false;
    if (IsTaaodv() && p->GetSize() > 0)
    {
        p->RemoveHeader(taaIn);
        taaHas = true;
    }
    Ipv4Address dst = rrepHeader.GetDst();
    NS_LOG_LOGIC("RREP destination " << dst << " RREP origin " << rrepHeader.GetOrigin());

    uint8_t hop = rrepHeader.GetHopCount() + 1;
    rrepHeader.SetHopCount(hop);

    // If RREP is Hello message
    if (dst == rrepHeader.GetOrigin())
    {
        ProcessHello(rrepHeader, receiver);
        if (taaHas && taaIn.m_kind == TaaodvExtHeader::TAA_HELLO)
        {
            TaaOnHello(dst, taaIn);
        }
        return;
    }

    /*
     * If the route table entry to the destination is created or updated, then the following actions
     * occur:
     * -  the route is marked as active,
     * -  the destination sequence number is marked as valid,
     * -  the next hop in the route entry is assigned to be the node from which the RREP is
     * received, which is indicated by the source IP address field in the IP header,
     * -  the hop count is set to the value of the hop count from RREP message + 1
     * -  the expiry time is set to the current time plus the value of the Lifetime in the RREP
     * message,
     * -  and the destination sequence number is the Destination Sequence Number in the RREP
     * message.
     */
    Ptr<NetDevice> dev = m_ipv4->GetNetDevice(m_ipv4->GetInterfaceForAddress(receiver));
    RoutingTableEntry newEntry(
        /*dev=*/dev,
        /*dst=*/dst,
        /*vSeqNo=*/true,
        /*seqNo=*/rrepHeader.GetDstSeqno(),
        /*iface=*/m_ipv4->GetAddress(m_ipv4->GetInterfaceForAddress(receiver), 0),
        /*hops=*/hop,
        /*nextHop=*/sender,
        /*lifetime=*/rrepHeader.GetLifeTime());
    RoutingTableEntry toDst;
    // R058: within one HELLO interval of a TAAODV source's first reply, a later
    // reply for the same discovery replaces the route only if its path has more
    // pheromone per hop (Eq. 27); AODV's hop-count rule does not apply there.
    const double taaRatio =
        (taaHas && taaIn.m_kind == TaaodvExtHeader::TAA_RREP) ? taaIn.m_pheromone / hop : 0.0;
    bool taaWindow = false;
    bool taaBetter = false;
    if (IsTaaodv() && IsMyOwnAddress(rrepHeader.GetOrigin()))
    {
        auto sel = m_taaSel.find(dst);
        RoutingTableEntry cur;
        if (sel != m_taaSel.end() && Simulator::Now() <= sel->second.until &&
            rrepHeader.GetDstSeqno() == sel->second.seq &&
            m_routingTable.LookupRoute(dst, cur) && cur.GetFlag() == VALID)
        {
            taaWindow = true;
            taaBetter = taaRatio > sel->second.ratio;
            if (taaBetter)
            {
                sel->second.ratio = taaRatio;
                ++m_taaReselect;
            }
            else
            {
                ++m_taaKept;
            }
        }
    }
    if (taaWindow)
    {
        m_routingTable.LookupRoute(dst, toDst);
        if (taaBetter)
        {
            m_routingTable.Update(newEntry);
        }
    }
    else if (m_routingTable.LookupRoute(dst, toDst))
    {
        // The existing entry is updated only in the following circumstances:
        if (
            // (i) the sequence number in the routing table is marked as invalid in route table
            // entry.
            (!toDst.GetValidSeqNo()) ||

            // (ii) the Destination Sequence Number in the RREP is greater than the node's copy of
            // the destination sequence number and the known value is valid,
            ((int32_t(rrepHeader.GetDstSeqno()) - int32_t(toDst.GetSeqNo())) > 0) ||

            // (iii) the sequence numbers are the same, but the route is marked as inactive.
            (rrepHeader.GetDstSeqno() == toDst.GetSeqNo() && toDst.GetFlag() != VALID) ||

            // (iv) the sequence numbers are the same, and the New Hop Count is smaller than the
            // hop count in route table entry.
            (rrepHeader.GetDstSeqno() == toDst.GetSeqNo() && hop < toDst.GetHop()))
        {
            m_routingTable.Update(newEntry);
        }
    }
    else
    {
        // The forward route for this destination is created if it does not already exist.
        NS_LOG_LOGIC("add new route");
        m_routingTable.AddRoute(newEntry);
    }
    // Acknowledge receipt of the RREP by sending a RREP-ACK message back
    if (rrepHeader.GetAckRequired())
    {
        SendReplyAck(sender);
        rrepHeader.SetAckRequired(false);
    }
    NS_LOG_LOGIC("receiver " << receiver << " origin " << rrepHeader.GetOrigin());
    if (IsMyOwnAddress(rrepHeader.GetOrigin()))
    {
        if (toDst.GetFlag() == IN_SEARCH)
        {
            m_routingTable.Update(newEntry);
            m_addressReqTimer[dst].Cancel();
            m_addressReqTimer.erase(dst);
            // SCR (E3): RREP clears in_flight and cancels unsent discovery, but
            // does NOT close the episode and does NOT restore allowance.
            if (IsTaaodv())
            {
                TaaSel sel;
                sel.ratio = taaRatio;
                sel.until = Simulator::Now() + m_helloInterval;
                sel.seq = rrepHeader.GetDstSeqno();
                m_taaSel[dst] = sel;
            }
            if (m_scrEnabled)
            {
                ScrLedger(dst).OnRouteInstalled(Simulator::Now(), newEntry.GetHop());
                m_scrScheduler.Withdraw(dst);
                if (IsCardMode(m_algo))
                {
                    // A new route ends the recovery the recorded cause belonged to.
                    m_cardCause.erase(dst);
                }
            }
        }
        m_routingTable.LookupRoute(dst, toDst);
        SendPacketFromQueue(dst, toDst.GetRoute());
        return;
    }

    RoutingTableEntry toOrigin;
    if (!m_routingTable.LookupRoute(rrepHeader.GetOrigin(), toOrigin) ||
        toOrigin.GetFlag() == IN_SEARCH)
    {
        return; // Impossible! drop.
    }
    toOrigin.SetLifeTime(std::max(m_activeRouteTimeout, toOrigin.GetLifeTime()));
    m_routingTable.Update(toOrigin);

    // Update information about precursors
    if (m_routingTable.LookupValidRoute(rrepHeader.GetDst(), toDst))
    {
        toDst.InsertPrecursor(toOrigin.GetNextHop());
        m_routingTable.Update(toDst);

        RoutingTableEntry toNextHopToDst;
        m_routingTable.LookupRoute(toDst.GetNextHop(), toNextHopToDst);
        toNextHopToDst.InsertPrecursor(toOrigin.GetNextHop());
        m_routingTable.Update(toNextHopToDst);

        toOrigin.InsertPrecursor(toDst.GetNextHop());
        m_routingTable.Update(toOrigin);

        RoutingTableEntry toNextHopToOrigin;
        m_routingTable.LookupRoute(toOrigin.GetNextHop(), toNextHopToOrigin);
        toNextHopToOrigin.InsertPrecursor(toDst.GetNextHop());
        m_routingTable.Update(toNextHopToOrigin);
    }
    SocketIpTtlTag tag;
    p->RemovePacketTag(tag);
    if (tag.GetTtl() < 2)
    {
        NS_LOG_DEBUG("TTL exceeded. Drop RREP destination " << dst << " origin "
                                                            << rrepHeader.GetOrigin());
        return;
    }

    Ptr<Packet> packet = Create<Packet>();
    SocketIpTtlTag ttl;
    ttl.SetTtl(tag.GetTtl() - 1);
    packet->AddPacketTag(ttl);
    if (taaHas)
    {
        packet->AddHeader(taaIn);
    }
    packet->AddHeader(rrepHeader);
    TypeHeader tHeader(SCRTYPE_RREP);
    packet->AddHeader(tHeader);
    Ptr<Socket> socket = FindSocketWithInterfaceAddress(toOrigin.GetInterface());
    NS_ASSERT(socket);
    socket->SendTo(packet, 0, InetSocketAddress(toOrigin.GetNextHop(), SCR_PORT));
}

void
RoutingProtocol::RecvReplyAck(Ipv4Address neighbor)
{
    NS_LOG_FUNCTION(this);
    RoutingTableEntry rt;
    if (m_routingTable.LookupRoute(neighbor, rt))
    {
        rt.m_ackTimer.Cancel();
        rt.SetFlag(VALID);
        m_routingTable.Update(rt);
    }
}

void
RoutingProtocol::ProcessHello(const RrepHeader& rrepHeader, Ipv4Address receiver)
{
    NS_LOG_FUNCTION(this << "from " << rrepHeader.GetDst());
    /*
     *  Whenever a node receives a Hello message from a neighbor, the node
     * SHOULD make sure that it has an active route to the neighbor, and
     * create one if necessary.
     */
    RoutingTableEntry toNeighbor;
    if (!m_routingTable.LookupRoute(rrepHeader.GetDst(), toNeighbor))
    {
        Ptr<NetDevice> dev = m_ipv4->GetNetDevice(m_ipv4->GetInterfaceForAddress(receiver));
        RoutingTableEntry newEntry(
            /*dev=*/dev,
            /*dst=*/rrepHeader.GetDst(),
            /*vSeqNo=*/true,
            /*seqNo=*/rrepHeader.GetDstSeqno(),
            /*iface=*/m_ipv4->GetAddress(m_ipv4->GetInterfaceForAddress(receiver), 0),
            /*hops=*/1,
            /*nextHop=*/rrepHeader.GetDst(),
            /*lifetime=*/rrepHeader.GetLifeTime());
        m_routingTable.AddRoute(newEntry);
    }
    else
    {
        toNeighbor.SetLifeTime(
            std::max(Time(m_allowedHelloLoss * m_helloInterval), toNeighbor.GetLifeTime()));
        toNeighbor.SetSeqNo(rrepHeader.GetDstSeqno());
        toNeighbor.SetValidSeqNo(true);
        toNeighbor.SetFlag(VALID);
        toNeighbor.SetOutputDevice(m_ipv4->GetNetDevice(m_ipv4->GetInterfaceForAddress(receiver)));
        toNeighbor.SetInterface(m_ipv4->GetAddress(m_ipv4->GetInterfaceForAddress(receiver), 0));
        toNeighbor.SetHop(1);
        toNeighbor.SetNextHop(rrepHeader.GetDst());
        m_routingTable.Update(toNeighbor);
    }
    if (m_enableHello)
    {
        m_nb.Update(rrepHeader.GetDst(), Time(m_allowedHelloLoss * m_helloInterval));
    }
}

void
RoutingProtocol::RecvError(Ptr<Packet> p, Ipv4Address src)
{
    NS_LOG_FUNCTION(this << " from " << src);
    RerrHeader rerrHeader;
    p->RemoveHeader(rerrHeader);
    // CARD (R053): the forwarded RERR reuses this header, so the topology bit
    // travels with it; only the overflow split below clears it.
    const bool cardTopology = IsCardMode(m_algo) && rerrHeader.GetTopologyChange();
    std::map<Ipv4Address, uint32_t> dstWithNextHopSrc;
    std::map<Ipv4Address, uint32_t> unreachable;
    m_routingTable.GetListOfDestinationWithNextHop(src, dstWithNextHopSrc);
    std::pair<Ipv4Address, uint32_t> un;
    while (rerrHeader.RemoveUnDestination(un))
    {
        for (auto i = dstWithNextHopSrc.begin(); i != dstWithNextHopSrc.end(); ++i)
        {
            if (i->first == un.first)
            {
                unreachable.insert(un);
            }
        }
    }

    std::vector<Ipv4Address> precursors;
    for (auto i = unreachable.begin(); i != unreachable.end();)
    {
        if (!rerrHeader.AddUnDestination(i->first, i->second))
        {
            TypeHeader typeHeader(SCRTYPE_RERR);
            Ptr<Packet> packet = Create<Packet>();
            SocketIpTtlTag tag;
            tag.SetTtl(1);
            packet->AddPacketTag(tag);
            packet->AddHeader(rerrHeader);
            packet->AddHeader(typeHeader);
            SendRerrMessage(packet, precursors);
            rerrHeader.Clear();
            if (cardTopology)
            {
                rerrHeader.SetTopologyChange(true);
            }
        }
        else
        {
            RoutingTableEntry toDst;
            m_routingTable.LookupRoute(i->first, toDst);
            toDst.GetPrecursors(precursors);
            ++i;
        }
    }
    if (rerrHeader.GetDestCount() != 0)
    {
        TypeHeader typeHeader(SCRTYPE_RERR);
        Ptr<Packet> packet = Create<Packet>();
        SocketIpTtlTag tag;
        tag.SetTtl(1);
        packet->AddPacketTag(tag);
        packet->AddHeader(rerrHeader);
        packet->AddHeader(typeHeader);
        SendRerrMessage(packet, precursors);
    }
    if (IsCardMode(m_algo))
    {
        for (const auto& u : unreachable)
        {
            CardRecordCause(u.first, cardTopology);
        }
        if (cardTopology)
        {
            m_cardRerrTopologyRx += unreachable.size();
        }
    }
    m_routingTable.InvalidateRoutesWithDst(unreachable);
}

void
RoutingProtocol::RouteRequestTimerExpire(Ipv4Address dst)
{
    NS_LOG_LOGIC(this);
    RoutingTableEntry toDst;
    if (m_routingTable.LookupValidRoute(dst, toDst))
    {
        SendPacketFromQueue(dst, toDst.GetRoute());
        NS_LOG_LOGIC("route to " << dst << " found");
        return;
    }
    /*
     *  If a route discovery has been attempted RreqRetries times at the maximum TTL without
     *  receiving any RREP, all data packets destined for the corresponding destination SHOULD be
     *  dropped from the buffer and a Destination Unreachable message SHOULD be delivered to the
     * application.
     */
    // R028. This test was `==`, inherited from upstream AODV, where it is safe
    // because RouteRequestTimerExpire is the ONLY path that increments the
    // counter, so it always lands exactly on the cap. In this fork SendRequest
    // also increments from ScrOriginate, so the count can step PAST
    // m_rreqRetries without ever equalling it -- after which this give-up branch
    // never fires again, the counter runs away, and the retry backoff above
    // eventually overflows and kills the run.
    //
    // The counters below are the measurement instrument: with the fixed test,
    // an overshoot is impossible and m_rreqCapOvershot must be zero.
    if (toDst.GetRreqCnt() > m_rreqRetries)
    {
        ++m_rreqCapOvershot;
    }
    if (toDst.GetRreqCnt() > m_rreqMaxCnt)
    {
        m_rreqMaxCnt = toDst.GetRreqCnt();
    }
    const bool giveUp = m_rreqCapLegacyEquality
                            ? (toDst.GetRreqCnt() == m_rreqRetries)
                            : (toDst.GetRreqCnt() >= m_rreqRetries);
    if (giveUp)
    {
        NS_LOG_LOGIC("route discovery to " << dst << " has been attempted RreqRetries ("
                                           << m_rreqRetries << ") times with ttl "
                                           << m_netDiameter);
        m_addressReqTimer.erase(dst);
        m_routingTable.DeleteRoute(dst);
        NS_LOG_DEBUG("Route not found. Drop all packets with dst " << dst);
        m_queue.DropPacketWithDst(dst);
        return;
    }

    if (toDst.GetFlag() == IN_SEARCH)
    {
        NS_LOG_LOGIC("Resend RREQ to " << dst << " previous ttl " << toDst.GetHop());
        // SCR: a retry is a separately charged origination (E4) and must go
        // through admission like any other, never straight to SendRequest.
        if (m_scrEnabled)
        {
            ScrLedger(dst).OnReplyTimeout(Simulator::Now());
            ScrTryDiscovery(dst);
        }
        else
        {
            SendRequest(dst);
        }
    }
    else
    {
        NS_LOG_DEBUG("Route down. Stop search. Drop packet with destination " << dst);
        m_addressReqTimer.erase(dst);
        m_routingTable.DeleteRoute(dst);
        m_queue.DropPacketWithDst(dst);
    }
}

void
RoutingProtocol::HelloTimerExpire()
{
    NS_LOG_FUNCTION(this);
    if (IsTaaodv())
    {
        // R058: TAAODV reads its neighbours' fields from every HELLO ("requires
        // the periodic sending of HELLO control packets", Li et al., Sec. V-C),
        // so its HELLO is not deferred by other broadcasts.
        SendHello();
        m_htimer.Cancel();
        m_htimer.Schedule(m_helloInterval);
        m_lastBcastTime = Seconds(0);
        return;
    }
    Time offset;
    if (m_lastBcastTime.IsStrictlyPositive())
    {
        offset = Simulator::Now() - m_lastBcastTime;
        NS_LOG_DEBUG("Hello deferred due to last bcast at:" << m_lastBcastTime);
    }
    else
    {
        SendHello();
    }
    m_htimer.Cancel();
    Time diff = m_helloInterval - offset;
    m_htimer.Schedule(std::max(Seconds(0), diff));
    m_lastBcastTime = Seconds(0);
}

void
RoutingProtocol::RreqRateLimitTimerExpire()
{
    NS_LOG_FUNCTION(this);
    m_rreqCount = 0;
    m_rreqRateLimitTimer.Schedule(Seconds(1));
}

void
RoutingProtocol::RerrRateLimitTimerExpire()
{
    NS_LOG_FUNCTION(this);
    m_rerrCount = 0;
    m_rerrRateLimitTimer.Schedule(Seconds(1));
}

void
RoutingProtocol::AckTimerExpire(Ipv4Address neighbor, Time blacklistTimeout)
{
    NS_LOG_FUNCTION(this);
    m_routingTable.MarkLinkAsUnidirectional(neighbor, blacklistTimeout);
}

void
RoutingProtocol::SendHello()
{
    NS_LOG_FUNCTION(this);
    /* Broadcast a RREP with TTL = 1 with the RREP message fields set as follows:
     *   Destination IP Address         The node's IP address.
     *   Destination Sequence Number    The node's latest sequence number.
     *   Hop Count                      0
     *   Lifetime                       AllowedHelloLoss * HelloInterval
     */
    for (auto j = m_socketAddresses.begin(); j != m_socketAddresses.end(); ++j)
    {
        Ptr<Socket> socket = j->first;
        Ipv4InterfaceAddress iface = j->second;
        RrepHeader helloHeader(/*prefixSize=*/0,
                               /*hopCount=*/0,
                               /*dst=*/iface.GetLocal(),
                               /*dstSeqNo=*/m_seqNo,
                               /*origin=*/iface.GetLocal(),
                               /*lifetime=*/Time(m_allowedHelloLoss * m_helloInterval));
        Ptr<Packet> packet = Create<Packet>();
        SocketIpTtlTag tag;
        tag.SetTtl(1);
        packet->AddPacketTag(tag);
        if (IsTaaodv())
        {
            TaaodvExtHeader ext;
            ext.m_kind = TaaodvExtHeader::TAA_HELLO;
            Vector pos;
            Vector vel;
            TaaMyKinematics(pos, vel);
            ext.m_x = static_cast<float>(pos.x);
            ext.m_y = static_cast<float>(pos.y);
            ext.m_vx = static_cast<float>(vel.x);
            ext.m_vy = static_cast<float>(vel.y);
            ext.m_addrs = TaaLiveNeighbors();
            if (ext.m_addrs.size() > 255)
            {
                ext.m_addrs.resize(255);
            }
            packet->AddHeader(ext);
        }
        packet->AddHeader(helloHeader);
        TypeHeader tHeader(SCRTYPE_RREP);
        packet->AddHeader(tHeader);
        // Send to all-hosts broadcast if on /32 addr, subnet-directed otherwise
        Ipv4Address destination;
        if (iface.GetMask() == Ipv4Mask::GetOnes())
        {
            destination = Ipv4Address("255.255.255.255");
        }
        else
        {
            destination = iface.GetBroadcast();
        }
        Time jitter = MilliSeconds(m_uniformRandomVariable->GetInteger(0, 10));
        Simulator::Schedule(jitter, &RoutingProtocol::SendTo, this, socket, packet, destination);
    }
}

void
RoutingProtocol::SendPacketFromQueue(Ipv4Address dst, Ptr<Ipv4Route> route)
{
    NS_LOG_FUNCTION(this);
    QueueEntry queueEntry;
    while (m_queue.Dequeue(dst, queueEntry))
    {
        DeferredRouteOutputTag tag;
        Ptr<Packet> p = ConstCast<Packet>(queueEntry.GetPacket());
        if (p->RemovePacketTag(tag) && tag.GetInterface() != -1 &&
            tag.GetInterface() != m_ipv4->GetInterfaceForDevice(route->GetOutputDevice()))
        {
            NS_LOG_DEBUG("Output device doesn't match. Dropped.");
            return;
        }
        UnicastForwardCallback ucb = queueEntry.GetUnicastForwardCallback();
        Ipv4Header header = queueEntry.GetIpv4Header();
        header.SetSource(route->GetSource());
        header.SetTtl(header.GetTtl() +
                      1); // compensate extra TTL decrement by fake loopback routing
        ucb(route, p, header);
    }
}

void
RoutingProtocol::SendRerrWhenBreaksLinkToNextHop(Ipv4Address nextHop)
{
    NS_LOG_FUNCTION(this << nextHop);
    RerrHeader rerrHeader;
    std::vector<Ipv4Address> precursors;
    std::map<Ipv4Address, uint32_t> unreachable;

    RoutingTableEntry toNextHop;
    if (!m_routingTable.LookupRoute(nextHop, toNextHop))
    {
        return;
    }
    toNextHop.GetPrecursors(precursors);
    // CARD (R053): this is the only node that observed the lost neighbour, so
    // the cause is judged here and carried to the sources in the RERR.
    bool cardTopology = false;
    if (IsCardMode(m_algo))
    {
        double dbm = 0.0;
        double trendDb = 0.0;
        double ageS = 0.0;
        const CardCause c = CardClassify(nextHop, dbm, trendDb, ageS);
        switch (c)
        {
        case CARD_TOPOLOGY: ++m_cardBreakTopology; break;
        case CARD_TRANSIENT: ++m_cardBreakTransient; break;
        case CARD_UNKNOWN: ++m_cardBreakUnknown; break;
        }
        if (!m_cardBreakLog.IsNull())
        {
            m_cardBreakLog(m_ipv4->GetAddress(1, 0).GetLocal(), nextHop, dbm, trendDb, ageS,
                           static_cast<uint8_t>(c));
        }
        cardTopology = CardReportsTopology(c);
        rerrHeader.SetTopologyChange(cardTopology);
    }
    rerrHeader.AddUnDestination(nextHop, toNextHop.GetSeqNo());
    m_routingTable.GetListOfDestinationWithNextHop(nextHop, unreachable);
    for (auto i = unreachable.begin(); i != unreachable.end();)
    {
        if (!rerrHeader.AddUnDestination(i->first, i->second))
        {
            NS_LOG_LOGIC("Send RERR message with maximum size.");
            TypeHeader typeHeader(SCRTYPE_RERR);
            Ptr<Packet> packet = Create<Packet>();
            SocketIpTtlTag tag;
            tag.SetTtl(1);
            packet->AddPacketTag(tag);
            packet->AddHeader(rerrHeader);
            packet->AddHeader(typeHeader);
            SendRerrMessage(packet, precursors);
            rerrHeader.Clear();
            if (cardTopology)
            {
                rerrHeader.SetTopologyChange(true);
            }
        }
        else
        {
            RoutingTableEntry toDst;
            m_routingTable.LookupRoute(i->first, toDst);
            toDst.GetPrecursors(precursors);
            ++i;
        }
    }
    if (rerrHeader.GetDestCount() != 0)
    {
        TypeHeader typeHeader(SCRTYPE_RERR);
        Ptr<Packet> packet = Create<Packet>();
        SocketIpTtlTag tag;
        tag.SetTtl(1);
        packet->AddPacketTag(tag);
        packet->AddHeader(rerrHeader);
        packet->AddHeader(typeHeader);
        SendRerrMessage(packet, precursors);
    }
    unreachable.insert(std::make_pair(nextHop, toNextHop.GetSeqNo()));
    if (IsCardMode(m_algo))
    {
        // A source whose own next hop was lost learns the cause here, not from
        // a RERR, because a node sends no RERR to itself.
        for (const auto& u : unreachable)
        {
            CardRecordCause(u.first, cardTopology);
        }
    }
    m_routingTable.InvalidateRoutesWithDst(unreachable);
}

void
RoutingProtocol::SendRerrWhenNoRouteToForward(Ipv4Address dst,
                                              uint32_t dstSeqNo,
                                              Ipv4Address origin)
{
    NS_LOG_FUNCTION(this);
    // A node SHOULD NOT originate more than RERR_RATELIMIT RERR messages per second.
    if (m_rerrCount == m_rerrRateLimit)
    {
        // Just make sure that the RerrRateLimit timer is running and will expire
        NS_ASSERT(m_rerrRateLimitTimer.IsRunning());
        // discard the packet and return
        NS_LOG_LOGIC("RerrRateLimit reached at "
                     << Simulator::Now().As(Time::S) << " with timer delay left "
                     << m_rerrRateLimitTimer.GetDelayLeft().As(Time::S) << "; suppressing RERR");
        return;
    }
    RerrHeader rerrHeader;
    rerrHeader.AddUnDestination(dst, dstSeqNo);
    if (IsCardMode(m_algo))
    {
        // CARD (R053): a node that has already lost its route to dst to a
        // topology change passes that cause on when data still arrives for it.
        auto c = m_cardCause.find(dst);
        if (c != m_cardCause.end() && c->second.first &&
            Simulator::Now() - c->second.second <= m_activeRouteTimeout)
        {
            rerrHeader.SetTopologyChange(true);
        }
    }
    RoutingTableEntry toOrigin;
    Ptr<Packet> packet = Create<Packet>();
    SocketIpTtlTag tag;
    tag.SetTtl(1);
    packet->AddPacketTag(tag);
    packet->AddHeader(rerrHeader);
    packet->AddHeader(TypeHeader(SCRTYPE_RERR));
    if (m_routingTable.LookupValidRoute(origin, toOrigin))
    {
        Ptr<Socket> socket = FindSocketWithInterfaceAddress(toOrigin.GetInterface());
        NS_ASSERT(socket);
        NS_LOG_LOGIC("Unicast RERR to the source of the data transmission");
        socket->SendTo(packet, 0, InetSocketAddress(toOrigin.GetNextHop(), SCR_PORT));
    }
    else
    {
        for (auto i = m_socketAddresses.begin(); i != m_socketAddresses.end(); ++i)
        {
            Ptr<Socket> socket = i->first;
            Ipv4InterfaceAddress iface = i->second;
            NS_ASSERT(socket);
            NS_LOG_LOGIC("Broadcast RERR message from interface " << iface.GetLocal());
            // Send to all-hosts broadcast if on /32 addr, subnet-directed otherwise
            Ipv4Address destination;
            if (iface.GetMask() == Ipv4Mask::GetOnes())
            {
                destination = Ipv4Address("255.255.255.255");
            }
            else
            {
                destination = iface.GetBroadcast();
            }
            socket->SendTo(packet->Copy(), 0, InetSocketAddress(destination, SCR_PORT));
        }
    }
}

void
RoutingProtocol::SendRerrMessage(Ptr<Packet> packet, std::vector<Ipv4Address> precursors)
{
    NS_LOG_FUNCTION(this);

    if (precursors.empty())
    {
        NS_LOG_LOGIC("No precursors");
        return;
    }
    // A node SHOULD NOT originate more than RERR_RATELIMIT RERR messages per second.
    if (m_rerrCount == m_rerrRateLimit)
    {
        // Just make sure that the RerrRateLimit timer is running and will expire
        NS_ASSERT(m_rerrRateLimitTimer.IsRunning());
        // discard the packet and return
        NS_LOG_LOGIC("RerrRateLimit reached at "
                     << Simulator::Now().As(Time::S) << " with timer delay left "
                     << m_rerrRateLimitTimer.GetDelayLeft().As(Time::S) << "; suppressing RERR");
        return;
    }
    // If there is only one precursor, RERR SHOULD be unicast toward that precursor
    if (precursors.size() == 1)
    {
        RoutingTableEntry toPrecursor;
        if (m_routingTable.LookupValidRoute(precursors.front(), toPrecursor))
        {
            Ptr<Socket> socket = FindSocketWithInterfaceAddress(toPrecursor.GetInterface());
            NS_ASSERT(socket);
            NS_LOG_LOGIC("one precursor => unicast RERR to "
                         << toPrecursor.GetDestination() << " from "
                         << toPrecursor.GetInterface().GetLocal());
            Simulator::Schedule(MilliSeconds(m_uniformRandomVariable->GetInteger(0, 10)),
                                &RoutingProtocol::SendTo,
                                this,
                                socket,
                                packet,
                                precursors.front());
            m_rerrCount++;
        }
        return;
    }

    //  Should only transmit RERR on those interfaces which have precursor nodes for the broken
    //  route
    std::vector<Ipv4InterfaceAddress> ifaces;
    RoutingTableEntry toPrecursor;
    for (auto i = precursors.begin(); i != precursors.end(); ++i)
    {
        if (m_routingTable.LookupValidRoute(*i, toPrecursor) &&
            std::find(ifaces.begin(), ifaces.end(), toPrecursor.GetInterface()) == ifaces.end())
        {
            ifaces.push_back(toPrecursor.GetInterface());
        }
    }

    for (auto i = ifaces.begin(); i != ifaces.end(); ++i)
    {
        Ptr<Socket> socket = FindSocketWithInterfaceAddress(*i);
        NS_ASSERT(socket);
        NS_LOG_LOGIC("Broadcast RERR message from interface " << i->GetLocal());
        // std::cout << "Broadcast RERR message from interface " << i->GetLocal () << std::endl;
        // Send to all-hosts broadcast if on /32 addr, subnet-directed otherwise
        Ptr<Packet> p = packet->Copy();
        Ipv4Address destination;
        if (i->GetMask() == Ipv4Mask::GetOnes())
        {
            destination = Ipv4Address("255.255.255.255");
        }
        else
        {
            destination = i->GetBroadcast();
        }
        Simulator::Schedule(MilliSeconds(m_uniformRandomVariable->GetInteger(0, 10)),
                            &RoutingProtocol::SendTo,
                            this,
                            socket,
                            p,
                            destination);
    }
}

Ptr<Socket>
RoutingProtocol::FindSocketWithInterfaceAddress(Ipv4InterfaceAddress addr) const
{
    NS_LOG_FUNCTION(this << addr);
    for (auto j = m_socketAddresses.begin(); j != m_socketAddresses.end(); ++j)
    {
        Ptr<Socket> socket = j->first;
        Ipv4InterfaceAddress iface = j->second;
        if (iface == addr)
        {
            return socket;
        }
    }
    Ptr<Socket> socket;
    return socket;
}

Ptr<Socket>
RoutingProtocol::FindSubnetBroadcastSocketWithInterfaceAddress(Ipv4InterfaceAddress addr) const
{
    NS_LOG_FUNCTION(this << addr);
    for (auto j = m_socketSubnetBroadcastAddresses.begin();
         j != m_socketSubnetBroadcastAddresses.end();
         ++j)
    {
        Ptr<Socket> socket = j->first;
        Ipv4InterfaceAddress iface = j->second;
        if (iface == addr)
        {
            return socket;
        }
    }
    Ptr<Socket> socket;
    return socket;
}

void
RoutingProtocol::DoInitialize()
{
    NS_LOG_FUNCTION(this);

    NS_ABORT_MSG_IF(m_ttlStart > m_netDiameter,
                    "SCR: configuration error, TtlStart ("
                        << m_ttlStart << ") must be less than or equal to NetDiameter ("
                        << m_netDiameter << ").");

    if (m_enableHello)
    {
        m_htimer.SetFunction(&RoutingProtocol::HelloTimerExpire, this);
        uint32_t startTime = m_uniformRandomVariable->GetInteger(0, 100);
        NS_LOG_DEBUG("Starting at time " << startTime << "ms");
        m_htimer.Schedule(MilliSeconds(startTime));
    }
    Ipv4RoutingProtocol::DoInitialize();
}


// ===========================================================================
// SCR additions (E3-E6). Traceability M13/M14 (routing integration), M09.
// ===========================================================================

RepairLedger&
RoutingProtocol::ScrLedger(Ipv4Address dst)
{
    // Ledgers are created on demand and NEVER erased. E3 requires a tombstone
    // that survives route deletion and application restart, so that restarting
    // an application cannot reset accumulated debt.
    auto it = m_scrLedgers.find(dst);
    if (it == m_scrLedgers.end())
    {
        RepairLedger fresh;
        fresh.SetAlgorithm(m_algo);
        fresh.SetB(m_scrB);
        fresh.SetRho((m_algo == ALGO_NO_PROBE) ? 0.0 : m_scrRho);
        fresh.SetBucketRefill(m_scrBucketRefill);
        fresh.SetTtlMax(m_netDiameter);
        it = m_scrLedgers.emplace(dst, fresh).first;
    }
    return it->second;
}

void
RoutingProtocol::ScrSetDemand(Ipv4Address dst, bool hasDemand)
{
    if (hasDemand)
    {
        m_scrDemand.insert(dst);
    }
    else
    {
        m_scrDemand.erase(dst);
        m_scrScheduler.Withdraw(dst);
        ScrLedger(dst).OnSessionStop(Simulator::Now());
    }
}

void
RoutingProtocol::ScrOnServiceConfirmed(Ipv4Address dst)
{
    RoutingTableEntry rt;
    const bool routeExists = m_routingTable.LookupValidRoute(dst, rt);
    // E3: a confirmation renews allowance only while a usable route exists.
    // Otherwise the evidence is retained and re-evaluated after an ordinary RREP.
    ScrLedger(dst).OnServiceConfirmed(Simulator::Now(), routeExists);
}

bool
RoutingProtocol::ScrTryDiscovery(Ipv4Address dst)
{
    // THE single admission point. Every source origination path routes here and
    // nowhere else (T10 asserts admissions == RREQs that reach the wire).
    const Time now = Simulator::Now();
    RepairLedger& led = ScrLedger(dst);

    const bool hasDemand = (m_scrDemand.find(dst) != m_scrDemand.end());

    RoutingTableEntry rt;
    if (m_routingTable.LookupValidRoute(dst, rt))
    {
        // A usable route exists; no discovery is needed.
        m_scrScheduler.Withdraw(dst);
        auto it = m_scrReconsiderEvent.find(dst);
        if (it != m_scrReconsiderEvent.end() && it->second.IsPending())
        {
            Simulator::Cancel(it->second);
        }
        return false;
    }

    led.OnDiscoveryNeeded(now);
    // E9: never allow a stale in_flight marker to lock this pair out
    // permanently. See RepairLedger::ExpireIfDue.
    led.ExpireIfDue(now);

    if (IsCardMode(m_algo))
    {
        // R053/R054: the recovery follows a topology change when the break
        // that caused it was reported as one. The ledger then decides, per
        // variant, whether that exempts it and whether it starts at the reach
        // hint. A pair with no recorded break (first discovery, route expiry)
        // is treated as transient.
        auto c = m_cardCause.find(dst);
        led.SetCardTopology(c != m_cardCause.end() && c->second.first);
    }

    const AdmitDecision d = led.Evaluate(now, hasDemand);
    if (d != ADMIT_FAST && d != ADMIT_PROBE && d != ADMIT_EXEMPT)
    {
        ++m_scrDenied;
        switch (d)
        {
        case DENY_NO_DEMAND: ++m_denyNoDemand; break;
        case DENY_IN_FLIGHT: ++m_denyInFlight; break;
        case DENY_NOT_ELIGIBLE:
            ++m_denyNotEligible;
            if (led.ReplyReceivedSinceSend())
            {
                ++m_denyCooldownAfterReply;
            }
            break;
        case DENY_NO_ALLOWANCE: ++m_denyNoAllowance; break;
        default: break;
        }
        NS_LOG_INFO("SCR: origination to " << dst << " denied: " << AdmitDecisionName(d));
        if (d == DENY_NOT_ELIGIBLE || d == DENY_NO_ALLOWANCE)
        {
            m_scrScheduler.MarkReady(dst, now);
        }
        return false;
    }

    // NO-NODE-CAP ablation (R037): the per-node aggregate limiter G and its FIFO
    // ordering are the component under test, so this arm skips both and is
    // bounded only by the per-pair allowance. Until 2026-09-19 this mode existed
    // as an enum value that nothing read, so it silently reproduced SCR exactly.
    const bool nodeCapEnabled = (m_algo != ALGO_NO_NODE_CAP);

    // E6: a node token is also required, and destinations are served FIFO.
    if (nodeCapEnabled && !m_scrScheduler.HasToken(now))
    {
        ++m_scrDenied;
        ++m_denyNoNodeToken;
        m_scrScheduler.MarkReady(dst, now);
        // Schedule at most ONE pending re-evaluation per destination. Scheduling
        // one per denial makes the event queue grow without bound under sustained
        // token starvation (see m_scrReconsiderEvent).
        auto it = m_scrReconsiderEvent.find(dst);
        if (it == m_scrReconsiderEvent.end() || !it->second.IsPending())
        {
            // R014 LIVELOCK FIX. MarkReady() above calls Refill(), which
            // mutates the token count. NextTokenTime() therefore re-reads a
            // DIFFERENT state than the HasToken() test that put us in this
            // branch, and can disagree with it by one ULP: it sees >= 1.0,
            // takes its "a token is already available" early return, and hands
            // back `now`. Scheduling ScrReconsider at `now` re-enters this
            // method at the same timestamp with the same state and schedules
            // again -- a zero-delay loop that freezes simulated time at 100%
            // CPU, with a constant event-queue size and no log output to
            // betray it. It stalled a 300 s run past 2 hours at t=128.6 s.
            //
            // The caller has already decided no token is available, so the
            // retry must be strictly in the future regardless of what the
            // scheduler reports. One time unit is sufficient and cannot
            // reorder anything observable at this timestamp.
            Time delay = m_scrScheduler.NextTokenTime(now) - now;
            if (!delay.IsStrictlyPositive())
            {
                delay = TimeStep(1);
                ++m_scrZeroDelayClamped;
            }
            NS_LOG_INFO("SCR: origination to " << dst << " denied: no_node_token"
                        << " tokens=" << m_scrScheduler.GetTokens(now)
                        << " retry_in=" << delay.As(Time::US));
            m_scrReconsiderEvent[dst] =
                Simulator::Schedule(delay, &RoutingProtocol::ScrReconsider, this, dst);
        }
        return false;
    }
    Ipv4Address head;
    if (nodeCapEnabled && m_scrScheduler.PeekNext(head) && head != dst
        && m_scrScheduler.IsQueued(dst))
    {
        // Another destination has been waiting longer; do not jump the queue.
        ++m_scrDenied;
        ++m_denyFifo;
        return false;
    }

    const uint16_t ttl = (d == ADMIT_PROBE) ? m_netDiameter : led.TtlForNextAttempt();
    return ScrOriginate(dst, ttl, d);
}

bool
RoutingProtocol::ScrOriginate(Ipv4Address dst, uint16_t ttl, AdmitDecision kind)
{
    const Time now = Simulator::Now();
    RepairLedger& led = ScrLedger(dst);

    // Record the decision so SendRequest uses the ledger TTL rather than the
    // native expanding ring, then perform the ordinary AODV origination.
    m_scrPendingSend[dst] = kind;
    ++m_scrAdmitted;

    SendRequest(dst);

    // If the native rate-limit branch deferred this attempt, the pending record
    // was cleared there and nothing reached the wire: do not debit.
    if (m_scrPendingSend.find(dst) == m_scrPendingSend.end())
    {
        --m_scrAdmitted;
        return false;
    }
    m_scrPendingSend.erase(dst);

    // E6: debit the pair allowance and the node budget ATOMICALLY, and only
    // because a socket handoff was attempted. Radio loss after this point never
    // refunds.
    const Time timeout = led.ReplyTimeout(ttl);
    led.CommitSend(now, kind, timeout);
    if (m_algo != ALGO_NO_NODE_CAP)
    {
        // NO-NODE-CAP (R037) bypasses the node budget at admission, so it holds
        // no token to spend here. Debiting anyway trips
        // "OriginScheduler::CommitSend with no node token" and aborts the run --
        // which is exactly what the first version of this ablation did, 32 s in.
        m_scrScheduler.CommitSend(now, dst);
    }
    ++m_scrWireRreq;
    if (kind == ADMIT_EXEMPT)
    {
        ++m_cardExemptAdmitted;
    }

    NS_LOG_INFO("SCR: originated to " << dst << " kind=" << AdmitDecisionName(kind)
                                      << " ttl=" << ttl << " timeout=" << timeout.As(Time::S)
                                      << " B_remaining=" << led.GetRemainingFast()
                                      << " j=" << led.GetEscalationIndex());
    return true;
}

void
RoutingProtocol::ScrReconsider(Ipv4Address dst)
{
    if (!m_scrEnabled)
    {
        return;
    }
    ScrTryDiscovery(dst);
}


Time
RoutingProtocol::ScrGetEpisodeStart(Ipv4Address dst)
{
    return ScrLedger(dst).GetEpisodeStart();
}

uint64_t
RoutingProtocol::ScrGetRenewals(Ipv4Address dst)
{
    return ScrLedger(dst).GetRenewals();
}

uint64_t
RoutingProtocol::ScrGetProbesSpent(Ipv4Address dst)
{
    return ScrLedger(dst).GetProbesSpent();
}

void
RoutingProtocol::SetAlgorithm(AlgorithmMode a)
{
    m_algo = a;
    // AODV-STOCK and AODV-FB use the native discovery path unchanged; the SCR
    // admission machinery is switched off entirely rather than configured to be
    // permissive, so those references really are native AODV.
    m_scrEnabled = (a != ALGO_AODV_STOCK && a != ALGO_AODV_FB && a != ALGO_CLAF_AODV &&
                    a != ALGO_TAAODV);
    if (a == ALGO_TAAODV && !m_taaSamplerStarted)
    {
        // R058: queue-occupancy measurements for the load ratio Cn (Eq. 20).
        m_taaSamplerStarted = true;
        m_taaFwdCache.SetLifetime(m_rreqIdCache.GetLifeTime());
        Simulator::Schedule(m_taaSampleInterval, &RoutingProtocol::TaaSample, this);
    }
    if (IsClafMode(a) && !m_clafSamplerStarted)
    {
        // R057: CLAF's cross-layer inputs are refreshed every 2 s (T_int in
        // Safari et al.). Scheduled only in this mode, so no other mode gains
        // an event.
        m_clafSamplerStarted = true;
        m_clafWindowStart = Simulator::Now();
        Simulator::Schedule(Seconds(2), &RoutingProtocol::ClafSample, this);
    }
    for (auto& kv : m_scrLedgers)
    {
        kv.second.SetAlgorithm(a);
    }
}

// ---------------------------------------------------------------------------
// CLAF-AODV (R057). Membership functions and rules transcribed from Safari et
// al., IEEE Access 11 (2023): Figs. 5, 6, 8, 9 and Tables 1, 4.
namespace
{
struct Trap
{
    double a, b, c, d; // trapezoid; a triangle has b == c
    double Mu(double x) const
    {
        if (x < a || x > d)
        {
            return 0.0;
        }
        if (x >= b && x <= c)
        {
            return 1.0;
        }
        return (x < b) ? (x - a) / (b - a) : (d - x) / (d - c);
    }
};

// Level 1 inputs.
const Trap kCgD[3] = {{0, 0, 0.2, 0.4}, {0.2, 0.4, 0.6, 0.8}, {0.6, 0.8, 1, 1}};
const Trap kBF[2] = {{0, 0, 0.2, 0.3}, {0.2, 0.3, 1, 1}};
const Trap kErem[2] = {{0, 0, 0.1, 0.3}, {0.1, 0.3, 1, 1}};
const Trap kCtD[3] = {{0, 0, 0.05, 0.062}, {0.05, 0.062, 0.124, 0.249}, {0.124, 0.249, 1, 1}};
// Level 1 output and level 2 NQM / PQM inputs (Figs. 6, 8b, 8c).
const Trap kQ[3] = {{0, 0, 0.2, 0.4}, {0.2, 0.4, 0.5, 0.7}, {0.5, 0.7, 1, 1}};
// Level 2 node degree (Fig. 8a).
const Trap kND[3] = {{0, 0, 6, 8}, {6, 8, 10, 12}, {10, 12, 1e9, 1e9}};
// Level 2 output (Fig. 9): VL, L, M, H, VH.
const Trap kP[5] = {{0.1, 0.1, 0.15, 0.2},
                    {0.15, 0.2, 0.2, 0.25},
                    {0.2, 0.25, 0.25, 0.3},
                    {0.25, 0.3, 0.3, 0.5},
                    {0.3, 0.5, 0.9, 0.9}};

// Received signal strength sets (Fig. 5e) are defined at the powers of Table 3,
// i.e. at 150, 120, 100, 80 and 50 m of a 150 m radio. We place them at the same
// fractions of our 141 m range and convert with our channel (log-distance,
// PL(1 m) = 40.046 dB, exponent 2.7, 16 dBm).
double OurRssAt(double d)
{
    return 16.0 - 40.046 - 27.0 * std::log10(d);
}

Trap RssSet(int level)
{
    const double k = 141.0 / 150.0;
    const double p120 = OurRssAt(120 * k), p100 = OurRssAt(100 * k);
    const double p80 = OurRssAt(80 * k), p50 = OurRssAt(50 * k);
    if (level == 0)
    {
        return {-300, -300, p120, p100};
    }
    if (level == 1)
    {
        return {p120, p100, p80, p50};
    }
    return {p80, p50, 100, 100};
}

// Table 1: node quality (0 L, 1 M, 2 H) for (E, BW, CgD, CtD, RSS).
int Table1(int e, int bw, int cg, int ct, int rss)
{
    if (bw == 0)
    {
        return 0;
    }
    if (cg == 2 || ct == 2)
    {
        return 0;
    }
    // BW high, CgD in {L, M}, CtD in {L, M}
    if (e == 1 && rss != 0 && cg == 0 && ct == 0)
    {
        return 2; // rules 28 and 100
    }
    return 1;
}

// Table 4: forwarding level (0 VL .. 4 VH) for (ND, NQM, PQM).
const int kTable4[3][3][3] = {
    // ND = L
    {{2, 2, 3}, {2, 2, 4}, {2, 3, 4}},
    // ND = M
    {{1, 2, 2}, {1, 2, 3}, {2, 2, 3}},
    // ND = H
    {{0, 1, 2}, {0, 1, 2}, {0, 1, 2}},
};

// Mamdani (min, max) with last-of-maxima defuzzification over [lo, hi].
double DefuzzLom(const Trap* sets, const double* strength, int nsets, double lo, double hi)
{
    const int steps = 2000;
    double best = -1.0;
    double arg = lo;
    for (int i = 0; i <= steps; ++i)
    {
        const double xv = lo + (hi - lo) * i / steps;
        double mu = 0.0;
        for (int s = 0; s < nsets; ++s)
        {
            mu = std::max(mu, std::min(strength[s], sets[s].Mu(xv)));
        }
        if (mu >= best - 1e-12)
        {
            best = std::max(best, mu);
            arg = xv; // keeps the LAST x at the maximum
        }
    }
    return arg;
}
} // namespace

void
RoutingProtocol::AssignClafStream(int64_t stream)
{
    if (!m_clafRng)
    {
        m_clafRng = CreateObject<UniformRandomVariable>();
    }
    m_clafRng->SetStream(stream);
}

void
RoutingProtocol::ClafPhyState(Time start, Time duration, WifiPhyState state)
{
    if (!IsClaf())
    {
        return;
    }
    if (state == WifiPhyState::IDLE)
    {
        m_clafIdleAccum += duration;
    }
}

void
RoutingProtocol::ClafSample()
{
    const Time now = Simulator::Now();
    const Time window = now - m_clafWindowStart;
    if (window.IsStrictlyPositive())
    {
        // Eq. (3): share of the window the channel was idle.
        m_clafIdleFrac = std::min(1.0, m_clafIdleAccum.GetSeconds() / window.GetSeconds());
    }
    m_clafIdleAccum = Seconds(0);
    m_clafWindowStart = now;
    if (m_clafMac)
    {
        // Eqs. (1)-(2): EWMA of MAC queue occupancy.
        Ptr<WifiMacQueue> q = m_clafMac->GetTxopQueue(AC_BE_NQOS);
        if (q)
        {
            const double occ = static_cast<double>(q->GetNPackets()) /
                               std::max<double>(1.0, q->GetMaxSize().GetValue());
            m_clafQueueAvg = m_clafEwmaAlpha * occ + (1.0 - m_clafEwmaAlpha) * m_clafQueueAvg;
        }
        // Eqs. (7)-(8): EWMA of the contention window.
        Ptr<Txop> txop = m_clafMac->GetTxop();
        if (txop)
        {
            const double cw = txop->GetCw(SINGLE_LINK_OP_ID);
            m_clafCwAvg = m_clafCwInit ? m_clafEwmaAlpha * cw + (1.0 - m_clafEwmaAlpha) * m_clafCwAvg
                                       : cw;
            m_clafCwInit = true;
        }
    }
    Simulator::Schedule(Seconds(2), &RoutingProtocol::ClafSample, this);
}

double
RoutingProtocol::ClafRssDbm(Ipv4Address src)
{
    Mac48Address mac = m_nb.GetMacAddress(src);
    if (mac == Mac48Address())
    {
        auto learned = m_cardIpMac.find(src);
        if (learned != m_cardIpMac.end())
        {
            mac = learned->second;
        }
    }
    auto it = m_cardObs.find(mac);
    return (it == m_cardObs.end()) ? std::nan("") : it->second.curDbm;
}

double
RoutingProtocol::ClafNodeQuality(double rssDbm)
{
    const double erem = 1.0; // no energy model in our scenarios
    const double bf = m_clafIdleFrac;
    const double cgd = m_clafQueueAvg;
    const double ctd = m_clafCwInit ? std::min(1.0, m_clafCwAvg / 1023.0) : 0.0; // Table 2
    double muE[2], muB[2], muCg[3], muCt[3], muR[3];
    for (int i = 0; i < 2; ++i)
    {
        muE[i] = kErem[i].Mu(erem);
        muB[i] = kBF[i].Mu(bf);
    }
    for (int i = 0; i < 3; ++i)
    {
        muCg[i] = kCgD[i].Mu(std::min(1.0, cgd));
        muCt[i] = kCtD[i].Mu(ctd);
        // Unknown sender power: treat as medium (no evidence either way).
        muR[i] = std::isnan(rssDbm) ? (i == 1 ? 1.0 : 0.0) : RssSet(i).Mu(rssDbm);
    }
    double strength[3] = {0, 0, 0};
    for (int e = 0; e < 2; ++e)
        for (int b = 0; b < 2; ++b)
            for (int g = 0; g < 3; ++g)
                for (int t = 0; t < 3; ++t)
                    for (int r = 0; r < 3; ++r)
                    {
                        const double w =
                            std::min({muE[e], muB[b], muCg[g], muCt[t], muR[r]});
                        if (w > 0.0)
                        {
                            const int out = Table1(e, b, g, t, r);
                            strength[out] = std::max(strength[out], w);
                        }
                    }
    return DefuzzLom(kQ, strength, 3, 0.0, 1.0);
}

double
RoutingProtocol::ClafForwardProbability(uint32_t nd, double nqm, double pqm)
{
    double muN[3], muQ[3], muP[3];
    for (int i = 0; i < 3; ++i)
    {
        muN[i] = kND[i].Mu(static_cast<double>(nd));
        muQ[i] = kQ[i].Mu(nqm);
        muP[i] = kQ[i].Mu(pqm);
    }
    double strength[5] = {0, 0, 0, 0, 0};
    for (int a = 0; a < 3; ++a)
        for (int b = 0; b < 3; ++b)
            for (int c = 0; c < 3; ++c)
            {
                const double w = std::min({muN[a], muQ[b], muP[c]});
                if (w > 0.0)
                {
                    const int out = kTable4[a][b][c];
                    strength[out] = std::max(strength[out], w);
                }
            }
    return DefuzzLom(kP, strength, 5, 0.1, 0.9);
}

void
RoutingProtocol::ClafPhase3Expire(std::pair<Ipv4Address, uint32_t> key)
{
    auto it = m_clafPending.find(key);
    if (it == m_clafPending.end())
    {
        return;
    }
    if (it->second.copies < m_clafCounterC)
    {
        ++m_clafPhase3Sent;
        RebroadcastRreq(it->second.hdr, it->second.ttl);
    }
    else
    {
        ++m_clafPhase3Cancelled;
    }
    m_clafPending.erase(it);
}

void
RoutingProtocol::ClafDestExpire(std::pair<Ipv4Address, uint32_t> key)
{
    auto it = m_clafDest.find(key);
    if (it == m_clafDest.end() || it->second.empty())
    {
        return;
    }
    // Eq. (12) with alpha = beta = 1: PPF = PQM / HC. Ties keep the earliest copy.
    const ClafCopy* best = &it->second.front();
    double bestPpf = best->pqm / std::max<double>(1.0, best->hop);
    for (const auto& c : it->second)
    {
        const double ppf = c.pqm / std::max<double>(1.0, c.hop);
        if (ppf > bestPpf)
        {
            best = &c;
            bestPpf = ppf;
        }
    }
    RoutingTableEntry toOrigin;
    if (m_routingTable.LookupRoute(key.first, toOrigin))
    {
        toOrigin.SetNextHop(best->src);
        toOrigin.SetHop(best->hop);
        m_routingTable.Update(toOrigin);
        ++m_clafDestReplies;
        SendReply(best->hdr, toOrigin);
    }
    m_clafDest.erase(it);
}

// ---------------------------------------------------------------------------
// TAAODV (R058). Li, Li, Yu and Li, "TAAODV: TOPSIS-aided AODV routing protocol
// to diffuse mass information in the unstable and high-density UAV ad hoc
// networks", IEEE T-ITS 26(10) (2025) 17255-17268. Equation numbers are the
// paper's.

void
RoutingProtocol::TaaSample()
{
    if (m_clafMac)
    {
        Ptr<WifiMacQueue> q = m_clafMac->GetTxopQueue(AC_BE_NQOS);
        if (q)
        {
            m_taaQmax = std::max<double>(1.0, q->GetMaxSize().GetValue());
            // Eq. (19): BO = Q / Q_max.
            m_taaQueueSamples.push_back(static_cast<double>(q->GetNPackets()) / m_taaQmax);
            while (m_taaQueueSamples.size() > m_taaSamples)
            {
                m_taaQueueSamples.pop_front();
            }
        }
    }
    Simulator::Schedule(m_taaSampleInterval, &RoutingProtocol::TaaSample, this);
}

void
RoutingProtocol::TaaMyKinematics(Vector& pos, Vector& vel) const
{
    pos = Vector(0, 0, 0);
    vel = Vector(0, 0, 0);
    Ptr<Node> node = m_ipv4 ? m_ipv4->GetObject<Node>() : nullptr;
    Ptr<MobilityModel> mm = node ? node->GetObject<MobilityModel>() : nullptr;
    if (mm)
    {
        pos = mm->GetPosition();
        vel = mm->GetVelocity();
    }
}

void
RoutingProtocol::TaaOnHello(Ipv4Address from, const TaaodvExtHeader& ext)
{
    const Time now = Simulator::Now();
    TaaNbr& r = m_taaNbr[from];
    // Eq. (2) compares the lists one HELLO period apart; after a silence
    // longer than the neighbour lifetime the old list says nothing.
    r.hasPrev = r.seen && (now - r.lastHello) <= m_allowedHelloLoss * m_helloInterval;
    if (r.hasPrev)
    {
        r.prev = r.cur;
    }
    r.cur = std::set<Ipv4Address>(ext.m_addrs.begin(), ext.m_addrs.end());
    r.pos = Vector(ext.m_x, ext.m_y, 0);
    r.vel = Vector(ext.m_vx, ext.m_vy, 0);
    r.posT = now;
    r.lastHello = now;
    r.seen = true;
}

std::vector<Ipv4Address>
RoutingProtocol::TaaLiveNeighbors() const
{
    // A neighbour is current while its HELLOs keep arriving, with AODV's own
    // allowance for lost HELLOs (AllowedHelloLoss x HelloInterval).
    const Time now = Simulator::Now();
    std::vector<Ipv4Address> out;
    for (const auto& kv : m_taaNbr)
    {
        if (kv.second.seen && now - kv.second.lastHello <= m_allowedHelloLoss * m_helloInterval)
        {
            out.push_back(kv.first);
        }
    }
    return out;
}

std::vector<Ipv4Address>
RoutingProtocol::TaaSelectForwarders(Ipv4Address prevHop)
{
    const std::vector<Ipv4Address> live = TaaLiveNeighbors();
    std::vector<Ipv4Address> cand;
    for (const auto& a : live)
    {
        if (a != prevHop)
        {
            cand.push_back(a);
        }
    }
    const size_t n = cand.size();
    // Sec. IV-A: 3 or fewer neighbours -> broadcast; 4-6 -> the three most
    // stable; more than 6 -> the more stable half, rounded down.
    if (n <= 3)
    {
        ++m_taaFloodForward;
        return {};
    }
    const size_t k = (n <= 6) ? 3 : n / 2;

    // Eq. (3): N is the number of nodes within two hops of this node.
    std::set<Ipv4Address> twoHop(live.begin(), live.end());
    for (const auto& a : live)
    {
        const auto& lst = m_taaNbr.at(a).cur;
        twoHop.insert(lst.begin(), lst.end());
    }
    for (const auto& s : m_socketAddresses)
    {
        twoHop.erase(s.second.GetLocal());
    }
    const double nTwo = std::max<double>(1.0, twoHop.size());

    Vector pi;
    Vector vi;
    TaaMyKinematics(pi, vi);
    const double now = Simulator::Now().GetSeconds();
    const double R = m_taaRange;
    std::vector<std::array<double, 3>> X(n);
    for (size_t j = 0; j < n; ++j)
    {
        const TaaNbr& r = m_taaNbr.at(cand[j]);
        // 'V' model, Eq. (2): overlap of the neighbour's neighbour set across
        // one HELLO period. Taken as a benefit criterion (a larger overlap is a
        // steadier neighbourhood), the reading under which the paper's own
        // example ranks the steadier cluster first.
        double v = 0.0;
        if (r.hasPrev)
        {
            size_t inter = 0;
            for (const auto& a : r.cur)
            {
                inter += r.prev.count(a);
            }
            const size_t uni = r.cur.size() + r.prev.size() - inter;
            v = uni ? static_cast<double>(inter) / uni : 0.0;
        }
        // 'N' model, Eq. (3).
        const double nn = r.cur.size() / nTwo;
        // 'S' model, Eqs. (4)-(12): normalised distance left before the pair
        // separates to R, along their relative velocity. The advertised
        // position is advanced to now with the advertised velocity.
        const double age = now - r.posT.GetSeconds();
        const double x = (r.pos.x + r.vel.x * age) - pi.x;
        const double y = (r.pos.y + r.vel.y * age) - pi.y;
        const double rvx = r.vel.x - vi.x;
        const double rvy = r.vel.y - vi.y;
        double l;
        if (std::hypot(rvx, rvy) < 1e-9)
        {
            // No relative motion: the link does not expire while in range; 2 is
            // the largest value Eq. (12) can take.
            l = (std::hypot(x, y) <= R) ? 2.0 : 0.0;
        }
        else
        {
            const double th = std::atan2(rvy, rvx);
            const double rho = x * std::cos(th) + y * std::sin(th);
            const double perp = x * std::sin(th) - y * std::cos(th);
            const double disc = R * R - perp * perp;
            l = (disc < 0.0) ? 0.0 : std::max(0.0, (std::sqrt(disc) - rho) / R);
        }
        X[j] = {v, nn, l};
    }

    // Eqs. (14)-(16): coefficient-of-variation weights over the candidates.
    double w[3];
    double wSum = 0.0;
    for (int c = 0; c < 3; ++c)
    {
        double mean = 0.0;
        for (size_t j = 0; j < n; ++j)
        {
            mean += X[j][c];
        }
        mean /= n;
        double var = 0.0;
        for (size_t j = 0; j < n; ++j)
        {
            var += (X[j][c] - mean) * (X[j][c] - mean);
        }
        const double sd = std::sqrt(var / n);
        w[c] = (mean > 0.0) ? sd / mean : 0.0;
        wSum += w[c];
    }
    for (int c = 0; c < 3; ++c)
    {
        w[c] = (wSum > 0.0) ? w[c] / wSum : 1.0 / 3.0;
    }

    // TOPSIS, Sec. IV-A steps 2-5 (Eqs. 28-34): X divided by its Frobenius
    // norm, columns weighted, distances to the ideal and anti-ideal points
    // (all three criteria are benefits), closeness f = S0 / (S* + S0).
    double fro = 0.0;
    for (size_t j = 0; j < n; ++j)
    {
        for (int c = 0; c < 3; ++c)
        {
            fro += X[j][c] * X[j][c];
        }
    }
    fro = std::sqrt(fro);
    std::vector<double> f(n, 0.5);
    if (fro > 0.0)
    {
        std::vector<std::array<double, 3>> Y(n);
        double best[3];
        double worst[3];
        for (int c = 0; c < 3; ++c)
        {
            best[c] = -std::numeric_limits<double>::infinity();
            worst[c] = std::numeric_limits<double>::infinity();
            for (size_t j = 0; j < n; ++j)
            {
                Y[j][c] = w[c] * X[j][c] / fro;
                best[c] = std::max(best[c], Y[j][c]);
                worst[c] = std::min(worst[c], Y[j][c]);
            }
        }
        for (size_t j = 0; j < n; ++j)
        {
            double sBest = 0.0;
            double sWorst = 0.0;
            for (int c = 0; c < 3; ++c)
            {
                sBest += (Y[j][c] - best[c]) * (Y[j][c] - best[c]);
                sWorst += (Y[j][c] - worst[c]) * (Y[j][c] - worst[c]);
            }
            sBest = std::sqrt(sBest);
            sWorst = std::sqrt(sWorst);
            f[j] = (sBest + sWorst > 0.0) ? sWorst / (sBest + sWorst) : 0.5;
        }
    }
    // Rank by closeness; ties keep address order (cand is address-sorted).
    std::vector<size_t> idx(n);
    for (size_t j = 0; j < n; ++j)
    {
        idx[j] = j;
    }
    std::stable_sort(idx.begin(), idx.end(), [&f](size_t a, size_t b) { return f[a] > f[b]; });
    std::vector<Ipv4Address> out;
    for (size_t j = 0; j < k; ++j)
    {
        out.push_back(cand[idx[j]]);
    }
    ++m_taaRestrictedForward;
    m_taaForwardersNamed += k;
    // For the independent recomputation in scripts/taaodv_topsis_check.py.
    NS_LOG_DEBUG([&] {
        std::ostringstream os;
        os << "TAA-TOPSIS k=" << k << " w=" << w[0] << "," << w[1] << "," << w[2] << " |";
        for (size_t j = 0; j < n; ++j)
        {
            os << " " << cand[j] << ":" << X[j][0] << "," << X[j][1] << "," << X[j][2] << ","
               << f[j];
        }
        os << " | chosen";
        for (const auto& a : out)
        {
            os << " " << a;
        }
        return os.str();
    }());
    return out;
}

double
RoutingProtocol::TaaPheromoneDelta(Ipv4Address prevHop)
{
    // Eq. (23): Rn = Pr / Pt above the reception threshold (in watts).
    const double prDbm = ClafRssDbm(prevHop);
    if (std::isnan(prDbm))
    {
        ++m_taaRssUnknown;
        return 0.0;
    }
    const double ptDbm = m_clafPhy ? m_clafPhy->GetTxPowerStart() : 16.0;
    const double thrDbm = m_clafPhy ? m_clafPhy->GetRxSensitivity() : -101.0;
    if (prDbm <= thrDbm)
    {
        return 0.0;
    }
    const double rn = std::pow(10.0, (prDbm - ptDbm) / 10.0);
    // Eq. (18): our scenarios have no energy model, so E = 1.
    const double en = 1.0;
    // Eq. (20): mean of the last N occupancy samples. An empty queue would
    // divide by zero; it is counted as one queued packet.
    double cn = 0.0;
    for (double b : m_taaQueueSamples)
    {
        cn += b;
    }
    cn = m_taaQueueSamples.empty() ? 0.0 : cn / m_taaQueueSamples.size();
    cn = std::max(cn, 1.0 / m_taaQmax);
    // Eq. (25).
    return rn * en / cn;
}

std::string
RoutingProtocol::DumpEffectiveParameters() const
{
    // Emitted from C++ so that the recorded algorithm identity and parameters
    // are what the simulator actually ran, not what a script labelled the run.
    std::ostringstream os;
    os << "algorithm=" << AlgorithmModeName(m_algo)
       << " scr_enabled=" << (m_scrEnabled ? 1 : 0)
       << " B=" << m_scrB
       << " rho=" << ((m_algo == ALGO_NO_PROBE) ? 0.0 : m_scrRho)
       << " bucket_refill=" << m_scrBucketRefill
       << " Gmax=" << m_scrScheduler.GetGmax()
       << " lambda=" << m_scrScheduler.GetLambda()
       << " ttl_max=" << m_netDiameter
       << " node_traversal_ms=" << m_nodeTraversalTime.GetMilliSeconds()
       << " net_traversal_ms=" << m_netTraversalTime.GetMilliSeconds()
       << " rreq_rate_limit=" << m_rreqRateLimit
       << " rreq_retries=" << m_rreqRetries
       // R028: a run replayed with the pre-fix give-up test must be
       // identifiable from its own recorded parameters, not from the
       // command line that happened to launch it.
       << " rreq_cap_legacy_equality=" << (m_rreqCapLegacyEquality ? 1 : 0)
       // R029. FORK BEHAVIOUR REVISION. Bumped by every change that can alter
       // what a run produces, so a result carries its own provenance and the
       // analysis can refuse data from a superseded binary.
       //
       // This exists because provenance has now been the deciding question
       // three times (R024, R026, R028) and each time it had to be reconstructed
       // from file timestamps and which fields happened to be present. A run
       // should say what produced it.
       //
       //   1  first campaign binary
       //   2  R028: RREQ give-up test fixed, retry backoff bounded
       //   3  R029: SCR reply timeout taken from the ledger per D002
       //   4  R030: dedicated jitter stream wired up per D003
       //   5  R032: an answered request ends the reply wait (no post-reply cooldown)
       //   6  R053: CARD modes added. Every earlier mode is unchanged; this was
       //      checked by replaying stored rev-5 runs (see RESEARCH_LEDGER R053).
       //   7  R054: CARD applies the reach hint only after a topology change;
       //      CARD-NO-EXEMPT added. The other CARD variants and every earlier
       //      mode are unchanged (replayed against rev-6 and rev-5 runs).
       //   8  R057: CLAF-AODV baseline added (Safari et al., IEEE Access 2023).
       //      Every earlier mode is unchanged (replayed against rev 7 and 5).
       //   9  R058: TAAODV baseline added (Li et al., IEEE T-ITS 2025). Every
       //      earlier mode is unchanged (replayed against rev 8, 7 and 5).
       //  10  R060: CARD-CLAF composition mode (preregistered). Every earlier
       //      mode is unchanged (replayed against rev 9, 8, 7 and 5).
       << " fork_rev=" << SCR_FORK_REVISION;
    if (IsCardMode(m_algo))
    {
        // Emitted only for CARD so that the earlier modes' records differ from
        // rev 5 in the revision number alone.
        os << " card_edge_dbm=" << m_cardEdgeDbm << " card_trend_db=" << m_cardTrendDb;
    }
    if (IsClaf())
    {
        os << " claf_counter_c=" << m_clafCounterC << " claf_ewma_alpha=" << m_clafEwmaAlpha
           << " claf_dest_timer_ms=" << m_clafDestTimer.GetMilliSeconds()
           << " claf_nd_threshold=" << m_clafNdThreshold;
    }
    if (IsTaaodv())
    {
        os << " taaodv_range_m=" << m_taaRange
           << " taaodv_queue_sample_ms=" << m_taaSampleInterval.GetMilliSeconds()
           << " taaodv_queue_samples=" << m_taaSamples;
    }
    return os.str();
}

} // namespace scr
} // namespace ns3
