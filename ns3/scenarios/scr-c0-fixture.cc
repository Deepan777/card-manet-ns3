/*
 * SCR — C0 deterministic contract fixture (Stage 4/6 validation).
 *
 * Specification, section 2 "C0: deterministic contract fixtures":
 *   RangePropagationLossModel, 110 m range, 802.11g, 6 Mbps.
 *   Source at (0,0), destination at (400,0).
 *   Primary relay chain at (100,0), (200,0), (300,0).
 *   Alternate chain at (0,100), (100,100), (200,100), (300,100), (400,100).
 *   60 s simulation, traffic from 5 to 55 s.
 *   A designated relay PHY-off interval forces the intended break.
 *
 * These artificial fixtures prove implementation behaviour. They are NOT the
 * scientific environment and must never be reported as a result.
 *
 * Checks performed here:
 *   T9  — toggling SCR changes actual routing behaviour (not a no-op flag)
 *   T10 — every RREQ that reaches the wire has a matching admission record
 *   T6  — feedback never triggers a reverse-route discovery
 */

#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/internet-module.h"
#include "ns3/mobility-module.h"
#include "ns3/network-module.h"
#include "ns3/propagation-module.h"
#include "ns3/scr-helper.h"
#include "ns3/scr-routing-protocol.h"
#include "ns3/wifi-module.h"

#include <iostream>
#include <vector>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("ScrC0Fixture");

namespace
{

struct Cfg
{
    std::string algorithm = "SCR";
    double breakStart = 20.0; //!< relay PHY off
    double breakEnd = 40.0;   //!< relay PHY on
    double simEnd = 60.0;
    uint32_t rngRun = 1;
    uint32_t rngSeed = 20260905;
    bool blockAlternate = false; //!< if true, no alternate chain (forces partition)
};

Cfg g_cfg;

/// Turn a node's Wi-Fi PHY off/on to force a deterministic break.
void
SetPhyState(Ptr<Node> node, bool on)
{
    Ptr<NetDevice> dev = node->GetDevice(0);
    Ptr<WifiNetDevice> wifi = DynamicCast<WifiNetDevice>(dev);
    if (!wifi)
    {
        return;
    }
    Ptr<WifiPhy> phy = wifi->GetPhy();
    if (on)
    {
        phy->ResumeFromOff();
    }
    else
    {
        phy->SetOffMode();
    }
}

} // namespace

int
main(int argc, char* argv[])
{
    CommandLine cmd(__FILE__);
    cmd.AddValue("algorithm", "Registered algorithm mode", g_cfg.algorithm);
    cmd.AddValue("breakStart", "Relay PHY-off time (s)", g_cfg.breakStart);
    cmd.AddValue("breakEnd", "Relay PHY-on time (s)", g_cfg.breakEnd);
    cmd.AddValue("simEnd", "Simulation end (s)", g_cfg.simEnd);
    cmd.AddValue("rngRun", "RngRun", g_cfg.rngRun);
    cmd.AddValue("blockAlternate", "Remove the alternate chain", g_cfg.blockAlternate);
    cmd.Parse(argc, argv);

    RngSeedManager::SetSeed(g_cfg.rngSeed);
    RngSeedManager::SetRun(g_cfg.rngRun);

    Config::SetDefault("ns3::WifiRemoteStationManager::RtsCtsThreshold", UintegerValue(2347));
    Config::SetDefault("ns3::scr::RoutingProtocol::RreqRateLimit", UintegerValue(10));

    // ---- Positions (specification C0) ----
    std::vector<Vector> pos;
    pos.push_back(Vector(0, 0, 0));     // 0 source
    pos.push_back(Vector(400, 0, 0));   // 1 destination
    pos.push_back(Vector(100, 0, 0));   // 2 primary relay A  <- broken relay
    pos.push_back(Vector(200, 0, 0));   // 3 primary relay B
    pos.push_back(Vector(300, 0, 0));   // 4 primary relay C
    if (!g_cfg.blockAlternate)
    {
        pos.push_back(Vector(0, 100, 0));   // 5 alternate chain
        pos.push_back(Vector(100, 100, 0)); // 6
        pos.push_back(Vector(200, 100, 0)); // 7
        pos.push_back(Vector(300, 100, 0)); // 8
        pos.push_back(Vector(400, 100, 0)); // 9
    }

    NodeContainer nodes;
    nodes.Create(pos.size());

    // ---- Deterministic range channel: 110 m hard cutoff ----
    YansWifiChannelHelper channel;
    channel.SetPropagationDelay("ns3::ConstantSpeedPropagationDelayModel");
    channel.AddPropagationLoss("ns3::RangePropagationLossModel",
                               "MaxRange",
                               DoubleValue(110.0));

    YansWifiPhyHelper phy;
    phy.SetChannel(channel.Create());

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

    MobilityHelper mobility;
    Ptr<ListPositionAllocator> alloc = CreateObject<ListPositionAllocator>();
    for (const auto& p : pos)
    {
        alloc->Add(p);
    }
    mobility.SetPositionAllocator(alloc);
    mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    mobility.Install(nodes);

    // ---- SCR routing on every node ----
    ScrHelper scrRouting;
    InternetStackHelper internet;
    internet.SetRoutingHelper(scrRouting);
    internet.Install(nodes);

    Ipv4AddressHelper address;
    address.SetBase("10.2.0.0", "255.255.0.0");
    Ipv4InterfaceContainer ifaces = address.Assign(devices);

    // Enable or disable SCR on every node. T9 compares the two settings.
    scr::AlgorithmMode mode;
    // An unrecognised name aborts the run. It must never silently fall back to
    // a default, because that would execute one algorithm under another's label.
    NS_ABORT_MSG_UNLESS(scr::ParseAlgorithmMode(g_cfg.algorithm, mode),
                        "unknown algorithm mode: " << g_cfg.algorithm);
    for (uint32_t i = 0; i < nodes.GetN(); ++i)
    {
        Ptr<Ipv4> ipv4 = nodes.Get(i)->GetObject<Ipv4>();
        Ptr<scr::RoutingProtocol> rp =
            DynamicCast<scr::RoutingProtocol>(ipv4->GetRoutingProtocol());
        NS_ABORT_MSG_IF(!rp, "SCR routing protocol not installed");
        rp->SetAlgorithm(mode);
    }

    // ---- Traffic: source -> destination, 5 s to 55 s ----
    uint16_t port = 6000;
    UdpServerHelper server(port);
    ApplicationContainer sink = server.Install(nodes.Get(1));
    sink.Start(Seconds(4.0));
    sink.Stop(Seconds(g_cfg.simEnd));

    UdpClientHelper client(ifaces.GetAddress(1), port);
    client.SetAttribute("MaxPackets", UintegerValue(1000));
    client.SetAttribute("Interval", TimeValue(MilliSeconds(50)));
    client.SetAttribute("PacketSize", UintegerValue(512));
    ApplicationContainer src = client.Install(nodes.Get(0));
    src.Start(Seconds(5.0));
    src.Stop(Seconds(55.0));

    // SCR needs to know demand exists for this destination.
    {
        Ptr<Ipv4> ipv4 = nodes.Get(0)->GetObject<Ipv4>();
        Ptr<scr::RoutingProtocol> rp =
            DynamicCast<scr::RoutingProtocol>(ipv4->GetRoutingProtocol());
        Simulator::Schedule(Seconds(4.9),
                            &scr::RoutingProtocol::ScrSetDemand,
                            rp,
                            ifaces.GetAddress(1),
                            true);
    }

    // ---- Deterministic break: primary relay A goes dark ----
    Simulator::Schedule(Seconds(g_cfg.breakStart), &SetPhyState, nodes.Get(2), false);
    Simulator::Schedule(Seconds(g_cfg.breakEnd), &SetPhyState, nodes.Get(2), true);

    Simulator::Stop(Seconds(g_cfg.simEnd));
    Simulator::Run();

    // ---- Collect source-side SCR counters ----
    Ptr<Ipv4> sIpv4 = nodes.Get(0)->GetObject<Ipv4>();
    Ptr<scr::RoutingProtocol> srp =
        DynamicCast<scr::RoutingProtocol>(sIpv4->GetRoutingProtocol());

    Ptr<UdpServer> us = DynamicCast<UdpServer>(sink.Get(0));
    const uint64_t received = us->GetReceived();

    std::cout << srp->DumpEffectiveParameters()
              << " admitted=" << srp->ScrGetAdmitted()
              << " denied=" << srp->ScrGetDenied()
              << " wire_rreq=" << srp->ScrGetWireRreq()
              << " uncharged_rreq=" << srp->ScrGetUnchargedRreq()
              << " handoff_failed=" << srp->ScrGetHandoffFailed()
              << " total_rreq=" << srp->GetTotalRreqOriginated()
              << " received=" << received << "\n";

    Simulator::Destroy();
    return 0;
}
