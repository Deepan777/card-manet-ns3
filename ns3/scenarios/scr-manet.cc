/*
 * SCR — main experimental scenario (C2 random mobile MANET).
 *
 * Geometry is FROZEN at 60 nodes in 700 x 700 m (RESEARCH_LEDGER R007,
 * DESIGN_DEVIATIONS D005), decided from measured connectivity before any SCR
 * code existed.
 *
 * Supports every registered algorithm:
 *   SCR-family (SCR fork): SCR, RREP-RESET, TIME-BUCKET, PERSISTENT-BACKOFF,
 *                          AODV-FB, AODV-STOCK, and the five ablations
 *   Native references:     OLSR, DSDV
 *
 * The algorithm identity and every effective parameter are emitted from C++.
 * An unknown name aborts the run rather than defaulting.
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

#include <cstdio>
#include <array>
#include <cmath>
#include <fstream>
#include <iostream>
#include <map>
#include <vector>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("ScrManet");

namespace
{

struct Cfg
{
    std::string algorithm = "SCR";
    uint32_t numNodes = 60;
    double areaSide = 700.0; // FROZEN, see R007
    double speed = 5.0;
    double pause = 2.0;
    uint32_t numFlows = 8;
    uint32_t payloadBytes = 512;
    double ratePps = 20.0;
    double deadlineMs = 250.0;
    uint32_t blockSize = 10; // M
    double theta = 0.8;
    uint32_t kBlocks = 2; // K
    double freshnessS = 2.0; // F
    // ---- C3 channel and feedback stress (all default OFF) ----
    bool fading = false;         //!< Nakagami m0=m1=m2=1 on top of log-distance
    double feedbackLoss = 0.0;   //!< Bernoulli receipt drop at the source guard
    double clockOffsetMs = 0.0;  //!< receiver deadline-clock error
    // M19/R024: per-packet event log, required to reconstruct outage episodes.
    // Off by default because it costs roughly 2 MB per run and only some
    // research questions need it; the manifests that need it pass it.
    bool packetEvents = false;
    uint32_t B = 4;
    double rho = 0.2;
    double gmax = 4.0;
    double lambda = 2.0;
    double warmupS = 20.0;
    double genEndS = 290.0;
    double simEndS = 300.0;
    uint32_t rngRun = 1;
    uint32_t rngSeed = 20260905;
    std::string outPrefix = "run";
    // CARD (R053) classifier thresholds; see scr::RoutingProtocol attributes.
    double cardEdgeDbm = -79.0;
    double cardTrendDb = 0.1;
    // R055: write every trajectory sample at full precision. Observation only:
    // the samples are taken by the existing 10 s sampler whether or not this is
    // set, so it adds no event and draws no random number.
    bool trajDump = false;
};

Cfg g_cfg;

bool
IsScrFamily(const std::string& a)
{
    return a != "OLSR" && a != "DSDV";
}

/// Feedback apps are absent only for AODV-STOCK (deployment-overhead reference).
bool
UsesFeedback(const std::string& a)
{
    return a != "AODV-STOCK";
}

// ---------------------------------------------------------------------------
// M21 (E10): radio occupancy.
//
// Accumulates the time each PHY spends in each state, summed over all nodes.
// Strictly a passive observer: it reads the PHY state trace and never feeds any
// protocol decision.
//
// Why RREQ counts are not a substitute: a control packet occupies airtime in
// proportion to its transmission duration, not its count, and contention
// (CCA_BUSY) is felt by every node in range, not only the originator. Two
// algorithms with equal RREQ counts can occupy very different medium time.
// ---------------------------------------------------------------------------
std::map<std::string, double> g_radioSeconds;

void
PhyStateTrace(std::string /*context*/, Time start, Time duration, WifiPhyState state)
{
    const char* name = "UNKNOWN";
    switch (state)
    {
    case WifiPhyState::IDLE: name = "IDLE"; break;
    case WifiPhyState::CCA_BUSY: name = "CCA_BUSY"; break;
    case WifiPhyState::TX: name = "TX"; break;
    case WifiPhyState::RX: name = "RX"; break;
    case WifiPhyState::SWITCHING: name = "SWITCHING"; break;
    case WifiPhyState::SLEEP: name = "SLEEP"; break;
    case WifiPhyState::OFF: name = "OFF"; break;
    }
    g_radioSeconds[name] += duration.GetSeconds();
}

} // namespace

// ---------------------------------------------------------------------------
// M19 packet-event recording (R024). OBSERVER ONLY: nothing here is read by any
// protocol decision, and the traces fire after the decision they describe.
//
// Why this exists. Until 2026-09-10 the campaign scenarios wrote only aggregate
// summaries, so analysis/recovery.py -- which reconstructs outage episodes and
// produces capped T_true, recovery fraction and censoring fraction -- had no
// input at all. Every C1 research question (RQ1, RQ2, RQ5, RQ10) is defined on
// those quantities, so C1 could not answer any of them. scr-base-manet.cc had
// always written this file; the mechanism was simply never carried across.
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
    int64_t genNs;
    int64_t rxNs;
    bool ontime;
};

std::vector<GenEvent> g_genEvents;
std::vector<RxEvent> g_rxEvents;

void
OnGenerate(uint64_t session, uint64_t seq, Time when, bool /*accepted*/)
{
    // Recorded regardless of socket acceptance: a packet the source scheduled
    // but could not hand off is still a generation, and treating it otherwise
    // would silently shrink the denominator.
    g_genEvents.push_back({session, seq, when.GetNanoSeconds()});
}

void
OnRx(uint64_t session, uint64_t seq, int64_t genNs, int64_t rxNs, bool ontime)
{
    g_rxEvents.push_back({session, seq, genNs, rxNs, ontime});
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
    cmd.AddValue("numNodes", "Node count", g_cfg.numNodes);
    cmd.AddValue("areaSide", "Square side (m), frozen at 700", g_cfg.areaSide);
    cmd.AddValue("speed", "Node speed (m/s)", g_cfg.speed);
    cmd.AddValue("numFlows", "Disjoint flows", g_cfg.numFlows);
    cmd.AddValue("ratePps", "Packets/s per flow", g_cfg.ratePps);
    cmd.AddValue("B", "Fast allowance B", g_cfg.B);
    cmd.AddValue("rho", "Probe refill rate", g_cfg.rho);
    // C4 (scale/density) and the payload family drive these; defaults are the
    // frozen C2 values, so an invocation that omits them is byte-identical to
    // the campaign binary. Verified by exogenous-trajectory hash after adding.
    cmd.AddValue("payloadBytes", "Scientific payload per packet (bytes)", g_cfg.payloadBytes);
    cmd.AddValue("freshnessS", "Evidence freshness window F (s)", g_cfg.freshnessS);
    cmd.AddValue("blockSize", "Block size M", g_cfg.blockSize);
    cmd.AddValue("kBlocks", "Consecutive good blocks K", g_cfg.kBlocks);
    cmd.AddValue("theta", "Per-block on-time threshold theta", g_cfg.theta);
    // ---- C3 ----
    cmd.AddValue("fading", "C3: add Nakagami fading (m0=m1=m2=1)", g_cfg.fading);
    cmd.AddValue("feedbackLoss", "C3: Bernoulli receipt-drop probability", g_cfg.feedbackLoss);
    cmd.AddValue("clockOffsetMs", "C3: receiver deadline-clock error (ms)", g_cfg.clockOffsetMs);
    cmd.AddValue("packetEvents", "M19: write <prefix>_packet_events.csv", g_cfg.packetEvents);
    cmd.AddValue("simEnd", "Simulation end (s)", g_cfg.simEndS);
    cmd.AddValue("genEnd", "Generation end (s)", g_cfg.genEndS);
    cmd.AddValue("rngRun", "RngRun", g_cfg.rngRun);
    cmd.AddValue("rngSeed", "RngSeed", g_cfg.rngSeed);
    cmd.AddValue("trajDump", "R055: write <prefix>_traj.csv (full-precision positions)", g_cfg.trajDump);
    cmd.AddValue("cardEdgeDbm", "CARD: edge-of-reception threshold (dBm)", g_cfg.cardEdgeDbm);
    cmd.AddValue("cardTrendDb", "CARD: falling-signal threshold (dB)", g_cfg.cardTrendDb);
    cmd.AddValue("outPrefix", "Output prefix", g_cfg.outPrefix);
    cmd.Parse(argc, argv);

    // Validate the algorithm name before doing anything else.
    scr::AlgorithmMode mode = scr::ALGO_SCR;
    const bool scrFamily = IsScrFamily(g_cfg.algorithm);
    if (scrFamily)
    {
        NS_ABORT_MSG_UNLESS(scr::ParseAlgorithmMode(g_cfg.algorithm, mode),
                            "unknown algorithm: " << g_cfg.algorithm);
    }

    RngSeedManager::SetSeed(g_cfg.rngSeed);
    RngSeedManager::SetRun(g_cfg.rngRun);

    Config::SetDefault("ns3::WifiRemoteStationManager::RtsCtsThreshold", UintegerValue(2347));

    NodeContainer nodes;
    nodes.Create(g_cfg.numNodes);

    // ---- Registered PHY ----
    YansWifiChannelHelper channel;
    channel.SetPropagationDelay("ns3::ConstantSpeedPropagationDelayModel");
    channel.AddPropagationLoss("ns3::LogDistancePropagationLossModel",
                               "Exponent", DoubleValue(2.7),
                               "ReferenceDistance", DoubleValue(1.0),
                               "ReferenceLoss", DoubleValue(40.046));
    // C3 impairment streams, disjoint from mobility (1000), PHY (2000) and
    // routing (3000). Declared here because the fading model is constructed
    // below and must be pinned at construction.
    constexpr int64_t kFadingStream = 4000;
    constexpr int64_t kFeedbackLossStream = 5000;

    YansWifiPhyHelper phy;
    if (g_cfg.fading)
    {
        // C3: severe fading, chained AFTER log-distance. m0=m1=m2=1 is Rayleigh
        // at every distance -- the severe case the specification asks for, not
        // a realistic average.
        //
        // The channel is assembled by hand here for one reason: YansWifiChannel
        // exposes no way to retrieve its loss model, and the fading model
        // carries random variables that MUST be pinned to a disjoint stream.
        // Left unpinned they would draw from the global pool, which each
        // routing protocol depletes by a different amount, so node fading would
        // differ per algorithm and the paired comparison would be invalid --
        // exactly the defect R011 was raised to fix, reintroduced through a
        // side door. PropagationLossModel::AssignStreams walks the chain, so
        // pinning the head pins the fading model too.
        //
        // This branch is entered ONLY when fading is requested; the primary
        // condition still goes through the helper below, untouched.
        Ptr<YansWifiChannel> chan = CreateObject<YansWifiChannel>();
        chan->SetPropagationDelayModel(CreateObject<ConstantSpeedPropagationDelayModel>());

        Ptr<LogDistancePropagationLossModel> ld =
            CreateObject<LogDistancePropagationLossModel>();
        ld->SetAttribute("Exponent", DoubleValue(2.7));
        ld->SetAttribute("ReferenceDistance", DoubleValue(1.0));
        ld->SetAttribute("ReferenceLoss", DoubleValue(40.046));

        Ptr<NakagamiPropagationLossModel> nak = CreateObject<NakagamiPropagationLossModel>();
        nak->SetAttribute("m0", DoubleValue(1.0));
        nak->SetAttribute("m1", DoubleValue(1.0));
        nak->SetAttribute("m2", DoubleValue(1.0));
        ld->SetNext(nak);
        ld->AssignStreams(kFadingStream);

        chan->SetPropagationLossModel(ld);
        phy.SetChannel(chan);
    }
    else
    {
        phy.SetChannel(channel.Create());
    }
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

    // ---- Mobility (identical exogenous input for every algorithm) ----
    MobilityHelper mobility;
    Ptr<UniformRandomVariable> ux = CreateObject<UniformRandomVariable>();
    ux->SetAttribute("Min", DoubleValue(0.0));
    ux->SetAttribute("Max", DoubleValue(g_cfg.areaSide));
    Ptr<UniformRandomVariable> uy = CreateObject<UniformRandomVariable>();
    uy->SetAttribute("Min", DoubleValue(0.0));
    uy->SetAttribute("Max", DoubleValue(g_cfg.areaSide));
    Ptr<RandomRectanglePositionAllocator> alloc = CreateObject<RandomRectanglePositionAllocator>();
    alloc->SetX(ux);
    alloc->SetY(uy);
    mobility.SetPositionAllocator(alloc);
    std::ostringstream sp, pa;
    sp << "ns3::ConstantRandomVariable[Constant=" << g_cfg.speed << "]";
    pa << "ns3::ConstantRandomVariable[Constant=" << g_cfg.pause << "]";
    mobility.SetMobilityModel("ns3::RandomDirection2dMobilityModel",
                              "Bounds",
                              RectangleValue(Rectangle(0, g_cfg.areaSide, 0, g_cfg.areaSide)),
                              "Speed", StringValue(sp.str()),
                              "Pause", StringValue(pa.str()));
    mobility.Install(nodes);

    // ---- M22: FIXED RNG STREAM ASSIGNMENT ----
    //
    // Without this, every component draws from one shared global stream. Each
    // routing protocol consumes a DIFFERENT number of random values (AODV draws
    // jitter per RREQ, SCR draws different amounts, OLSR/DSDV differ again), so
    // the stream feeding mobility would be shifted by the algorithm under test.
    // Node trajectories would then differ between algorithms and the paired
    // comparison would be meaningless - the very failure M22 exists to prevent.
    //
    // Disjoint fixed ranges make mobility and PHY identical across algorithms
    // for a given seed, no matter how much randomness the routing layer uses.
    constexpr int64_t kMobilityStream = 1000;
    constexpr int64_t kWifiStream = 2000;
    constexpr int64_t kRoutingStream = 3000;
    // C3 impairments get their own disjoint ranges. Both are inert when the
    // impairment is off (no random variable is created, no stream consumed),
    // so switching them on cannot shift mobility, PHY or routing draws and the
    // primary condition stays comparable with the frozen C2 results.
    mobility.AssignStreams(nodes, kMobilityStream);
    wifi.AssignStreams(devices, kWifiStream);

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
        ScrHelper scrRouting;
        internet.SetRoutingHelper(scrRouting);
    }
    internet.Install(nodes);

    // Routing gets its own disjoint range, so however many values it draws it
    // can never reach into the mobility or PHY streams.
    if (g_cfg.algorithm == "OLSR")
    {
        OlsrHelper().AssignStreams(nodes, kRoutingStream);
    }
    else if (g_cfg.algorithm == "DSDV")
    {
        // DsdvHelper exposes no AssignStreams, but dsdv::RoutingProtocol does.
        // Assign per node so DSDV's jitter also cannot reach the mobility stream.
        int64_t s = kRoutingStream;
        for (uint32_t i = 0; i < nodes.GetN(); ++i)
        {
            Ptr<Ipv4> ipv4 = nodes.Get(i)->GetObject<Ipv4>();
            Ptr<dsdv::RoutingProtocol> rp =
                DynamicCast<dsdv::RoutingProtocol>(ipv4->GetRoutingProtocol());
            NS_ABORT_MSG_IF(!rp, "DSDV routing protocol missing");
            s += rp->AssignStreams(s);
        }
    }
    else
    {
        ScrHelper().AssignStreams(nodes, kRoutingStream);
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
    address.SetBase("10.1.0.0", "255.255.0.0");
    Ipv4InterfaceContainer ifaces = address.Assign(devices);

    if (scrFamily)
    {
        Config::SetDefault("ns3::scr::RoutingProtocol::RreqRateLimit", UintegerValue(10));
        for (uint32_t i = 0; i < nodes.GetN(); ++i)
        {
            Ptr<Ipv4> ipv4 = nodes.Get(i)->GetObject<Ipv4>();
            Ptr<scr::RoutingProtocol> rp =
                DynamicCast<scr::RoutingProtocol>(ipv4->GetRoutingProtocol());
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

    // ---- Flows: disjoint, deterministic endpoints ----
    const uint16_t dataPort = 6000;
    const uint16_t reportPort = 6001;
    std::vector<Ptr<scr::DeadlineTelemetryApp>> sources;
    std::vector<Ptr<scr::ServiceEvidenceApp>> evidenceApps;
    std::vector<Ptr<scr::ServiceReceiptApp>> sinks; // C3 clock/feedback counters

    for (uint32_t f = 0; f < g_cfg.numFlows; ++f)
    {
        const uint32_t srcIdx = f;
        const uint32_t dstIdx = g_cfg.numNodes - 1 - f;
        NS_ABORT_MSG_IF(srcIdx >= dstIdx, "flow endpoints collide");

        const Time startAt = Seconds(g_cfg.warmupS + (1.0 * f) / g_cfg.numFlows);

        // Destination: telemetry sink + receipt emitter
        Ptr<scr::ServiceReceiptApp> sink = CreateObject<scr::ServiceReceiptApp>();
        sink->SetAttribute("Port", UintegerValue(dataPort + f));
        sink->SetAttribute("ReportPort", UintegerValue(reportPort + f));
        // R037 ablations. These arms were registered in the specification and
        // shipped as enum values nothing read, so they reproduced SCR exactly
        // and would have entered the paper as "removing this component changes
        // nothing". Derived from the parsed mode, never from a filename.
        sink->SetAttribute("IgnoreDeadline",
                           BooleanValue(mode == scr::ALGO_NO_DEADLINE));
        sink->SetAttribute("ClockOffset", TimeValue(MilliSeconds(g_cfg.clockOffsetMs)));
        nodes.Get(dstIdx)->AddApplication(sink);
        sink->SetStartTime(Seconds(g_cfg.warmupS - 1.0));
        sink->SetStopTime(Seconds(g_cfg.simEndS));
        sinks.push_back(sink);
        if (g_cfg.packetEvents)
        {
            sink->TraceConnectWithoutContext("Rx", MakeCallback(&OnRx));
        }

        // Source: telemetry generator
        Ptr<scr::DeadlineTelemetryApp> src = CreateObject<scr::DeadlineTelemetryApp>();
        src->SetAttribute("PeerAddress", Ipv4AddressValue(ifaces.GetAddress(dstIdx)));
        src->SetAttribute("PeerPort", UintegerValue(dataPort + f));
        src->SetAttribute("SessionId", UintegerValue(1000 + f));
        src->SetAttribute("Interval", TimeValue(Seconds(1.0 / g_cfg.ratePps)));
        src->SetAttribute("Deadline", TimeValue(MilliSeconds(g_cfg.deadlineMs)));
        src->SetAttribute("BlockSize", UintegerValue(g_cfg.blockSize));
        src->SetAttribute("PayloadBytes", UintegerValue(g_cfg.payloadBytes));
        nodes.Get(srcIdx)->AddApplication(src);
        src->SetStartTime(startAt);
        src->SetStopTime(Seconds(g_cfg.genEndS));
        sources.push_back(src);
        if (g_cfg.packetEvents)
        {
            src->TraceConnectWithoutContext("Generate", MakeCallback(&OnGenerate));
        }

        // Source: receipt listener (absent only for AODV-STOCK)
        if (UsesFeedback(g_cfg.algorithm))
        {
            Ptr<scr::ServiceEvidenceApp> ev = CreateObject<scr::ServiceEvidenceApp>();
            ev->SetAttribute("Port", UintegerValue(reportPort + f));
            nodes.Get(srcIdx)->AddApplication(ev);
            ev->SetStartTime(Seconds(g_cfg.warmupS - 1.0));
            ev->SetStopTime(Seconds(g_cfg.simEndS));
            ev->Configure(1000 + f,
                          ifaces.GetAddress(dstIdx),
                          startAt,
                          Seconds(1.0 / g_cfg.ratePps),
                          MilliSeconds(g_cfg.deadlineMs),
                          g_cfg.blockSize,
                          g_cfg.theta,
                          // ONE-BLOCK ablation (R037): K = 1 instead of 2.
                          (mode == scr::ALGO_ONE_BLOCK) ? 1u : g_cfg.kBlocks,
                          Seconds(g_cfg.freshnessS));
            ev->SetAttribute("FeedbackLoss", DoubleValue(g_cfg.feedbackLoss));
            // One stream per flow, disjoint from mobility/PHY/routing. Inert
            // at loss=0: AssignStreams creates nothing and consumes nothing.
            ev->AssignStreams(kFeedbackLossStream + static_cast<int64_t>(f));
            ev->SetHighestGeneratedSequence(
                static_cast<uint64_t>(g_cfg.ratePps * (g_cfg.genEndS - g_cfg.warmupS)) + 1);
            evidenceApps.push_back(ev);
        }
    }

    // ---- M22 evidence: hash the exogenous trajectory ----
    // Sampled by an observer that reads only positions. If two algorithms with
    // the same seed produce different hashes, the exogenous inputs diverged and
    // the pairing is invalid - so this is checked, not assumed.
    static uint64_t g_trajHash = 1469598103934665603ull; // FNV-1a offset basis
    static std::vector<std::array<double, 5>> g_trajRows; // t, node, x, y, z (R055)
    struct TrajSampler
    {
        static void Sample(NodeContainer n)
        {
            for (uint32_t i = 0; i < n.GetN(); ++i)
            {
                Vector v = n.Get(i)->GetObject<MobilityModel>()->GetPosition();
                if (g_cfg.trajDump)
                {
                    g_trajRows.push_back({Simulator::Now().GetSeconds(), static_cast<double>(i),
                                          v.x, v.y, v.z});
                }
                // Quantise to 1 mm so floating-point noise cannot mask a match.
                int64_t q[3] = {static_cast<int64_t>(v.x * 1000),
                                static_cast<int64_t>(v.y * 1000),
                                static_cast<int64_t>(v.z * 1000)};
                for (int64_t c : q)
                {
                    for (int b = 0; b < 8; ++b)
                    {
                        g_trajHash ^= static_cast<uint8_t>((c >> (b * 8)) & 0xFF);
                        g_trajHash *= 1099511628211ull; // FNV-1a prime
                    }
                }
            }
            // R014: emit simulated-time progress so a STALLED run is
            // distinguishable from a merely slow one. A zero-delay reschedule
            // livelock froze the clock at t=128.575 s and, because nothing was
            // printed until termination, it was mistaken for a slow seed for
            // over two hours.
            //
            // This piggy-backs on the existing 10 s sampler rather than
            // scheduling its own event: adding events would change
            // same-timestamp tie ordering and could perturb results.
            std::fprintf(stderr, "PROGRESS sim_t=%.1f\n", Simulator::Now().GetSeconds());
            std::fflush(stderr);

            Simulator::Schedule(Seconds(10.0), &TrajSampler::Sample, n);
        }
    };
    Simulator::Schedule(Seconds(0.0), &TrajSampler::Sample, nodes);

    // M21: passive radio-occupancy observation across every node PHY.
    Config::Connect("/NodeList/*/DeviceList/*/$ns3::WifiNetDevice/Phy/State/State",
                    MakeCallback(&PhyStateTrace));

    Simulator::Stop(Seconds(g_cfg.simEndS));
    Simulator::Run();

    // ---- Aggregate results ----
    uint64_t generated = 0, sendErrors = 0;
    bool transportError = false;
    uint64_t noRouteDrops = 0; // R017: routing loss, not a harness fault
    for (const auto& s : sources)
    {
        generated += s->GetGeneratedCount();
        noRouteDrops += s->GetNoRouteDrops();
        sendErrors += s->GetSendErrorCount();
        transportError = transportError || s->HasTransportIntegrationError();
    }

    uint64_t accepted = 0, rejected = 0, confirmations = 0;
    for (const auto& e : evidenceApps)
    {
        accepted += e->GetAccepted();
        rejected += e->GetRejected();
        confirmations += e->GetConfirmations();
    }

    uint64_t totalRreq = 0, admitted = 0, denied = 0, uncharged = 0, handoffFailed = 0;
    uint64_t zeroDelayClamped = 0; // R014 liveness clamp; must stay small
    uint64_t rreqCapOvershot = 0;    // R028: must be 0 unless legacy mode is set
    uint64_t rreqBackoffClamped = 0; // R028: must be 0 in a healthy run
    uint32_t rreqMaxCnt = 0;         // R028: largest RREQ retry count observed
    std::vector<uint64_t> denyBreakdown(7, 0); // R032
    // C3 observables. Zero in the primary condition by construction, which is
    // itself the check that the impairments are genuinely off by default.
    uint64_t feedbackDropped = 0;
    uint64_t ontimeObserved = 0;
    uint64_t ontimeTrue = 0;
    uint64_t clockDisagreements = 0;
    // PRIMARY ENDPOINT. Until 2026-09-09 no delivery quantity was written to
    // the run summary at all: the summary carried generated, control counters
    // and radio seconds, but nothing about arrivals. timely_pdr is the study's
    // primary endpoint and the quantity the registered noninferiority margin
    // (-0.02) and the n-selection rule are both defined on, so every run
    // produced before this point can support cost comparisons only.
    uint64_t received = 0;
    uint64_t duplicates = 0;
    uint64_t late = 0;
    for (const auto& k : sinks)
    {
        ontimeObserved += k->GetOntimeObserved();
        ontimeTrue += k->GetOntimeTrue();
        clockDisagreements += k->GetClockDisagreements();
        received += k->GetReceivedCount();
        duplicates += k->GetDuplicateCount();
        late += k->GetLateCount();
    }
    for (const auto& e : evidenceApps)
    {
        feedbackDropped += e->GetFeedbackDropped();
    }
    uint64_t ctrlRreqRelayed = 0, ctrlRrepSent = 0, ctrlRerrSent = 0, ctrlBytes = 0;
    std::string paramDump = "algorithm=" + g_cfg.algorithm + " scr_family=0";
    if (scrFamily)
    {
        for (uint32_t i = 0; i < nodes.GetN(); ++i)
        {
            Ptr<Ipv4> ipv4 = nodes.Get(i)->GetObject<Ipv4>();
            Ptr<scr::RoutingProtocol> rp =
                DynamicCast<scr::RoutingProtocol>(ipv4->GetRoutingProtocol());
            totalRreq += rp->GetTotalRreqOriginated();
            {
                const auto cc = rp->GetCardCounters();
                for (size_t k = 0; k < cc.size(); ++k) { g_cardCounters[k] += cc[k]; }
            }
            ctrlRreqRelayed += rp->GetCtrlRreqRelayed();
            ctrlRrepSent += rp->GetCtrlRrepSent();
            ctrlRerrSent += rp->GetCtrlRerrSent();
            ctrlBytes += rp->GetCtrlBytesSent();
            admitted += rp->ScrGetAdmitted();
            denied += rp->ScrGetDenied();
            uncharged += rp->ScrGetUnchargedRreq();
            handoffFailed += rp->ScrGetHandoffFailed();
            zeroDelayClamped += rp->ScrGetZeroDelayClamped();
            rreqCapOvershot += rp->GetRreqCapOvershot();
            rreqBackoffClamped += rp->GetRreqBackoffClamped();
            rreqMaxCnt = std::max(rreqMaxCnt, rp->GetRreqMaxCnt());
            {
                const auto bd = rp->GetDenyBreakdown();
                for (size_t k = 0; k < bd.size(); ++k) { denyBreakdown[k] += bd[k]; }
            }
            if (i == 0)
            {
                paramDump = rp->DumpEffectiveParameters();
            }
        }
    }

    // ---- M19: join generations against first arrivals and emit the episode
    // input (R024). The join is offline and order-independent, so a duplicate
    // arrival can never overwrite a first arrival (E1).
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
            // Session ids are assigned 1000+flow at construction; recover the
            // flow index so the file is readable without a lookup table.
            const uint64_t flow = (g.session >= 1000) ? (g.session - 1000) : g.session;
            auto it = rxIndex.find(std::make_pair(g.session, g.seq));
            if (it == rxIndex.end())
            {
                // Generated but never delivered. Preserved rather than dropped:
                // an outage is defined by these rows (E10).
                pf << flow << "," << g.seq << "," << g.genNs << ",,,,0,0\n";
                continue;
            }
            const RxEvent* r = it->second;
            pf << flow << "," << g.seq << "," << g.genNs << "," << r->rxNs << ","
               << (r->rxNs - g.genNs) << ",," << 1 << "," << (r->ontime ? 1 : 0)
               << "\n";
        }
        pf.close();
    }

    if (g_cfg.trajDump)
    {
        std::ofstream tf(g_cfg.outPrefix + "_traj.csv");
        tf << "t_s,node,x,y,z\n";
        tf.precision(17);
        for (const auto& r : g_trajRows)
        {
            tf << r[0] << "," << static_cast<int>(r[1]) << "," << r[2] << "," << r[3] << ","
               << r[4] << "\n";
        }
        tf.close();
    }

    std::ofstream sf(g_cfg.outPrefix + "_summary.csv");
    sf << "key,value\n";
    sf << "algorithm," << g_cfg.algorithm << "\n";
    sf << "effective_parameters,\"" << paramDump << "\"\n";
    sf << "num_nodes," << g_cfg.numNodes << "\n";
    sf << "area_side_m," << g_cfg.areaSide << "\n";
    sf << "speed_mps," << g_cfg.speed << "\n";
    sf << "num_flows," << g_cfg.numFlows << "\n";
    sf << "rate_pps," << g_cfg.ratePps << "\n";
    sf << "payload_bytes," << g_cfg.payloadBytes << "\n";
    sf << "freshness_s," << g_cfg.freshnessS << "\n";
    sf << "block_size_m," << g_cfg.blockSize << "\n";
    sf << "k_blocks," << g_cfg.kBlocks << "\n";
    sf << "theta," << g_cfg.theta << "\n";
    sf << "c3_fading," << (g_cfg.fading ? 1 : 0) << "\n";
    sf << "c3_feedback_loss," << g_cfg.feedbackLoss << "\n";
    sf << "c3_clock_offset_ms," << g_cfg.clockOffsetMs << "\n";
    sf << "c3_feedback_dropped," << feedbackDropped << "\n";
    sf << "ontime_observed," << ontimeObserved << "\n";
    sf << "ontime_true," << ontimeTrue << "\n";
    sf << "clock_disagreements," << clockDisagreements << "\n";
    sf << "received," << received << "\n";
    sf << "duplicates," << duplicates << "\n";
    sf << "late," << late << "\n";
    sf << "timely_pdr," << (generated ? (static_cast<double>(ontimeTrue) /
                                        static_cast<double>(generated))
                                     : 0.0) << "\n";
    sf << "rng_seed," << g_cfg.rngSeed << "\n";
    sf << "rng_run," << g_cfg.rngRun << "\n";
    sf << "exogenous_trajectory_hash," << g_trajHash << "\n";
    sf << "generated," << generated << "\n";
    sf << "send_errors," << sendErrors << "\n";
    sf << "transport_integration_error," << (transportError ? 1 : 0) << "\n";
    sf << "no_route_drops," << noRouteDrops << "\n";
    sf << "reports_accepted," << accepted << "\n";
    sf << "reports_rejected," << rejected << "\n";
    sf << "service_confirmations," << confirmations << "\n";
    sf << "source_rreq_originated," << totalRreq << "\n";
    sf << "scr_admitted," << admitted << "\n";
    sf << "scr_denied," << denied << "\n";
    sf << "scr_uncharged_rreq," << uncharged << "\n";
    sf << "scr_handoff_failed," << handoffFailed << "\n";
    // R014/R028 health counters. These were previously printed to stdout
    // only, which meant the analysis could not see them; a defect that only
    // shows up in a log nobody parses is a defect nobody finds.
    sf << "scr_zero_delay_clamped," << zeroDelayClamped << "\n";
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
    sf << "ctrl_rreq_relayed," << ctrlRreqRelayed << "\n";
    sf << "ctrl_rrep_sent," << ctrlRrepSent << "\n";
    sf << "ctrl_rerr_sent," << ctrlRerrSent << "\n";
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

    std::cout << paramDump << " generated=" << generated << " reports_accepted=" << accepted
              << " reports_rejected=" << rejected << " confirmations=" << confirmations
              << " source_rreq=" << totalRreq << " admitted=" << admitted
              << " denied=" << denied << " uncharged=" << uncharged
              << " zero_delay_clamped=" << zeroDelayClamped
              << " rreq_cap_overshot=" << rreqCapOvershot
              << " rreq_backoff_clamped=" << rreqBackoffClamped
              << " rreq_max_cnt=" << rreqMaxCnt
              << " exo_hash=" << g_trajHash
              << " transport_error=" << (transportError ? 1 : 0) << "\n";

    Simulator::Destroy();
    return 0;
}
