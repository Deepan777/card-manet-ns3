/*
 * SCR — Stage 2 base MANET scenario (native AODV only, no SCR logic)
 *
 * Specification Stage 2:
 *   "First implement the minimal telemetry scenario with pinned native AODV, actual
 *    mobility, UDP and trace logging. Demonstrate that nodes send/receive, chosen
 *    endpoints are nontrivial, seeds change exogenous trajectories, and identical paired
 *    configurations reproduce them."
 *   "Use generation/first-reception joins to validate packet counts. ... Preserve packets
 *    generated but never delivered."
 *
 * Scope of THIS program:
 *   - C2 random mobile MANET geometry (specification EXPERIMENTAL_PLAN section 2, C2)
 *   - Native ns-3 AODV. No SCR, no feedback, no ledger. Those arrive in Stages 3-4.
 *   - Per-packet generation and first-reception rows, joined offline by (session,seq).
 *   - Hop count measured from the delivered IPv4 TTL, not assumed.
 *   - Route availability sampled periodically per registered pair.
 *
 * Deliberately minimal: uses ns-3's built-in SeqTsHeader for sequence + generation time.
 * The specified 56-byte TelemetryHeader is a Stage 4 component and is NOT used here, so
 * this program must not be cited for wire-cost measurements.
 */

#include "ns3/aodv-module.h"
#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/internet-module.h"
#include "ns3/mobility-module.h"
#include "ns3/network-module.h"
#include "ns3/propagation-module.h"
#include "ns3/wifi-module.h"

#include <fstream>
#include <map>
#include <set>
#include <vector>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("ScrBaseManet");

namespace
{

// ---------------------------------------------------------------------------
// Registered scenario parameters (specification section 7 / EXPERIMENTAL_PLAN C2)
// ---------------------------------------------------------------------------
struct Config1
{
    uint32_t numNodes = 60;
    double areaSide = 1000.0;      // m
    double speed = 5.0;            // m/s nominal
    double pause = 2.0;            // s
    uint32_t numFlows = 8;         // disjoint source-destination pairs
    uint32_t payloadBytes = 512;   // application payload
    double ratePps = 20.0;         // packets/s per flow
    double warmupS = 20.0;         // protocol warm-up
    double genEndS = 290.0;        // generation ends
    double simEndS = 300.0;        // simulation ends
    double deadlineMs = 250.0;     // D
    uint32_t rngRun = 1;
    uint32_t rngSeed = 20260905;
    double txPowerDbm = 16.0;
    double routeSampleS = 1.0;     // route-availability sampling interval
    std::string outPrefix = "stage2";
};

Config1 g_cfg;

// ---------------------------------------------------------------------------
// Event recording. Observer only: nothing here feeds any protocol decision.
// ---------------------------------------------------------------------------
struct GenRow
{
    uint32_t flow;
    uint64_t seq;
    int64_t genNs;
};

struct RxRow
{
    uint32_t flow;
    uint64_t seq;
    int64_t genNs;
    int64_t rxNs;
    uint32_t hops;
};

std::vector<GenRow> g_gen;
std::vector<RxRow> g_rx;
// First arrival only; duplicates never overwrite (specification E1).
std::set<std::pair<uint32_t, uint64_t>> g_seenFirst;

// Route-availability samples: flow -> (samples, samplesWithRoute, hopSum, hopSamples)
struct RouteStat
{
    uint64_t samples = 0;
    uint64_t withRoute = 0;
};

std::vector<RouteStat> g_routeStat;

// Hop count of the packet currently being delivered, extracted from the IPv4 TTL by the
// LocalDeliver trace immediately before the socket read callback runs.
uint32_t g_pendingHops = 0;
bool g_pendingHopsValid = false;

constexpr uint8_t kInitialTtl = 64;

void
LocalDeliverTrace(const Ipv4Header& header, Ptr<const Packet> packet, uint32_t /*iface*/)
{
    // hops traversed = initial TTL - TTL observed at final delivery
    uint8_t ttl = header.GetTtl();
    if (ttl <= kInitialTtl)
    {
        g_pendingHops = static_cast<uint32_t>(kInitialTtl - ttl);
        g_pendingHopsValid = true;
    }
}

// ---------------------------------------------------------------------------
// Minimal telemetry source: periodic UDP with SeqTsHeader (sequence + generation time).
// ---------------------------------------------------------------------------
class TelemetrySource : public Application
{
  public:
    static TypeId GetTypeId()
    {
        static TypeId tid = TypeId("ScrTelemetrySource")
                                .SetParent<Application>()
                                .SetGroupName("Scr")
                                .AddConstructor<TelemetrySource>();
        return tid;
    }

    void Setup(Address peer, uint32_t flow)
    {
        m_peer = peer;
        m_flow = flow;
    }

  private:
    void StartApplication() override
    {
        m_socket = Socket::CreateSocket(GetNode(), UdpSocketFactory::GetTypeId());
        m_socket->Connect(m_peer);
        m_running = true;
        Send();
    }

    void StopApplication() override
    {
        m_running = false;
        if (m_sendEvent.IsPending())
        {
            Simulator::Cancel(m_sendEvent);
        }
        if (m_socket)
        {
            m_socket->Close();
        }
    }

    void Send()
    {
        if (!m_running)
        {
            return;
        }
        SeqTsHeader hdr;
        hdr.SetSeq(static_cast<uint32_t>(m_seq));
        uint32_t pad = (g_cfg.payloadBytes > hdr.GetSerializedSize())
                           ? g_cfg.payloadBytes - hdr.GetSerializedSize()
                           : 1;
        Ptr<Packet> p = Create<Packet>(pad);
        p->AddHeader(hdr);

        // Record generation unconditionally, before the socket call. A packet that the
        // socket refuses is still a generated packet (specification E1/E10).
        g_gen.push_back({m_flow, m_seq, Simulator::Now().GetNanoSeconds()});
        m_socket->Send(p);
        ++m_seq;

        m_sendEvent = Simulator::Schedule(Seconds(1.0 / g_cfg.ratePps), &TelemetrySource::Send, this);
    }

    Ptr<Socket> m_socket;
    Address m_peer;
    EventId m_sendEvent;
    uint64_t m_seq = 0;
    uint32_t m_flow = 0;
    bool m_running = false;
};

// ---------------------------------------------------------------------------
// Minimal telemetry sink: records first arrival per (flow, sequence).
// ---------------------------------------------------------------------------
class TelemetrySink : public Application
{
  public:
    static TypeId GetTypeId()
    {
        static TypeId tid = TypeId("ScrTelemetrySink")
                                .SetParent<Application>()
                                .SetGroupName("Scr")
                                .AddConstructor<TelemetrySink>();
        return tid;
    }

    void Setup(uint16_t port, uint32_t flow)
    {
        m_port = port;
        m_flow = flow;
    }

  private:
    void StartApplication() override
    {
        m_socket = Socket::CreateSocket(GetNode(), UdpSocketFactory::GetTypeId());
        m_socket->Bind(InetSocketAddress(Ipv4Address::GetAny(), m_port));
        m_socket->SetRecvCallback(MakeCallback(&TelemetrySink::HandleRead, this));
    }

    void StopApplication() override
    {
        if (m_socket)
        {
            m_socket->Close();
        }
    }

    void HandleRead(Ptr<Socket> socket)
    {
        Ptr<Packet> p;
        Address from;
        while ((p = socket->RecvFrom(from)))
        {
            SeqTsHeader hdr;
            if (p->GetSize() < hdr.GetSerializedSize())
            {
                continue;
            }
            p->RemoveHeader(hdr);
            uint64_t seq = hdr.GetSeq();
            auto key = std::make_pair(m_flow, seq);
            if (g_seenFirst.count(key))
            {
                continue; // duplicate arrival never overwrites first (E1)
            }
            g_seenFirst.insert(key);

            uint32_t hops = g_pendingHopsValid ? g_pendingHops : 0;
            g_pendingHopsValid = false;

            g_rx.push_back({m_flow,
                            seq,
                            hdr.GetTs().GetNanoSeconds(),
                            Simulator::Now().GetNanoSeconds(),
                            hops});
        }
    }

    Ptr<Socket> m_socket;
    uint16_t m_port = 0;
    uint32_t m_flow = 0;
};

// ---------------------------------------------------------------------------
// Periodic route-availability sampler. Read-only inspection of the source's own routing
// table via the standard Ipv4RoutingProtocol lookup interface.
// ---------------------------------------------------------------------------
std::vector<Ptr<Node>> g_srcNodes;
std::vector<Ipv4Address> g_dstAddrs;

// IMPORTANT: this must be strictly read-only with respect to the protocol.
//
// An earlier version called Ipv4RoutingProtocol::RouteOutput() to test for a route. That
// is wrong twice over. With a null packet, aodv::RoutingProtocol::RouteOutput returns
// LoopbackRoute() unconditionally (aodv-routing-protocol.cc:414), so the probe reported
// "route available" 100% of the time regardless of reality. With a non-null packet it
// would instead have taken the DeferredRouteOutput path and injected real RREQs, letting
// the observer perturb the protocol it is measuring.
//
// PrintRoutingTable is used instead: it is a const, read-only interface that also purges
// expired entries before printing, and it cannot originate a discovery.
void
SampleRoutes()
{
    for (uint32_t f = 0; f < g_srcNodes.size(); ++f)
    {
        Ptr<Ipv4> ipv4 = g_srcNodes[f]->GetObject<Ipv4>();
        Ptr<Ipv4RoutingProtocol> rp = ipv4->GetRoutingProtocol();

        std::ostringstream oss;
        Ptr<OutputStreamWrapper> wrap = Create<OutputStreamWrapper>(&oss);
        rp->PrintRoutingTable(wrap, Time::S);

        std::ostringstream dstOss;
        dstOss << g_dstAddrs[f];
        const std::string dstStr = dstOss.str();

        bool haveValidRoute = false;
        std::istringstream lines(oss.str());
        std::string line;
        while (std::getline(lines, line))
        {
            // Entry rows begin with the destination address in a 16-wide left-aligned
            // field. Require the row to start with the exact address and to be flagged UP.
            if (line.rfind(dstStr, 0) == 0 && line.find(" UP ") != std::string::npos)
            {
                haveValidRoute = true;
                break;
            }
        }

        g_routeStat[f].samples++;
        if (haveValidRoute)
        {
            g_routeStat[f].withRoute++;
        }
    }
    Simulator::Schedule(Seconds(g_cfg.routeSampleS), &SampleRoutes);
}

} // namespace

int
main(int argc, char* argv[])
{
    CommandLine cmd(__FILE__);
    cmd.AddValue("numNodes", "Number of nodes", g_cfg.numNodes);
    cmd.AddValue("areaSide", "Square side in metres", g_cfg.areaSide);
    cmd.AddValue("speed", "Node speed m/s", g_cfg.speed);
    cmd.AddValue("numFlows", "Number of disjoint flows", g_cfg.numFlows);
    cmd.AddValue("ratePps", "Packets per second per flow", g_cfg.ratePps);
    cmd.AddValue("payloadBytes", "Application payload bytes", g_cfg.payloadBytes);
    cmd.AddValue("simEnd", "Simulation end time (s)", g_cfg.simEndS);
    cmd.AddValue("genEnd", "Generation end time (s)", g_cfg.genEndS);
    cmd.AddValue("rngRun", "ns-3 RngRun", g_cfg.rngRun);
    cmd.AddValue("rngSeed", "ns-3 RngSeed", g_cfg.rngSeed);
    cmd.AddValue("outPrefix", "Output file prefix", g_cfg.outPrefix);
    cmd.Parse(argc, argv);

    RngSeedManager::SetSeed(g_cfg.rngSeed);
    RngSeedManager::SetRun(g_cfg.rngRun);

    Config::SetDefault("ns3::WifiRemoteStationManager::RtsCtsThreshold", UintegerValue(2347));
    // Pin the AODV origination safety cap in every AODV-family condition (specification E6).
    Config::SetDefault("ns3::aodv::RoutingProtocol::RreqRateLimit", UintegerValue(10));

    NodeContainer nodes;
    nodes.Create(g_cfg.numNodes);

    // ---- Registered PHY ----
    YansWifiChannelHelper channel;
    channel.SetPropagationDelay("ns3::ConstantSpeedPropagationDelayModel");
    channel.AddPropagationLoss("ns3::LogDistancePropagationLossModel",
                               "Exponent",
                               DoubleValue(2.7),
                               "ReferenceDistance",
                               DoubleValue(1.0),
                               "ReferenceLoss",
                               DoubleValue(40.046));

    YansWifiPhyHelper phy;
    phy.SetChannel(channel.Create());
    phy.Set("TxPowerStart", DoubleValue(g_cfg.txPowerDbm));
    phy.Set("TxPowerEnd", DoubleValue(g_cfg.txPowerDbm));
    phy.Set("RxNoiseFigure", DoubleValue(7.0));

    WifiHelper wifi;
    wifi.SetStandard(WIFI_STANDARD_80211g);
    wifi.SetRemoteStationManager("ns3::ConstantRateWifiManager",
                                 "DataMode",
                                 StringValue("ErpOfdmRate6Mbps"),
                                 "ControlMode",
                                 StringValue("ErpOfdmRate6Mbps"));
    WifiMacHelper mac;
    mac.SetType("ns3::AdhocWifiMac");
    NetDeviceContainer devices = wifi.Install(phy, mac, nodes);

    // ---- Mobility: RandomDirection2d, uniform initial placement ----
    MobilityHelper mobility;
    Ptr<UniformRandomVariable> ux = CreateObject<UniformRandomVariable>();
    ux->SetAttribute("Min", DoubleValue(0.0));
    ux->SetAttribute("Max", DoubleValue(g_cfg.areaSide));
    Ptr<UniformRandomVariable> uy = CreateObject<UniformRandomVariable>();
    uy->SetAttribute("Min", DoubleValue(0.0));
    uy->SetAttribute("Max", DoubleValue(g_cfg.areaSide));
    Ptr<RandomRectanglePositionAllocator> alloc =
        CreateObject<RandomRectanglePositionAllocator>();
    alloc->SetX(ux);
    alloc->SetY(uy);
    mobility.SetPositionAllocator(alloc);

    std::ostringstream sspeed;
    sspeed << "ns3::ConstantRandomVariable[Constant=" << g_cfg.speed << "]";
    std::ostringstream spause;
    spause << "ns3::ConstantRandomVariable[Constant=" << g_cfg.pause << "]";
    mobility.SetMobilityModel("ns3::RandomDirection2dMobilityModel",
                              "Bounds",
                              RectangleValue(Rectangle(0, g_cfg.areaSide, 0, g_cfg.areaSide)),
                              "Speed",
                              StringValue(sspeed.str()),
                              "Pause",
                              StringValue(spause.str()));
    mobility.Install(nodes);

    // ---- Native AODV ----
    AodvHelper aodv;
    InternetStackHelper internet;
    internet.SetRoutingHelper(aodv);
    internet.Install(nodes);

    Ipv4AddressHelper address;
    address.SetBase("10.1.0.0", "255.255.0.0");
    Ipv4InterfaceContainer interfaces = address.Assign(devices);

    // ---- Flows: disjoint pairs, no endpoint reused (EXPERIMENTAL_PLAN C2) ----
    // Deterministic assignment from node index so that all algorithms receive identical
    // exogenous inputs regardless of how they consume random numbers.
    g_routeStat.resize(g_cfg.numFlows);
    uint16_t basePort = 5000;
    for (uint32_t f = 0; f < g_cfg.numFlows; ++f)
    {
        uint32_t srcIdx = f;
        uint32_t dstIdx = g_cfg.numNodes - 1 - f;
        NS_ABORT_MSG_IF(srcIdx >= dstIdx, "flow endpoints collide; reduce numFlows");

        Ptr<TelemetrySink> sink = CreateObject<TelemetrySink>();
        sink->Setup(basePort + f, f);
        nodes.Get(dstIdx)->AddApplication(sink);
        sink->SetStartTime(Seconds(g_cfg.warmupS - 1.0));
        sink->SetStopTime(Seconds(g_cfg.simEndS));

        Ptr<TelemetrySource> src = CreateObject<TelemetrySource>();
        src->Setup(InetSocketAddress(interfaces.GetAddress(dstIdx), basePort + f), f);
        nodes.Get(srcIdx)->AddApplication(src);
        // Source start offsets in [20,21] s (specification section 7)
        src->SetStartTime(Seconds(g_cfg.warmupS + (1.0 * f) / g_cfg.numFlows));
        src->SetStopTime(Seconds(g_cfg.genEndS));

        g_srcNodes.push_back(nodes.Get(srcIdx));
        g_dstAddrs.push_back(interfaces.GetAddress(dstIdx));
    }

    Config::ConnectWithoutContext("/NodeList/*/$ns3::Ipv4L3Protocol/LocalDeliver",
                                  MakeCallback(&LocalDeliverTrace));

    Simulator::Schedule(Seconds(g_cfg.warmupS), &SampleRoutes);
    Simulator::Stop(Seconds(g_cfg.simEndS));
    Simulator::Run();

    // ---- Offline join: generation x first reception ----
    std::map<std::pair<uint32_t, uint64_t>, const RxRow*> rxIndex;
    for (const auto& r : g_rx)
    {
        rxIndex[{r.flow, r.seq}] = &r;
    }

    const int64_t deadlineNs = static_cast<int64_t>(g_cfg.deadlineMs * 1e6);

    std::string packetsPath = g_cfg.outPrefix + "_packet_events.csv";
    std::ofstream pf(packetsPath);
    pf << "flow,sequence,generation_ns,first_rx_ns,delay_ns,hops,delivered,ontime\n";

    uint64_t generated = 0, delivered = 0, ontime = 0;
    uint64_t hopSum = 0, hopN = 0;
    std::map<uint32_t, uint64_t> hopHist;

    for (const auto& g : g_gen)
    {
        ++generated;
        auto it = rxIndex.find({g.flow, g.seq});
        if (it == rxIndex.end())
        {
            // Preserved: generated but never delivered (specification E10)
            pf << g.flow << "," << g.seq << "," << g.genNs << ",,,,0,0\n";
            continue;
        }
        const RxRow* r = it->second;
        int64_t delay = r->rxNs - g.genNs;
        int onTimeFlag = (delay >= 0 && delay <= deadlineNs) ? 1 : 0;
        ++delivered;
        ontime += onTimeFlag;
        if (r->hops > 0)
        {
            hopSum += r->hops;
            ++hopN;
            hopHist[r->hops]++;
        }
        pf << g.flow << "," << g.seq << "," << g.genNs << "," << r->rxNs << "," << delay << ","
           << r->hops << ",1," << onTimeFlag << "\n";
    }
    pf.close();

    // ---- Summary ----
    std::string summaryPath = g_cfg.outPrefix + "_summary.csv";
    std::ofstream sf(summaryPath);
    sf << "metric,value\n";
    sf << "num_nodes," << g_cfg.numNodes << "\n";
    sf << "area_side_m," << g_cfg.areaSide << "\n";
    sf << "speed_mps," << g_cfg.speed << "\n";
    sf << "num_flows," << g_cfg.numFlows << "\n";
    sf << "rng_seed," << g_cfg.rngSeed << "\n";
    sf << "rng_run," << g_cfg.rngRun << "\n";
    sf << "generated," << generated << "\n";
    sf << "delivered," << delivered << "\n";
    sf << "ontime," << ontime << "\n";
    sf << "unique_pdr," << (generated ? double(delivered) / double(generated) : 0.0) << "\n";
    sf << "timely_pdr," << (generated ? double(ontime) / double(generated) : 0.0) << "\n";
    sf << "mean_hops," << (hopN ? double(hopSum) / double(hopN) : 0.0) << "\n";

    double routeAvailSum = 0.0;
    for (uint32_t f = 0; f < g_cfg.numFlows; ++f)
    {
        double avail = g_routeStat[f].samples
                           ? double(g_routeStat[f].withRoute) / double(g_routeStat[f].samples)
                           : 0.0;
        routeAvailSum += avail;
        sf << "route_availability_flow" << f << "," << avail << "\n";
    }
    sf << "route_availability_mean," << (g_cfg.numFlows ? routeAvailSum / g_cfg.numFlows : 0.0)
       << "\n";
    for (const auto& kv : hopHist)
    {
        sf << "hop_count_" << kv.first << "," << kv.second << "\n";
    }
    sf.close();

    std::cout << "generated=" << generated << " delivered=" << delivered << " ontime=" << ontime
              << " unique_pdr=" << (generated ? double(delivered) / double(generated) : 0.0)
              << " timely_pdr=" << (generated ? double(ontime) / double(generated) : 0.0)
              << " mean_hops=" << (hopN ? double(hopSum) / double(hopN) : 0.0)
              << " route_avail=" << (g_cfg.numFlows ? routeAvailSum / g_cfg.numFlows : 0.0)
              << "\n";
    std::cout << "wrote " << packetsPath << " and " << summaryPath << "\n";

    Simulator::Destroy();
    return 0;
}
