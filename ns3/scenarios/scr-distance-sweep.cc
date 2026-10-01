/*
 * SCR — two-node distance sweep (Stage 2 topology validation)
 *
 * Purpose (FINAL_RESEARCH_SPECIFICATION section 7, "Primary radio and traffic plan"):
 *   "Before tuning SCR, run a two-node distance sweep using the same PHY, packet size and
 *    rate, then record reception versus distance. If the intended 3-6 hop environment is
 *    not obtained, adjust the common scenario geometry once during pilots, document the
 *    reason, and freeze it before comparing algorithms."
 *
 * This program measures single-hop reception versus distance under the EXACT registered
 * PHY of the primary study. It deliberately does NOT assign a fixed "radio range": the
 * channel is probabilistic and the output is a reception curve, not a range constant.
 *
 * One invocation measures one distance and prints one CSV row. The sweep is driven
 * externally by scripts/run_distance_sweep.sh so that each point is an independent
 * simulator process with its own seed/run assignment.
 *
 * No SCR logic is present here. This is native ns-3 only.
 */

#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/internet-module.h"
#include "ns3/mobility-module.h"
#include "ns3/network-module.h"
#include "ns3/propagation-module.h"
#include "ns3/wifi-module.h"

#include <iomanip>
#include <iostream>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("ScrDistanceSweep");

int
main(int argc, char* argv[])
{
    // ---- Registered PHY/traffic parameters (specification section 7) ----
    double distance = 100.0;      // m, swept externally
    double txPowerDbm = 16.0;     // registered
    double rxNoiseFigureDb = 7.0; // registered
    double pathLossExponent = 2.7;
    double referenceDistance = 1.0;    // m
    double referenceLossDb = 40.046;   // dB
    uint32_t payloadBytes = 512;       // application payload
    double packetsPerSecond = 20.0;    // per flow
    double durationS = 20.0;           // measurement window
    uint32_t rngRun = 1;
    uint32_t rngSeed = 20260905; // registered base seed
    std::string dataMode = "ErpOfdmRate6Mbps";
    bool printHeader = false;

    CommandLine cmd(__FILE__);
    cmd.AddValue("distance", "Separation between the two nodes in metres", distance);
    cmd.AddValue("payloadBytes", "UDP application payload bytes", payloadBytes);
    cmd.AddValue("rate", "Application packets per second", packetsPerSecond);
    cmd.AddValue("duration", "Traffic duration in seconds", durationS);
    cmd.AddValue("rngRun", "ns-3 RngRun number", rngRun);
    cmd.AddValue("rngSeed", "ns-3 RngSeed", rngSeed);
    cmd.AddValue("txPowerDbm", "Transmit power in dBm", txPowerDbm);
    cmd.AddValue("printHeader", "Print CSV header row and exit", printHeader);
    cmd.Parse(argc, argv);

    if (printHeader)
    {
        std::cout << "distance_m,sent,received,pdr,payload_bytes,rate_pps,duration_s,"
                     "tx_power_dbm,rx_noise_figure_db,path_loss_exponent,reference_loss_db,"
                     "data_mode,rng_seed,rng_run\n";
        return 0;
    }

    RngSeedManager::SetSeed(rngSeed);
    RngSeedManager::SetRun(rngRun);

    // RTS/CTS threshold 2347 bytes (registered): effectively disabled for our payloads.
    Config::SetDefault("ns3::WifiRemoteStationManager::RtsCtsThreshold", UintegerValue(2347));

    NodeContainer nodes;
    nodes.Create(2);

    // ---- Channel: LogDistance + constant-speed delay (registered) ----
    YansWifiChannelHelper channel;
    channel.SetPropagationDelay("ns3::ConstantSpeedPropagationDelayModel");
    channel.AddPropagationLoss("ns3::LogDistancePropagationLossModel",
                               "Exponent",
                               DoubleValue(pathLossExponent),
                               "ReferenceDistance",
                               DoubleValue(referenceDistance),
                               "ReferenceLoss",
                               DoubleValue(referenceLossDb));

    YansWifiPhyHelper phy;
    phy.SetChannel(channel.Create());
    phy.Set("TxPowerStart", DoubleValue(txPowerDbm));
    phy.Set("TxPowerEnd", DoubleValue(txPowerDbm));
    phy.Set("RxNoiseFigure", DoubleValue(rxNoiseFigureDb));

    // ---- 802.11g, ad hoc, constant rate (registered) ----
    WifiHelper wifi;
    wifi.SetStandard(WIFI_STANDARD_80211g);
    wifi.SetRemoteStationManager("ns3::ConstantRateWifiManager",
                                 "DataMode",
                                 StringValue(dataMode),
                                 "ControlMode",
                                 StringValue(dataMode));

    WifiMacHelper mac;
    mac.SetType("ns3::AdhocWifiMac");

    NetDeviceContainer devices = wifi.Install(phy, mac, nodes);

    // ---- Fixed positions at the swept separation ----
    MobilityHelper mobility;
    Ptr<ListPositionAllocator> positions = CreateObject<ListPositionAllocator>();
    positions->Add(Vector(0.0, 0.0, 0.0));
    positions->Add(Vector(distance, 0.0, 0.0));
    mobility.SetPositionAllocator(positions);
    mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    mobility.Install(nodes);

    // ---- Internet stack. No routing protocol: this is a single-hop PHY probe. ----
    InternetStackHelper internet;
    internet.Install(nodes);

    Ipv4AddressHelper address;
    address.SetBase("10.1.1.0", "255.255.255.0");
    Ipv4InterfaceContainer interfaces = address.Assign(devices);

    // ---- Traffic: node 0 -> node 1, periodic UDP at the registered rate/payload ----
    uint16_t port = 9;
    UdpServerHelper server(port);
    ApplicationContainer serverApp = server.Install(nodes.Get(1));
    serverApp.Start(Seconds(0.5));
    serverApp.Stop(Seconds(durationS + 2.0));

    uint32_t maxPackets = static_cast<uint32_t>(packetsPerSecond * durationS);
    UdpClientHelper client(interfaces.GetAddress(1), port);
    client.SetAttribute("MaxPackets", UintegerValue(maxPackets));
    client.SetAttribute("Interval", TimeValue(Seconds(1.0 / packetsPerSecond)));
    client.SetAttribute("PacketSize", UintegerValue(payloadBytes));
    ApplicationContainer clientApp = client.Install(nodes.Get(0));
    clientApp.Start(Seconds(1.0));
    clientApp.Stop(Seconds(1.0 + durationS));

    Simulator::Stop(Seconds(durationS + 3.0));
    Simulator::Run();

    Ptr<UdpServer> udpServer = DynamicCast<UdpServer>(serverApp.Get(0));
    uint64_t received = udpServer->GetReceived();

    // Denominator is the number of packets the application actually scheduled. The client
    // stop time is set after all MaxPackets are due, so every scheduled packet is handed
    // to the socket. Packets generated but never delivered therefore remain counted,
    // as specification E10 requires.
    uint64_t sent = maxPackets;

    double pdr = (sent > 0) ? (static_cast<double>(received) / static_cast<double>(sent)) : 0.0;

    std::cout << std::fixed << std::setprecision(1) << distance << "," << sent << ","
              << received << "," << std::setprecision(6) << pdr << "," << payloadBytes << ","
              << std::setprecision(1) << packetsPerSecond << "," << durationS << ","
              << txPowerDbm << "," << rxNoiseFigureDb << "," << std::setprecision(2)
              << pathLossExponent << "," << std::setprecision(3) << referenceLossDb << ","
              << dataMode << "," << rngSeed << "," << rngRun << "\n";

    Simulator::Destroy();
    return 0;
}
