/*
 * SCR — C1: controlled bridge outage and churn.
 *
 * Specification section 2, C1:
 *   Sixty static nodes: two 5x5 clusters plus two five-relay bridges.
 *   Left cluster  (100+50i, 100+50j), i,j in 0..4  -> IDs 0-24 row-major
 *   Right cluster (700+50i, 100+50j), i,j in 0..4  -> IDs 25-49
 *   Bridge A (350+75i, 150), i=0..4                -> IDs 50-54
 *   Bridge B (350+75i, 250), i=0..4                -> IDs 55-59
 *   Primary log-distance PHY (NOT the fixture range model).
 *   Pair left node i with right node 25+i, i=0..7.
 *   PHY off/on events on relays model hardware unavailability while retaining
 *   node software memory - which is exactly what the SCR ledger tombstone must
 *   survive.
 *
 * Fault patterns (--fault):
 *   healthy     no fault
 *   single      bridge A off [100,120)
 *   repeated    A off [100,104.2) [108,112.2) [116,120)
 *               B off [104,108.2) [112,116.2)   -> 200 ms overlaps at four boundaries
 *   long        all ten bridge nodes off [100,160)
 *   cold        repeated bridge events, but all eight flows start at 100 s
 *               (--coldStagger gives 100 + 0.1*i instead). Tests concurrent
 *               discovery, not recovery of running service.
 *
 * Topology validation: a scheduled outage is NOT automatically a partition.
 * --validateTopology reports the measured link structure so the intended
 * topology can be demonstrated rather than assumed.
 */

#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/dsdv-module.h"
#include "ns3/dsdv-routing-protocol.h"
#include "ns3/internet-module.h"
#include "ns3/mobility-module.h"
#include "ns3/network-module.h"
#include "ns3/olsr-module.h"
#include "ns3/propagation-module.h"
#include "ns3/scr-helper.h"
#include "ns3/scr-routing-protocol.h"
#include "ns3/wifi-module.h"

#include "ns3/deadline-telemetry-app.h"
#include "ns3/service-evidence-app.h"
#include "ns3/service-receipt-app.h"

#include <cmath>
#include <fstream>
#include <iostream>
#include <map>
#include <vector>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("ScrC1Outage");

namespace
{

struct Cfg
{
    std::string algorithm = "SCR";
    std::string fault = "repeated";
    bool coldStagger = false;
    bool validateTopology = false;
    uint32_t numFlows = 8;
    uint32_t payloadBytes = 512;
    double ratePps = 20.0;
    double deadlineMs = 250.0;
    uint32_t blockSize = 10;
    double theta = 0.8;
    uint32_t kBlocks = 2;
    double freshnessS = 2.0;
    double warmupS = 20.0;
    double genEndS = 290.0;
    double simEndS = 300.0;
    // M19/R024: C1's four research questions (RQ1, RQ2, RQ5, RQ10) are ALL
    // defined on episode reconstruction, so this defaults ON here -- unlike the
    // C2 scenario, where only some questions need it.
    bool packetEvents = true;
    uint32_t rngRun = 1;
    uint32_t rngSeed = 20260905;
    std::string outPrefix = "c1";
    // CARD (R053) classifier thresholds; see scr::RoutingProtocol attributes.
    double cardEdgeDbm = -79.0;
    double cardTrendDb = 0.1;
};

Cfg g_cfg;
std::map<std::string, double> g_radioSeconds;

void
PhyStateTrace(std::string, Time, Time duration, WifiPhyState state)
{
    const char* n = "UNKNOWN";
    switch (state)
    {
    case WifiPhyState::IDLE: n = "IDLE"; break;
    case WifiPhyState::CCA_BUSY: n = "CCA_BUSY"; break;
    case WifiPhyState::TX: n = "TX"; break;
    case WifiPhyState::RX: n = "RX"; break;
    case WifiPhyState::SWITCHING: n = "SWITCHING"; break;
    case WifiPhyState::SLEEP: n = "SLEEP"; break;
    case WifiPhyState::OFF: n = "OFF"; break;
    }
    g_radioSeconds[n] += duration.GetSeconds();
}

void
SetPhy(Ptr<Node> node, bool on)
{
    // R015 / D006. The specification asks for PHY off/on to model hardware
    // unavailability while retaining node software memory (the SCR ledger
    // tombstone must survive the outage). WifiPhy::SetOffMode() expresses that
    // exactly, but in ns-3.46.1 it corrupts MAC state when it lands during a
    // frame exchange: FrameExchangeManager::Reset() clears the transmission
    // state while already-scheduled completion events still fire, and the next
    // one dereferences a null Txop. Measured on this study's own patterns:
    //
    //   healthy / single / cold  survived (no outage boundary hit an exchange)
    //   repeated                 NS_ASSERT "Attempted to dereference zero
    //                            pointer" at t=108.000288338 s, node 51
    //   long                     NS_ASSERT "m_sentFrameTo.empty()" at
    //                            t=160.002158751 s (the instant of restoration)
    //
    // This is an upstream defect in vendored ns-3, and patching vendored source
    // would destroy the provenance the whole study rests on. Unavailability is
    // therefore expressed one layer up, by taking the node's IPv4 interfaces
    // down and up.
    //
    // What this preserves: the node stops originating and forwarding IP
    // traffic, so the bridge genuinely disappears from every route; all node
    // software state -- SCR ledger, episode counters, routing table -- is
    // retained across the outage, which is the property the fixture exists to
    // test. What differs: the radio is still physically present, so neighbours
    // observe a silent-but-present node rather than an absent one. For a
    // controlled bridge outage, where the question is whether routing recovers
    // and what it costs, that distinction does not bear on the measurement.
    Ptr<Ipv4> ipv4 = node->GetObject<Ipv4>();
    if (!ipv4)
    {
        return;
    }
    for (uint32_t i = 1; i < ipv4->GetNInterfaces(); ++i) // 0 is loopback
    {
        if (on)
        {
            ipv4->SetUp(i);
        }
        else
        {
            ipv4->SetDown(i);
        }
    }
}

void
ScheduleOutage(NodeContainer nodes, uint32_t first, uint32_t last, double from, double to)
{
    for (uint32_t i = first; i <= last; ++i)
    {
        Simulator::Schedule(Seconds(from), &SetPhy, nodes.Get(i), false);
        Simulator::Schedule(Seconds(to), &SetPhy, nodes.Get(i), true);
    }
}

} // namespace

// ---------------------------------------------------------------------------
// M19 packet-event recording (R024). OBSERVER ONLY.
//
// C1 exists to measure recovery from an imposed outage. RQ1 needs episode
// histories, RQ2 needs capped T_true and recovery fraction, RQ5 needs per-flow
// recovery, RQ10 needs the censoring fraction -- every one of them comes from
// analysis/recovery.py, which takes per-packet delivery evidence as input.
// Without this file C1 answers none of its own questions.
// ---------------------------------------------------------------------------
namespace
{
struct GenEvent
{
    uint64_t session;
    uint64_t seq;
    int64_t genNs;
};

struct RxEvent
{
    uint64_t session;
    uint64_t seq;
    int64_t rxNs;
    bool ontime;
};

std::vector<GenEvent> g_genEvents;
std::vector<RxEvent> g_rxEvents;

void
OnGenerate(uint64_t session, uint64_t seq, Time when, bool /*accepted*/)
{
    g_genEvents.push_back({session, seq, when.GetNanoSeconds()});
}

void
OnRx(uint64_t session, uint64_t seq, int64_t /*genNs*/, int64_t rxNs, bool ontime)
{
    g_rxEvents.push_back({session, seq, rxNs, ontime});
}
} // namespace

// ---- CARD (R053): classifier scoring ----
// Every break a CARD node classifies is logged with the true geometry of the
// lost link at that instant, so the verdict can be scored offline against what
// actually happened. The log is written by the scenario and never read by the
// protocol.
struct CardBreakRow
{
    double tS;
    uint32_t self;
    uint32_t neighbor;
    double dbm;
    double trendDb;
    double ageS;
    uint32_t verdict;
    double distM;
    double radialMps;
};
std::vector<CardBreakRow> g_cardBreaks;
std::map<Ipv4Address, Ptr<Node>> g_cardIpNode;
std::vector<uint64_t> g_cardCounters(5, 0);

void
CardBreakLog(Ipv4Address self, Ipv4Address nb, double dbm, double trendDb, double ageS,
             uint8_t verdict)
{
    CardBreakRow r{Simulator::Now().GetSeconds(), 0, 0, dbm, trendDb, ageS, verdict,
                   std::nan(""), std::nan("")};
    auto a = g_cardIpNode.find(self);
    auto b = g_cardIpNode.find(nb);
    if (a != g_cardIpNode.end() && b != g_cardIpNode.end())
    {
        r.self = a->second->GetId();
        r.neighbor = b->second->GetId();
        Ptr<MobilityModel> ma = a->second->GetObject<MobilityModel>();
        Ptr<MobilityModel> mb = b->second->GetObject<MobilityModel>();
        if (ma && mb)
        {
            const Vector pa = ma->GetPosition();
            const Vector pb = mb->GetPosition();
            const Vector va = ma->GetVelocity();
            const Vector vb = mb->GetVelocity();
            const double dx = pb.x - pa.x;
            const double dy = pb.y - pa.y;
            const double dz = pb.z - pa.z;
            r.distM = std::sqrt(dx * dx + dy * dy + dz * dz);
            // d/dt of the separation: positive when the nodes are moving apart.
            r.radialMps = (r.distM > 0.0) ? (dx * (vb.x - va.x) + dy * (vb.y - va.y) +
                                             dz * (vb.z - va.z)) / r.distM
                                          : 0.0;
        }
    }
    g_cardBreaks.push_back(r);
}

int
main(int argc, char* argv[])
{
    CommandLine cmd(__FILE__);
    cmd.AddValue("algorithm", "Registered algorithm name", g_cfg.algorithm);
    cmd.AddValue("fault", "healthy|single|repeated|long|cold", g_cfg.fault);
    cmd.AddValue("coldStagger", "Cold fixture: stagger starts by 0.1*i s", g_cfg.coldStagger);
    cmd.AddValue("validateTopology", "Report measured link structure and exit",
                 g_cfg.validateTopology);
    cmd.AddValue("rngRun", "RngRun", g_cfg.rngRun);
    cmd.AddValue("rngSeed", "RngSeed", g_cfg.rngSeed);
    cmd.AddValue("simEnd", "Simulation end (s)", g_cfg.simEndS);
    cmd.AddValue("packetEvents", "M19: write <prefix>_packet_events.csv", g_cfg.packetEvents);
    cmd.AddValue("cardEdgeDbm", "CARD: edge-of-reception threshold (dBm)", g_cfg.cardEdgeDbm);
    cmd.AddValue("cardTrendDb", "CARD: falling-signal threshold (dB)", g_cfg.cardTrendDb);
    cmd.AddValue("outPrefix", "Output prefix", g_cfg.outPrefix);
    cmd.Parse(argc, argv);

    const bool scrFamily = (g_cfg.algorithm != "OLSR" && g_cfg.algorithm != "DSDV");
    scr::AlgorithmMode mode = scr::ALGO_SCR;
    if (scrFamily)
    {
        NS_ABORT_MSG_UNLESS(scr::ParseAlgorithmMode(g_cfg.algorithm, mode),
                            "unknown algorithm: " << g_cfg.algorithm);
    }

    RngSeedManager::SetSeed(g_cfg.rngSeed);
    RngSeedManager::SetRun(g_cfg.rngRun);
    Config::SetDefault("ns3::WifiRemoteStationManager::RtsCtsThreshold", UintegerValue(2347));

    // ---- Exact C1 positions ----
    std::vector<Vector> pos;
    for (uint32_t j = 0; j < 5; ++j)          // left cluster, row-major: IDs 0-24
    {
        for (uint32_t i = 0; i < 5; ++i)
        {
            pos.push_back(Vector(100 + 50.0 * i, 100 + 50.0 * j, 0));
        }
    }
    for (uint32_t j = 0; j < 5; ++j)          // right cluster: IDs 25-49
    {
        for (uint32_t i = 0; i < 5; ++i)
        {
            pos.push_back(Vector(700 + 50.0 * i, 100 + 50.0 * j, 0));
        }
    }
    for (uint32_t i = 0; i < 5; ++i)          // bridge A: IDs 50-54
    {
        pos.push_back(Vector(350 + 75.0 * i, 150, 0));
    }
    for (uint32_t i = 0; i < 5; ++i)          // bridge B: IDs 55-59
    {
        pos.push_back(Vector(350 + 75.0 * i, 250, 0));
    }
    NS_ABORT_MSG_UNLESS(pos.size() == 60, "C1 must have exactly 60 nodes");

    NodeContainer nodes;
    nodes.Create(pos.size());

    // ---- Primary log-distance PHY (NOT the C0 range model) ----
    YansWifiChannelHelper channel;
    channel.SetPropagationDelay("ns3::ConstantSpeedPropagationDelayModel");
    channel.AddPropagationLoss("ns3::LogDistancePropagationLossModel",
                               "Exponent", DoubleValue(2.7),
                               "ReferenceDistance", DoubleValue(1.0),
                               "ReferenceLoss", DoubleValue(40.046));
    YansWifiPhyHelper phy;
    phy.SetChannel(channel.Create());
    phy.Set("TxPowerStart", DoubleValue(16.0));
    phy.Set("TxPowerEnd", DoubleValue(16.0));
    phy.Set("RxNoiseFigure", DoubleValue(7.0));

    WifiHelper wifi;
    wifi.SetStandard(WIFI_STANDARD_80211g);
    wifi.SetRemoteStationManager("ns3::ConstantRateWifiManager",
                                 "DataMode", StringValue("ErpOfdmRate6Mbps"),
                                 "ControlMode", StringValue("ErpOfdmRate6Mbps"));
    WifiMacHelper mac;
    mac.SetType("ns3::AdhocWifiMac");
    NetDeviceContainer devices = wifi.Install(phy, mac, nodes);

    MobilityHelper mobility;
    Ptr<ListPositionAllocator> alloc = CreateObject<ListPositionAllocator>();
    for (const auto& p : pos)
    {
        alloc->Add(p);
    }
    mobility.SetPositionAllocator(alloc);
    mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    mobility.Install(nodes);

    // Fixed streams so routing randomness cannot perturb the PHY (see R011).
    wifi.AssignStreams(devices, 2000);

    // ---- Topology validation ----
    // The specification warns that a scheduled outage is not automatically a
    // partition: long direct links may survive. Report the measured structure
    // rather than assuming it.
    if (g_cfg.validateTopology)
    {
        const double range = 141.0; // measured under this PHY, see R004
        uint32_t crossDirect = 0;
        double minCross = 1e9;
        for (uint32_t a = 0; a < 25; ++a)
        {
            for (uint32_t b = 25; b < 50; ++b)
            {
                double d = CalculateDistance(pos[a], pos[b]);
                minCross = std::min(minCross, d);
                if (d <= range)
                {
                    ++crossDirect;
                }
            }
        }
        uint32_t aToB = 0;
        for (uint32_t a = 50; a < 55; ++a)
        {
            for (uint32_t b = 55; b < 60; ++b)
            {
                if (CalculateDistance(pos[a], pos[b]) <= range)
                {
                    ++aToB;
                }
            }
        }
        std::cout << "topology_validation"
                  << " min_cluster_to_cluster_m=" << minCross
                  << " direct_cross_links=" << crossDirect
                  << " bridgeA_to_bridgeB_links=" << aToB
                  << " assumed_range_m=" << range << "\n";
        std::cout << (crossDirect == 0
                          ? "VERDICT: clusters cannot communicate directly; bridges required\n"
                          : "VERDICT: DIRECT CROSS LINKS EXIST - outage is NOT a partition\n");
        return 0;
    }

    // ---- Routing ----
    InternetStackHelper internet;
    if (g_cfg.algorithm == "OLSR")
    {
        OlsrHelper olsr;
        internet.SetRoutingHelper(olsr);
    }
    else if (g_cfg.algorithm == "DSDV")
    {
        DsdvHelper dsdv;
        internet.SetRoutingHelper(dsdv);
    }
    else
    {
        ScrHelper s;
        internet.SetRoutingHelper(s);
    }
    internet.Install(nodes);

    if (g_cfg.algorithm == "OLSR")
    {
        OlsrHelper().AssignStreams(nodes, 3000);
    }
    else if (g_cfg.algorithm == "DSDV")
    {
        int64_t s = 3000;
        for (uint32_t i = 0; i < nodes.GetN(); ++i)
        {
            Ptr<dsdv::RoutingProtocol> rp = DynamicCast<dsdv::RoutingProtocol>(
                nodes.Get(i)->GetObject<Ipv4>()->GetRoutingProtocol());
            if (rp)
            {
                s += rp->AssignStreams(s);
            }
        }
    }
    else
    {
        ScrHelper().AssignStreams(nodes, 3000);
        // D003: the dedicated scheduler-jitter stream, on a base disjoint from
        // mobility (1000), PHY (2000), routing (3000), fading (4000) and
        // feedback loss (5000). It is assigned separately rather than folded
        // into AssignStreams because the helper advances its cursor by that
        // function's return value: raising it would shift every later node's
        // routing stream and change the results of EVERY algorithm, including
        // the native references D002 requires be left untouched.
        for (uint32_t i = 0; i < nodes.GetN(); ++i)
        {
            Ptr<scr::RoutingProtocol> rp = DynamicCast<scr::RoutingProtocol>(
                nodes.Get(i)->GetObject<Ipv4>()->GetRoutingProtocol());
            if (rp)
            {
                rp->AssignJitterStream(6000 + static_cast<int64_t>(i));
            }
        }
    }

    Ipv4AddressHelper address;
    address.SetBase("10.3.0.0", "255.255.0.0");
    Ipv4InterfaceContainer ifaces = address.Assign(devices);

    if (scrFamily)
    {
        Config::SetDefault("ns3::scr::RoutingProtocol::RreqRateLimit", UintegerValue(10));
        for (uint32_t i = 0; i < nodes.GetN(); ++i)
        {
            Ptr<scr::RoutingProtocol> rp = DynamicCast<scr::RoutingProtocol>(
                nodes.Get(i)->GetObject<Ipv4>()->GetRoutingProtocol());
            NS_ABORT_MSG_IF(!rp, "SCR routing protocol missing");
            rp->SetAlgorithm(mode);
        }
    }

    if (scrFamily && scr::IsClafMode(mode))
    {
        // R057: CLAF-AODV draws its forwarding decisions from its own pinned
        // stream (7000 + node), disjoint from mobility, PHY, routing, jitter.
        for (uint32_t i = 0; i < nodes.GetN(); ++i)
        {
            Ptr<scr::RoutingProtocol> rp = DynamicCast<scr::RoutingProtocol>(
                nodes.Get(i)->GetObject<Ipv4>()->GetRoutingProtocol());
            rp->AssignClafStream(7000 + static_cast<int64_t>(i));
        }
    }
    if (scrFamily && scr::IsCardMode(mode))
    {
        for (uint32_t i = 0; i < nodes.GetN(); ++i)
        {
            g_cardIpNode[ifaces.GetAddress(i)] = nodes.Get(i);
            Ptr<scr::RoutingProtocol> rp = DynamicCast<scr::RoutingProtocol>(
                nodes.Get(i)->GetObject<Ipv4>()->GetRoutingProtocol());
            rp->SetAttribute("CardEdgeDbm", DoubleValue(g_cfg.cardEdgeDbm));
            rp->SetAttribute("CardTrendDb", DoubleValue(g_cfg.cardTrendDb));
            rp->SetCardBreakLog(MakeCallback(&CardBreakLog));
        }
    }

    // ---- Flows: left i paired with right 25+i ----
    const bool cold = (g_cfg.fault == "cold");
    const uint16_t dataPort = 6000;
    const uint16_t reportPort = 6100;
    std::vector<Ptr<scr::DeadlineTelemetryApp>> sources;
    std::vector<Ptr<scr::ServiceEvidenceApp>> evidence;

    std::vector<Ptr<scr::ServiceReceiptApp>> sinks; // delivery, see below
    for (uint32_t f = 0; f < g_cfg.numFlows; ++f)
    {
        const uint32_t srcIdx = f;
        const uint32_t dstIdx = 25 + f;

        // Cold fixture: every flow starts at 100 s (or staggered by 0.1*i).
        // It tests CONCURRENT DISCOVERY, not recovery of running service.
        const double start = cold ? (100.0 + (g_cfg.coldStagger ? 0.1 * f : 0.0))
                                  : (g_cfg.warmupS + (1.0 * f) / g_cfg.numFlows);
        const double stop = cold ? (start + 3600.0 / g_cfg.ratePps) : g_cfg.genEndS;

        Ptr<scr::ServiceReceiptApp> sink = CreateObject<scr::ServiceReceiptApp>();
        sinks.push_back(sink);
        if (g_cfg.packetEvents)
        {
            sink->TraceConnectWithoutContext("Rx", MakeCallback(&OnRx));
        }
        sink->SetAttribute("Port", UintegerValue(dataPort + f));
        sink->SetAttribute("ReportPort", UintegerValue(reportPort + f));
        // R037 ablations. These two arms were registered in the specification
        // and shipped as enum values nothing read, so they reproduced SCR
        // exactly and would have entered the paper as "removing this component
        // changes nothing". Derived from the parsed mode, never from a string
        // compare on a filename.
        sink->SetAttribute("IgnoreDeadline",
                           BooleanValue(mode == scr::ALGO_NO_DEADLINE));
        nodes.Get(dstIdx)->AddApplication(sink);
        sink->SetStartTime(Seconds(std::max(1.0, start - 1.0)));
        sink->SetStopTime(Seconds(g_cfg.simEndS));

        Ptr<scr::DeadlineTelemetryApp> src = CreateObject<scr::DeadlineTelemetryApp>();
        src->SetAttribute("PeerAddress", Ipv4AddressValue(ifaces.GetAddress(dstIdx)));
        src->SetAttribute("PeerPort", UintegerValue(dataPort + f));
        src->SetAttribute("SessionId", UintegerValue(2000 + f));
        src->SetAttribute("Interval", TimeValue(Seconds(1.0 / g_cfg.ratePps)));
        src->SetAttribute("Deadline", TimeValue(MilliSeconds(g_cfg.deadlineMs)));
        src->SetAttribute("BlockSize", UintegerValue(g_cfg.blockSize));
        src->SetAttribute("PayloadBytes", UintegerValue(g_cfg.payloadBytes));
        nodes.Get(srcIdx)->AddApplication(src);
        src->SetStartTime(Seconds(start));
        src->SetStopTime(Seconds(std::min(stop, g_cfg.simEndS)));
        sources.push_back(src);
        if (g_cfg.packetEvents)
        {
            src->TraceConnectWithoutContext("Generate", MakeCallback(&OnGenerate));
        }

        if (g_cfg.algorithm != "AODV-STOCK")
        {
            Ptr<scr::ServiceEvidenceApp> ev = CreateObject<scr::ServiceEvidenceApp>();
            ev->SetAttribute("Port", UintegerValue(reportPort + f));
            nodes.Get(srcIdx)->AddApplication(ev);
            ev->SetStartTime(Seconds(std::max(1.0, start - 1.0)));
            ev->SetStopTime(Seconds(g_cfg.simEndS));
            ev->Configure(2000 + f, ifaces.GetAddress(dstIdx), Seconds(start),
                          Seconds(1.0 / g_cfg.ratePps), MilliSeconds(g_cfg.deadlineMs),
                          g_cfg.blockSize, g_cfg.theta,
                          // ONE-BLOCK ablation (R037): K = 1 instead of 2.
                          (mode == scr::ALGO_ONE_BLOCK) ? 1u : g_cfg.kBlocks,
                          Seconds(g_cfg.freshnessS));
            ev->SetHighestGeneratedSequence(4000);
            evidence.push_back(ev);
        }
    }

    // ---- Registered fault patterns ----
    // Exact intervals from the specification, recorded here verbatim so the
    // 200 ms overlaps at the four switching boundaries are reproducible.
    if (g_cfg.fault == "single")
    {
        ScheduleOutage(nodes, 50, 54, 100.0, 120.0);
    }
    else if (g_cfg.fault == "repeated" || cold)
    {
        ScheduleOutage(nodes, 50, 54, 100.0, 104.2);
        ScheduleOutage(nodes, 50, 54, 108.0, 112.2);
        ScheduleOutage(nodes, 50, 54, 116.0, 120.0);
        ScheduleOutage(nodes, 55, 59, 104.0, 108.2);
        ScheduleOutage(nodes, 55, 59, 112.0, 116.2);
    }
    else if (g_cfg.fault == "long")
    {
        ScheduleOutage(nodes, 50, 59, 100.0, 160.0);
    }
    else if (g_cfg.fault != "healthy")
    {
        NS_ABORT_MSG("unknown fault pattern: " << g_cfg.fault);
    }

    Config::Connect("/NodeList/*/DeviceList/*/$ns3::WifiNetDevice/Phy/State/State",
                    MakeCallback(&PhyStateTrace));

    Simulator::Stop(Seconds(g_cfg.simEndS));
    Simulator::Run();

    // ---- Results ----
    uint64_t generated = 0;
    bool transportError = false;
    uint64_t noRouteDrops = 0;
    for (const auto& s : sources)
    {
        generated += s->GetGeneratedCount();
        noRouteDrops += s->GetNoRouteDrops();
        transportError = transportError || s->HasTransportIntegrationError();
    }

    // C1 carried no delivery quantity either, for the same reason C2 did not:
    // the summary counted evidence and control traffic but never arrivals.
    // Without this, a fault pattern cannot be shown to have DONE anything --
    // the five patterns all completed and differed by only a few percent in
    // control bytes, which is uninterpretable on its own. RQ1, RQ2, RQ5 and
    // RQ10 are all delivery questions.
    uint64_t received = 0, duplicates = 0, late = 0, ontimeTrue = 0;
    for (const auto& k : sinks)
    {
        received += k->GetReceivedCount();
        duplicates += k->GetDuplicateCount();
        late += k->GetLateCount();
        ontimeTrue += k->GetOntimeTrue();
    }
    uint64_t accepted = 0, rejected = 0, confirms = 0;
    for (const auto& e : evidence)
    {
        accepted += e->GetAccepted();
        rejected += e->GetRejected();
        confirms += e->GetConfirmations();
    }

    uint64_t rreq = 0, admitted = 0, denied = 0, uncharged = 0;
    uint64_t rreqCapOvershot = 0, rreqBackoffClamped = 0; // R028
    uint32_t rreqMaxCnt = 0;                              // R028
    std::vector<uint64_t> denyBreakdown(7, 0);            // R032
    uint64_t relayed = 0, rrep = 0, rerr = 0, ctrlBytes = 0;
    std::string params = "algorithm=" + g_cfg.algorithm;
    if (scrFamily)
    {
        for (uint32_t i = 0; i < nodes.GetN(); ++i)
        {
            Ptr<scr::RoutingProtocol> rp = DynamicCast<scr::RoutingProtocol>(
                nodes.Get(i)->GetObject<Ipv4>()->GetRoutingProtocol());
            rreq += rp->GetTotalRreqOriginated();
            {
                const auto cc = rp->GetCardCounters();
                for (size_t k = 0; k < cc.size(); ++k) { g_cardCounters[k] += cc[k]; }
            }
            admitted += rp->ScrGetAdmitted();
            denied += rp->ScrGetDenied();
            uncharged += rp->ScrGetUnchargedRreq();
            rreqCapOvershot += rp->GetRreqCapOvershot();
            rreqBackoffClamped += rp->GetRreqBackoffClamped();
            rreqMaxCnt = std::max(rreqMaxCnt, rp->GetRreqMaxCnt());
            {
                const auto bd = rp->GetDenyBreakdown();
                for (size_t k = 0; k < bd.size(); ++k) { denyBreakdown[k] += bd[k]; }
            }
            relayed += rp->GetCtrlRreqRelayed();
            rrep += rp->GetCtrlRrepSent();
            rerr += rp->GetCtrlRerrSent();
            ctrlBytes += rp->GetCtrlBytesSent();
            if (i == 0)
            {
                params = rp->DumpEffectiveParameters();
            }
        }
    }

    // ---- M19 episode input (R024): generations joined against first arrivals.
    if (g_cfg.packetEvents)
    {
        std::map<std::pair<uint64_t, uint64_t>, const RxEvent*> rxIndex;
        for (const auto& r : g_rxEvents)
        {
            rxIndex.emplace(std::make_pair(r.session, r.seq), &r);
        }
        std::ofstream pf(g_cfg.outPrefix + "_packet_events.csv");
        pf << "flow,sequence,generation_ns,first_rx_ns,delay_ns,hops,delivered,ontime\n";
        for (const auto& g : g_genEvents)
        {
            const uint64_t flow = (g.session >= 1000) ? (g.session - 1000) : g.session;
            auto it = rxIndex.find(std::make_pair(g.session, g.seq));
            if (it == rxIndex.end())
            {
                // Never delivered. These rows ARE the outage (E10).
                pf << flow << "," << g.seq << "," << g.genNs << ",,,,0,0\n";
                continue;
            }
            const RxEvent* r = it->second;
            pf << flow << "," << g.seq << "," << g.genNs << "," << r->rxNs << ","
               << (r->rxNs - g.genNs) << ",,1," << (r->ontime ? 1 : 0) << "\n";
        }
        pf.close();
    }

    std::ofstream sf(g_cfg.outPrefix + "_summary.csv");
    sf << "key,value\n";
    sf << "scenario,C1\n";
    sf << "fault," << g_cfg.fault << "\n";
    sf << "cold_stagger," << (g_cfg.coldStagger ? 1 : 0) << "\n";
    sf << "algorithm," << g_cfg.algorithm << "\n";
    sf << "effective_parameters,\"" << params << "\"\n";
    sf << "rng_seed," << g_cfg.rngSeed << "\n";
    sf << "rng_run," << g_cfg.rngRun << "\n";
    sf << "generated," << generated << "\n";
    sf << "received," << received << "\n";
    sf << "duplicates," << duplicates << "\n";
    sf << "late," << late << "\n";
    sf << "ontime_true," << ontimeTrue << "\n";
    sf << "no_route_drops," << noRouteDrops << "\n";
    sf << "timely_pdr," << (generated ? (static_cast<double>(ontimeTrue) /
                                        static_cast<double>(generated)) : 0.0)
       << "\n";
    sf << "transport_integration_error," << (transportError ? 1 : 0) << "\n";
    sf << "reports_accepted," << accepted << "\n";
    sf << "reports_rejected," << rejected << "\n";
    sf << "service_confirmations," << confirms << "\n";
    sf << "source_rreq_originated," << rreq << "\n";
    sf << "scr_admitted," << admitted << "\n";
    sf << "scr_denied," << denied << "\n";
    sf << "scr_uncharged_rreq," << uncharged << "\n";
    // R028 health counters; see scr-manet.cc for why these are in the CSV.
    sf << "rreq_cap_overshot," << rreqCapOvershot << "\n";
    sf << "rreq_backoff_clamped," << rreqBackoffClamped << "\n";
    sf << "rreq_max_cnt," << rreqMaxCnt << "\n";
    // R032 denial breakdown (sums to scr_denied).
    sf << "deny_no_demand," << denyBreakdown[0] << "\n";
    sf << "deny_in_flight," << denyBreakdown[1] << "\n";
    sf << "deny_not_eligible," << denyBreakdown[2] << "\n";
    sf << "deny_cooldown_after_reply," << denyBreakdown[3] << "\n";
    sf << "deny_no_allowance," << denyBreakdown[4] << "\n";
    sf << "deny_no_node_token," << denyBreakdown[5] << "\n";
    sf << "deny_fifo," << denyBreakdown[6] << "\n";
    sf << "ctrl_rreq_relayed," << relayed << "\n";
    sf << "ctrl_rrep_sent," << rrep << "\n";
    sf << "ctrl_rerr_sent," << rerr << "\n";
    sf << "ctrl_bytes_sent," << ctrlBytes << "\n";
    if (scrFamily && scr::IsClafMode(mode))
    {
        std::vector<uint64_t> cl(7, 0);
        for (uint32_t i = 0; i < nodes.GetN(); ++i)
        {
            Ptr<scr::RoutingProtocol> rp = DynamicCast<scr::RoutingProtocol>(
                nodes.Get(i)->GetObject<Ipv4>()->GetRoutingProtocol());
            const auto c = rp->GetClafCounters();
            for (size_t k = 0; k < c.size(); ++k)
            {
                cl[k] += c[k];
            }
        }
        sf << "claf_sparse_forward," << cl[0] << "\n";
        sf << "claf_prob_forward," << cl[1] << "\n";
        sf << "claf_suppressed," << cl[2] << "\n";
        sf << "claf_phase3_sent," << cl[3] << "\n";
        sf << "claf_phase3_cancelled," << cl[4] << "\n";
        sf << "claf_phase1_drop," << cl[5] << "\n";
        sf << "claf_dest_replies," << cl[6] << "\n";
    }
    if (scrFamily && mode == scr::ALGO_TAAODV)
    {
        // R058: TAAODV counters, written only for TAAODV runs.
        std::vector<uint64_t> tc(8, 0);
        for (uint32_t i = 0; i < nodes.GetN(); ++i)
        {
            Ptr<scr::RoutingProtocol> rp = DynamicCast<scr::RoutingProtocol>(
                nodes.Get(i)->GetObject<Ipv4>()->GetRoutingProtocol());
            const auto c = rp->GetTaaodvCounters();
            for (size_t k = 0; k < c.size(); ++k)
            {
                tc[k] += c[k];
            }
        }
        sf << "taa_flood_forward," << tc[0] << "\n";
        sf << "taa_restricted_forward," << tc[1] << "\n";
        sf << "taa_forwarders_named," << tc[2] << "\n";
        sf << "taa_not_selected," << tc[3] << "\n";
        sf << "taa_reselect," << tc[4] << "\n";
        sf << "taa_kept," << tc[5] << "\n";
        sf << "taa_rss_unknown," << tc[6] << "\n";
        sf << "taa_late_named," << tc[7] << "\n";
    }
    if (scrFamily && scr::IsCardMode(mode))
    {
        // CARD (R053). Written only for CARD runs, so the other modes' summaries
        // are unchanged apart from the fork revision.
        sf << "card_break_topology," << g_cardCounters[0] << "\n";
        sf << "card_break_transient," << g_cardCounters[1] << "\n";
        sf << "card_break_unknown," << g_cardCounters[2] << "\n";
        sf << "card_rerr_topology_rx," << g_cardCounters[3] << "\n";
        sf << "card_exempt_admitted," << g_cardCounters[4] << "\n";
        std::ofstream cf(g_cfg.outPrefix + "_card_breaks.csv");
        cf << "t_s,self,neighbor,dbm,trend_db,age_s,verdict,dist_m,radial_mps\n";
        cf.precision(9);
        for (const auto& r : g_cardBreaks)
        {
            cf << r.tS << "," << r.self << "," << r.neighbor << "," << r.dbm << "," << r.trendDb
               << "," << r.ageS << "," << r.verdict << "," << r.distM << "," << r.radialMps
               << "\n";
        }
        cf.close();
    }
    for (const auto& kv : g_radioSeconds)
    {
        sf << "radio_seconds_" << kv.first << "," << kv.second << "\n";
    }
    sf.close();

    std::cout << params << " fault=" << g_cfg.fault << " generated=" << generated
              << " received=" << received
              << " timely_pdr=" << (generated ? (static_cast<double>(ontimeTrue) /
                                                static_cast<double>(generated)) : 0.0)
              << " accepted=" << accepted << " confirms=" << confirms
              << " source_rreq=" << rreq << " relayed=" << relayed
              << " uncharged=" << uncharged << " ctrl_bytes=" << ctrlBytes << "\n";

    Simulator::Destroy();
    return 0;
}
