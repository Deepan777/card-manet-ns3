/*
 * SCR — deterministic contract fixtures T4, T6 and T12.
 *
 * The C0 fixture (scr-c0-fixture.cc) emits counters for T9/T10 but asserts
 * nothing; the checks are applied externally. The three fixtures below are the
 * ones the traceability matrix records as PARTIAL because no explicit,
 * self-asserting scenario existed for them:
 *
 *   T4  — reconnect after 20 s. Exhausting the fast allowance during a hard
 *         partition must not lock the pair out permanently: once connectivity
 *         returns, discovery must resume and service must be re-established
 *         (E9). M18.
 *
 *   T6  — no reverse feedback route. Receipts must never induce their own route
 *         discovery. A receiver with no reverse route suppresses the receipt;
 *         it must NOT originate a RREQ, or feedback would manufacture exactly
 *         the control load the mechanism claims to avoid. M16.
 *
 *   T12 — independent source and receiver metric reconciliation. The generator
 *         and the sink count traffic independently; their totals must reconcile
 *         under a stated identity, so a delivery figure cannot be produced by
 *         one side alone. M01, M19.
 *
 * Each fixture prints its own observations and a PASS/FAIL verdict, and the
 * process exit code is nonzero if any fixture fails, so this is usable as a
 * gate rather than as something a human must read.
 *
 * These are artificial fixtures. They prove implementation behaviour and must
 * never be reported as scientific results.
 *
 * Topology (shared, from the C0 specification):
 *   RangePropagationLossModel, 110 m hard cutoff, 802.11g 6 Mbps.
 *   source (0,0) -- A(100,0) -- B(200,0) -- C(300,0) -- destination (400,0)
 *   No alternate chain, so taking A dark is a genuine partition, not a detour.
 */

#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/internet-module.h"
#include "ns3/mobility-module.h"
#include "ns3/network-module.h"
#include "ns3/propagation-module.h"
#include "ns3/deadline-telemetry-app.h"
#include "ns3/scr-helper.h"
#include "ns3/scr-routing-protocol.h"
#include "ns3/service-receipt-app.h"
#include "ns3/wifi-module.h"

#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("ScrContractFixtures");

namespace
{

int g_failures = 0;

/// Record one assertion. Never aborts: every fixture runs so a single report
/// shows the full picture rather than only the first failure.
void
Check(const std::string& id, const std::string& what, bool ok, const std::string& detail)
{
    if (!ok)
    {
        ++g_failures;
    }
    std::cout << "  " << (ok ? "PASS" : "FAIL") << "  [" << id << "] " << what;
    if (!detail.empty())
    {
        std::cout << "  -- " << detail;
    }
    std::cout << "\n";
}

std::string
Detail(const std::string& k, double v)
{
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s=%g", k.c_str(), v);
    return std::string(buf);
}

void
SetPhyState(Ptr<Node> node, bool on)
{
    Ptr<WifiNetDevice> wifi = DynamicCast<WifiNetDevice>(node->GetDevice(0));
    if (!wifi)
    {
        return;
    }
    if (on)
    {
        wifi->GetPhy()->ResumeFromOff();
    }
    else
    {
        wifi->GetPhy()->SetOffMode();
    }
}

/// Everything a fixture needs from a built topology.
struct Topology
{
    NodeContainer nodes;
    Ipv4InterfaceContainer ifaces;
};

/**
 * Build the shared C0 chain.
 * @param n node count: 5 gives the bare chain (partitionable), more adds
 *          unused nodes far away, which no fixture here needs.
 */
Topology
BuildChain(uint32_t rngRun, uint32_t rngSeed, const std::string& algorithm)
{
    RngSeedManager::SetSeed(rngSeed);
    RngSeedManager::SetRun(rngRun);

    Config::SetDefault("ns3::WifiRemoteStationManager::RtsCtsThreshold", UintegerValue(2347));
    Config::SetDefault("ns3::scr::RoutingProtocol::RreqRateLimit", UintegerValue(10));

    std::vector<Vector> pos = {
        Vector(0, 0, 0),   // 0 source
        Vector(400, 0, 0), // 1 destination
        Vector(100, 0, 0), // 2 relay A -- the break point
        Vector(200, 0, 0), // 3 relay B
        Vector(300, 0, 0), // 4 relay C
    };

    Topology t;
    t.nodes.Create(pos.size());

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
    NetDeviceContainer devices = wifi.Install(phy, mac, t.nodes);

    MobilityHelper mobility;
    Ptr<ListPositionAllocator> alloc = CreateObject<ListPositionAllocator>();
    for (const auto& p : pos)
    {
        alloc->Add(p);
    }
    mobility.SetPositionAllocator(alloc);
    mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    mobility.Install(t.nodes);

    ScrHelper scrRouting;
    InternetStackHelper internet;
    internet.SetRoutingHelper(scrRouting);
    internet.Install(t.nodes);

    // D003: reserve the dedicated jitter stream, exactly as the campaign
    // scenarios do. These fixtures had no stream assignment at all, and the
    // guard in SendRequest caught that the moment D003 was wired up -- which is
    // the point of making it an assertion rather than a silent fallback.
    for (uint32_t i = 0; i < t.nodes.GetN(); ++i)
    {
        Ptr<scr::RoutingProtocol> rp = DynamicCast<scr::RoutingProtocol>(
            t.nodes.Get(i)->GetObject<Ipv4>()->GetRoutingProtocol());
        if (rp)
        {
            rp->AssignJitterStream(6000 + static_cast<int64_t>(i));
        }
    }

    Ipv4AddressHelper address;
    address.SetBase("10.3.0.0", "255.255.0.0");
    t.ifaces = address.Assign(devices);

    scr::AlgorithmMode mode;
    NS_ABORT_MSG_UNLESS(scr::ParseAlgorithmMode(algorithm, mode),
                        "unknown algorithm mode: " << algorithm);
    for (uint32_t i = 0; i < t.nodes.GetN(); ++i)
    {
        Ptr<scr::RoutingProtocol> rp = DynamicCast<scr::RoutingProtocol>(
            t.nodes.Get(i)->GetObject<Ipv4>()->GetRoutingProtocol());
        NS_ABORT_MSG_IF(!rp, "SCR routing protocol not installed");
        rp->SetAlgorithm(mode);
    }
    return t;
}

Ptr<scr::RoutingProtocol>
Rp(const Topology& t, uint32_t i)
{
    return DynamicCast<scr::RoutingProtocol>(
        t.nodes.Get(i)->GetObject<Ipv4>()->GetRoutingProtocol());
}

// ---------------------------------------------------------------------------
// T4 — reconnect after 20 s must not find the pair locked out.
//
// The chain is cut at t=20 s by taking relay A dark and restored at t=40 s.
// Demand is continuous throughout. During the partition SCR must spend its
// fast allowance and stop; that part is E3/E4 and is already covered. What T4
// asserts is the other side of it: that stopping is not permanent. After
// reconnection the source must originate again and packets must arrive.
//
// The failure this guards against is real and was hit once (R009 defect 3):
// a stale in_flight marker that no timer ever cleared silenced the pair for the
// remainder of the run, which looks like excellent control-overhead discipline
// and is in fact a dead flow.
// ---------------------------------------------------------------------------
void
FixtureT4(uint32_t rngRun, uint32_t rngSeed)
{
    std::cout << "\nT4  reconnect after 20 s (E9 no permanent lockout)\n";

    const double breakAt = 20.0;
    const double healAt = 40.0; // exactly 20 s later, per the specification
    const double simEnd = 70.0;

    Topology t = BuildChain(rngRun, rngSeed, "SCR");

    uint16_t port = 7000;
    UdpServerHelper server(port);
    ApplicationContainer sink = server.Install(t.nodes.Get(1));
    sink.Start(Seconds(4.0));
    sink.Stop(Seconds(simEnd));

    UdpClientHelper client(t.ifaces.GetAddress(1), port);
    client.SetAttribute("MaxPackets", UintegerValue(100000));
    client.SetAttribute("Interval", TimeValue(MilliSeconds(50)));
    client.SetAttribute("PacketSize", UintegerValue(512));
    ApplicationContainer src = client.Install(t.nodes.Get(0));
    src.Start(Seconds(5.0));
    src.Stop(Seconds(simEnd - 1.0));

    Simulator::Schedule(Seconds(4.9),
                        &scr::RoutingProtocol::ScrSetDemand,
                        Rp(t, 0),
                        t.ifaces.GetAddress(1),
                        true);

    Simulator::Schedule(Seconds(breakAt), &SetPhyState, t.nodes.Get(2), false);
    Simulator::Schedule(Seconds(healAt), &SetPhyState, t.nodes.Get(2), true);

    // Sample the counters at the instant of reconnection so "before" and
    // "after" are separable. Reading them only at the end cannot distinguish
    // a pair that recovered from one that never stopped.
    struct Snapshot
    {
        uint64_t admitted = 0;
        uint64_t wireRreq = 0;
        uint64_t received = 0;
    };
    auto snap = std::make_shared<Snapshot>();
    Ptr<UdpServer> us = DynamicCast<UdpServer>(sink.Get(0));

    Simulator::Schedule(Seconds(healAt), [snap, t, us]() {
        snap->admitted = Rp(t, 0)->ScrGetAdmitted();
        snap->wireRreq = Rp(t, 0)->ScrGetWireRreq();
        snap->received = us->GetReceived();
    });

    Simulator::Stop(Seconds(simEnd));
    Simulator::Run();

    const uint64_t admittedTotal = Rp(t, 0)->ScrGetAdmitted();
    const uint64_t receivedTotal = us->GetReceived();
    const uint64_t admittedAfter = admittedTotal - snap->admitted;
    const uint64_t receivedAfter = receivedTotal - snap->received;

    Check("T4",
          "service exists before the partition",
          snap->received > 0,
          Detail("received_by_20s", static_cast<double>(snap->received)));

    Check("T4",
          "discovery resumes after reconnection (pair is not locked out)",
          admittedAfter > 0,
          Detail("admitted_after_heal", static_cast<double>(admittedAfter)));

    Check("T4",
          "service is re-established after reconnection",
          receivedAfter > 0,
          Detail("received_after_heal", static_cast<double>(receivedAfter)));

    Check("T4",
          "every RREQ on the wire still carries an admission record",
          Rp(t, 0)->ScrGetUnchargedRreq() == 0,
          Detail("uncharged", static_cast<double>(Rp(t, 0)->ScrGetUnchargedRreq())));

    Check("T4",
          "no zero-delay reschedule clamp fired (R014)",
          Rp(t, 0)->ScrGetZeroDelayClamped() == 0,
          Detail("clamped", static_cast<double>(Rp(t, 0)->ScrGetZeroDelayClamped())));

    std::cout << "      totals: admitted=" << admittedTotal
              << " wire_rreq=" << Rp(t, 0)->ScrGetWireRreq()
              << " denied=" << Rp(t, 0)->ScrGetDenied() << " received=" << receivedTotal << "\n";

    Simulator::Destroy();
}

// ---------------------------------------------------------------------------
// T6 — feedback must not discover its own route.
//
// The receiver emits service receipts back to the source. If those receipts
// were allowed to trigger route discovery, the feedback channel would generate
// the very control traffic SCR exists to bound, and every overhead number in
// the study would be understated by whatever the receipts provoked.
//
// The chain is cut so that the receiver has data to acknowledge but no reverse
// route to acknowledge it over. The assertion is that the receiver originates
// ZERO route requests and instead records the receipts as suppressed.
// ---------------------------------------------------------------------------
void
FixtureT6(uint32_t rngRun, uint32_t rngSeed)
{
    std::cout << "\nT6  feedback never originates a reverse-route discovery\n";

    const double simEnd = 60.0;
    const double breakAt = 20.0; // stays broken: no reverse route can form

    Topology t = BuildChain(rngRun, rngSeed, "SCR");

    const uint16_t dataPort = 7100;
    const uint16_t reportPort = 7200;

    Ptr<scr::ServiceReceiptApp> sink = CreateObject<scr::ServiceReceiptApp>();
    sink->SetAttribute("Port", UintegerValue(dataPort));
    sink->SetAttribute("ReportPort", UintegerValue(reportPort));
    t.nodes.Get(1)->AddApplication(sink);
    sink->SetStartTime(Seconds(4.0));
    sink->SetStopTime(Seconds(simEnd));

    Ptr<scr::DeadlineTelemetryApp> src = CreateObject<scr::DeadlineTelemetryApp>();
    src->SetAttribute("PeerAddress", Ipv4AddressValue(t.ifaces.GetAddress(1)));
    src->SetAttribute("PeerPort", UintegerValue(dataPort));
    src->SetAttribute("SessionId", UintegerValue(4242));
    src->SetAttribute("Interval", TimeValue(MilliSeconds(50)));
    src->SetAttribute("Deadline", TimeValue(MilliSeconds(250)));
    src->SetAttribute("BlockSize", UintegerValue(10));
    src->SetAttribute("PayloadBytes", UintegerValue(512));
    t.nodes.Get(0)->AddApplication(src);
    src->SetStartTime(Seconds(5.0));
    src->SetStopTime(Seconds(simEnd - 1.0));

    Simulator::Schedule(Seconds(4.9),
                        &scr::RoutingProtocol::ScrSetDemand,
                        Rp(t, 0),
                        t.ifaces.GetAddress(1),
                        true);

    // Break the chain and leave it broken. The receiver keeps trying to report
    // on blocks it already received before the cut.
    Simulator::Schedule(Seconds(breakAt), &SetPhyState, t.nodes.Get(2), false);

    Simulator::Stop(Seconds(simEnd));
    Simulator::Run();

    const uint64_t dstRreq = Rp(t, 1)->GetTotalRreqOriginated();
    const uint64_t dstAdmitted = Rp(t, 1)->ScrGetAdmitted();
    const uint64_t dstWire = Rp(t, 1)->ScrGetWireRreq();
    const uint64_t suppressed = sink->GetReportsSuppressedNoRoute();
    const uint64_t reportsSent = sink->GetReportsSent();
    const uint64_t receivedAtSink = sink->GetReceivedCount();

    Check("T6",
          "the receiver actually had traffic to acknowledge",
          receivedAtSink > 0,
          Detail("received_at_sink", static_cast<double>(receivedAtSink)));

    Check("T6",
          "receiver originated ZERO route requests",
          dstRreq == 0,
          Detail("dst_total_rreq", static_cast<double>(dstRreq)));

    Check("T6",
          "receiver put ZERO route requests on the wire",
          dstWire == 0,
          Detail("dst_wire_rreq", static_cast<double>(dstWire)));

    Check("T6",
          "receiver made ZERO admission requests",
          dstAdmitted == 0,
          Detail("dst_admitted", static_cast<double>(dstAdmitted)));

    // OBSERVATION, NOT AN ASSERTION -- and the reason matters.
    //
    // This started as a fifth pass/fail check ("suppressed > 0") and it failed:
    // reports_sent=30, suppressed=0. The check was wrong, not the code. Cutting
    // the chain stops DATA reaching the receiver, so no further block ever
    // closes, so no receipt is ever attempted while the reverse route is
    // missing. The suppression path is unreachable in this topology, and an
    // assertion that cannot fire is worse than none: it reports coverage that
    // does not exist.
    //
    // The T6 contract is the four checks above -- zero receiver-originated
    // discovery -- and those are unambiguous. Reaching the suppression counter
    // needs an asymmetric case (forward path up, reverse path down) that this
    // linear chain cannot express. Recorded as a coverage gap against M16 in
    // METHODOLOGY_CODE_TRACEABILITY.md rather than papered over.
    std::cout << "      reports_sent=" << reportsSent
              << " suppressed=" << suppressed
              << "  (observation only: this topology cannot reach the"
                 " no-reverse-route path -- see comment)\n";
    std::cout << "      received_at_sink=" << receivedAtSink << "\n";

    Simulator::Destroy();
}

// ---------------------------------------------------------------------------
// T12 — source and receiver metrics are independent and must reconcile.
//
// The generator counts what it scheduled; the sink counts what arrived. Neither
// derives its number from the other, so a delivery figure can be checked rather
// than asserted. The identity that must hold on a healthy, uncut chain:
//
//     received + duplicates_rejected + late  <=  generated
//     generated - send_errors               >=  received
//
// A violation means the two sides are not measuring the same population --
// for example a sink counting retransmissions as arrivals, which would inflate
// delivery above what was ever sent.
// ---------------------------------------------------------------------------
void
FixtureT12(uint32_t rngRun, uint32_t rngSeed)
{
    std::cout << "\nT12 independent source and receiver metrics reconcile\n";

    const double simEnd = 60.0;

    Topology t = BuildChain(rngRun, rngSeed, "SCR");

    const uint16_t dataPort = 7300;
    const uint16_t reportPort = 7400;

    Ptr<scr::ServiceReceiptApp> sink = CreateObject<scr::ServiceReceiptApp>();
    sink->SetAttribute("Port", UintegerValue(dataPort));
    sink->SetAttribute("ReportPort", UintegerValue(reportPort));
    t.nodes.Get(1)->AddApplication(sink);
    sink->SetStartTime(Seconds(4.0));
    sink->SetStopTime(Seconds(simEnd));

    Ptr<scr::DeadlineTelemetryApp> src = CreateObject<scr::DeadlineTelemetryApp>();
    src->SetAttribute("PeerAddress", Ipv4AddressValue(t.ifaces.GetAddress(1)));
    src->SetAttribute("PeerPort", UintegerValue(dataPort));
    src->SetAttribute("SessionId", UintegerValue(9001));
    src->SetAttribute("Interval", TimeValue(MilliSeconds(50)));
    src->SetAttribute("Deadline", TimeValue(MilliSeconds(250)));
    src->SetAttribute("BlockSize", UintegerValue(10));
    src->SetAttribute("PayloadBytes", UintegerValue(512));
    t.nodes.Get(0)->AddApplication(src);
    src->SetStartTime(Seconds(5.0));
    src->SetStopTime(Seconds(simEnd - 5.0));

    Simulator::Schedule(Seconds(4.9),
                        &scr::RoutingProtocol::ScrSetDemand,
                        Rp(t, 0),
                        t.ifaces.GetAddress(1),
                        true);

    Simulator::Stop(Seconds(simEnd));
    Simulator::Run();

    const uint64_t generated = src->GetGeneratedCount();
    const uint64_t sendErrors = src->GetSendErrorCount();
    const uint64_t received = sink->GetReceivedCount();
    const uint64_t duplicates = sink->GetDuplicateCount();
    const uint64_t late = sink->GetLateCount();
    const uint64_t malformed = sink->GetMalformedCount();

    Check("T12",
          "the source generated traffic",
          generated > 0,
          Detail("generated", static_cast<double>(generated)));

    Check("T12",
          "the receiver observed traffic",
          received > 0,
          Detail("received", static_cast<double>(received)));

    Check("T12",
          "arrivals never exceed what was generated",
          received <= generated,
          Detail("generated", static_cast<double>(generated)) + " " +
              Detail("received", static_cast<double>(received)));

    Check("T12",
          "arrivals never exceed what was successfully handed to the socket",
          received <= (generated - sendErrors),
          Detail("generated_minus_send_errors", static_cast<double>(generated - sendErrors)));

    Check("T12",
          "counted populations do not overlap beyond the generated total",
          (received + duplicates + late) <= generated,
          Detail("received+dup+late", static_cast<double>(received + duplicates + late)));

    Check("T12",
          "no malformed telemetry (serialization is lossless end to end)",
          malformed == 0,
          Detail("malformed", static_cast<double>(malformed)));

    Check("T12",
          "the source reports no transport integration error",
          !src->HasTransportIntegrationError(),
          "");

    std::cout << "      generated=" << generated << " send_errors=" << sendErrors
              << " received=" << received << " duplicates=" << duplicates << " late=" << late
              << " loss=" << (generated - received) << "\n";

    Simulator::Destroy();
}

} // namespace


// ---------------------------------------------------------------------------
// D002 - the fork's full-diameter reply timeout is FLAT NetTraversalTime.
//
// D002 was registered on 2026-09-07, before any fork code existed. It records
// that upstream AODV applies binary exponential backoff to the full-diameter
// RREQ retry timeout, that E4 specifies a flat NetTraversalTime instead, and
// that the fork implements E4 as written. Its stated reason:
//
//   "Inheriting an additional native exponential backoff would silently add a
//    second, unregistered throttle and confound exactly the comparison the
//    study is designed to make - in particular against PERSISTENT-BACKOFF."
//
// It also registered a validating test: "a direct assertion that observed
// full-diameter retry intervals equal NetTraversalTime in the fork and follow
// 2^k scaling in AODV-STOCK."
//
// That test was never written. The fork shipped with upstream's exponential
// still in ScheduleRreqRetry, so every SCR-family run up to 2026-09-14 carried
// the second throttle D002 forbade, and the confound it predicted is exactly
// what R027 reported. This is that test, written late.
//
// D002 is equally clear that AODV-STOCK keeps native behaviour, so the second
// half asserts the exponential is still THERE for the native reference. A fix
// that made both flat would also be wrong.
// ---------------------------------------------------------------------------
void
FixtureD002(uint32_t rngRun, uint32_t rngSeed)
{
    std::cout << "\nD002  full-diameter reply timeout: flat in the fork, 2^k in AODV-STOCK\n";

    // A destination that exists in no routing table: every request to it goes
    // unanswered, so the retry timer is armed repeatedly at full diameter.
    const Ipv4Address unreachable("10.99.99.99");

    for (const std::string algo : {"SCR", "AODV-STOCK"})
    {
        Topology t = BuildChain(rngRun, rngSeed, algo);
        Ptr<scr::RoutingProtocol> rp = Rp(t, 0);

        std::vector<double> armed;   // seconds
        std::vector<uint16_t> ttls;
        for (int i = 1; i <= 200; ++i)
        {
            Simulator::Schedule(Seconds(i * 0.25), [&armed, &ttls, rp]() {
                const double d = rp->GetLastRreqRetryDelay().GetSeconds();
                if (d > 0.0 && (armed.empty() || armed.back() != d ||
                                ttls.back() != rp->GetLastRreqRetryTtl()))
                {
                    armed.push_back(d);
                    ttls.push_back(rp->GetLastRreqRetryTtl());
                }
            });
        }
        // Provoke discovery with REAL traffic through the stack. An earlier
        // fixture in this file called RouteOutput() directly and that is exactly
        // the mistake logged as defect 6 in IMPLEMENTATION_STATUS: a caller
        // outside the stack both misreports availability and injects requests
        // that the protocol never actually decided to send.
        Ptr<Socket> src = Socket::CreateSocket(
            t.nodes.Get(0), TypeId::LookupByName("ns3::UdpSocketFactory"));
        src->Bind();
        src->Connect(InetSocketAddress(unreachable, 9999));
        for (int i = 0; i < 12; ++i)
        {
            Simulator::Schedule(Seconds(1.0 + i * 2.0),
                                [src]() { src->Send(Create<Packet>(64)); });
        }

        Simulator::Stop(Seconds(52.0));
        Simulator::Run();

        // Full-diameter arms only: TTL == ttl_max is where the two rules differ.
        std::vector<double> full;
        for (size_t i = 0; i < armed.size(); ++i)
        {
            if (ttls[i] >= 35)
            {
                full.push_back(armed[i]);
            }
        }

        // How many arms to expect differs BY DESIGN, and conflating the two was
        // this fixture's first mistake. AODV-STOCK retries on a timer and so
        // arms repeatedly; SCR rations discovery, so against an unreachable
        // destination its budget denies further originations and it arms once.
        // Demanding two arms from SCR tests the rationing, not the timeout.
        const size_t needed = (algo == "SCR") ? 1u : 2u;
        if (full.size() < needed)
        {
            Check("D002", algo + ": full-diameter arms observed",
                  false, Detail("observed", static_cast<double>(full.size())) +
                         " " + Detail("needed", static_cast<double>(needed)));
            Simulator::Destroy();
            continue;
        }

        bool allFlat = true;
        bool anyDoubling = false;
        for (size_t i = 1; i < full.size(); ++i)
        {
            if (std::abs(full[i] - full[0]) > 1e-6)
            {
                allFlat = false;
            }
            if (std::abs(full[i] - 2.0 * full[i - 1]) < 1e-6)
            {
                anyDoubling = true;
            }
        }

        if (algo == "SCR")
        {
            // The value itself is the whole point: under the pre-fix binary
            // this was 2.8 * 2^k and grew without bound.
            Check("D002", "fork: every full-diameter T_wait equals NetTraversalTime",
                  allFlat && std::abs(full[0] - 2.8) < 1e-6,
                  Detail("t_wait_s", full[0]) + " " + Detail("arms",
                         static_cast<double>(full.size())));
            Check("D002", "fork: no 2^k doubling in the full-diameter timeout",
                  !anyDoubling,
                  "a doubling here is the unregistered second throttle D002 forbids");
        }
        else
        {
            Check("D002", "AODV-STOCK: native exponential backoff retained",
                  anyDoubling || full.size() < 2,
                  Detail("arms", static_cast<double>(full.size())));
        }
        Simulator::Destroy();
    }
}


// ---------------------------------------------------------------------------
// D003 - matched controller variants share one jitter sequence.
//
// D003 registered a dedicated RNG stream for the 0-10 ms scheduler jitter,
// "shared across matched controller variants", and registered two assertions:
// the distribution of handoff offsets, and identical jitter sequences across
// matched variants under the same seed. Neither was written, and the stream
// itself was never wired up -- m_scrJitter was declared in the header and never
// constructed. The jitter came from the shared native stream, which different
// modes consume at different rates, so matched variants drew DIFFERENT
// sequences: the exact opposite of the guarantee.
//
// The per-node dedicated stream makes this true by construction. That is not
// evidence: D002 was also true by construction for seven days while being false
// in the shipped binary. So it is asserted.
// ---------------------------------------------------------------------------
void
FixtureD003(uint32_t rngRun, uint32_t rngSeed)
{
    std::cout << "\nD003  matched controller variants share one jitter sequence\n";

    std::map<std::string, std::vector<std::vector<uint32_t>>> seqs; // algo -> per node

    for (const std::string algo : {"SCR", "RREP-RESET", "TIME-BUCKET"})
    {
        Topology t = BuildChain(rngRun, rngSeed, algo);

        // Two destinations, because one is not enough to exercise the stream.
        // The reachable peer produces a handful of draws and then stops needing
        // discovery; an unreachable address keeps every variant originating, so
        // the sequences being compared are long enough for the comparison to
        // mean something. The first version of this fixture compared four
        // values.
        const uint16_t port = 7400;
        Ptr<Socket> src = Socket::CreateSocket(
            t.nodes.Get(0), TypeId::LookupByName("ns3::UdpSocketFactory"));
        src->Bind();
        src->Connect(InetSocketAddress(t.ifaces.GetAddress(1), port));

        Ptr<Socket> dead = Socket::CreateSocket(
            t.nodes.Get(0), TypeId::LookupByName("ns3::UdpSocketFactory"));
        dead->Bind();
        dead->Connect(InetSocketAddress(Ipv4Address("10.3.200.200"), port));

        Ptr<Socket> dead2 = Socket::CreateSocket(
            t.nodes.Get(4), TypeId::LookupByName("ns3::UdpSocketFactory"));
        dead2->Bind();
        dead2->Connect(InetSocketAddress(Ipv4Address("10.3.201.201"), port));

        for (int i = 0; i < 100; ++i)
        {
            Simulator::Schedule(Seconds(1.0 + i * 1.0),
                                [src]() { src->Send(Create<Packet>(64)); });
            Simulator::Schedule(Seconds(1.5 + i * 1.0),
                                [dead]() { dead->Send(Create<Packet>(64)); });
            Simulator::Schedule(Seconds(1.7 + i * 1.0),
                                [dead2]() { dead2->Send(Create<Packet>(64)); });
        }

        Simulator::Stop(Seconds(110.0));
        Simulator::Run();

        std::vector<std::vector<uint32_t>> perNode;
        for (uint32_t i = 0; i < t.nodes.GetN(); ++i)
        {
            perNode.push_back(Rp(t, i)->GetJitterDraws());
        }
        seqs[algo] = perNode;
        Simulator::Destroy();
    }

    // 1. Registered assertion: handoff offsets lie in the declared 0-10 ms range.
    size_t drawn = 0;
    bool inRange = true;
    for (const auto& kv : seqs)
    {
        for (const auto& node : kv.second)
        {
            for (uint32_t v : node)
            {
                ++drawn;
                if (v > 10)
                {
                    inRange = false;
                }
            }
        }
    }
    Check("D003", "every dedicated-stream jitter draw lies in 0-10 ms",
          inRange && drawn > 0, Detail("draws", static_cast<double>(drawn)));

    // 2. Registered assertion: matched variants share the sequence. Variants
    //    originate at different times and counts, so only the common prefix of
    //    each node's sequence is comparable -- but on that prefix it must agree
    //    value for value, which is what a per-node dedicated stream buys.
    const auto& ref = seqs.at("SCR");
    size_t compared = 0;
    std::string firstMismatch;
    for (const auto& kv : seqs)
    {
        if (kv.first == "SCR")
        {
            continue;
        }
        for (size_t n = 0; n < ref.size() && n < kv.second.size(); ++n)
        {
            const size_t m = std::min(ref[n].size(), kv.second[n].size());
            for (size_t k = 0; k < m; ++k)
            {
                ++compared;
                if (ref[n][k] != kv.second[n][k] && firstMismatch.empty())
                {
                    firstMismatch = kv.first + " node " + std::to_string(n) +
                                    " draw " + std::to_string(k) + ": " +
                                    std::to_string(ref[n][k]) + " vs " +
                                    std::to_string(kv.second[n][k]);
                }
            }
        }
    }
    Check("D003", "matched variants draw an identical jitter sequence",
          firstMismatch.empty() && compared > 0,
          firstMismatch.empty() ? Detail("values_compared", static_cast<double>(compared))
                                : firstMismatch);
}

int
main(int argc, char* argv[])
{
    uint32_t rngRun = 1;
    uint32_t rngSeed = 20260905;
    std::string only;

    CommandLine cmd(__FILE__);
    cmd.AddValue("rngRun", "RngRun", rngRun);
    cmd.AddValue("rngSeed", "RngSeed", rngSeed);
    cmd.AddValue("only", "Run a single fixture: T4|T6|T12|D002|D003", only);
    cmd.Parse(argc, argv);

    std::cout << "SCR contract fixtures T4, T6, T12  (rngRun=" << rngRun << " rngSeed=" << rngSeed
              << ")\n";
    std::cout << "These are artificial fixtures. They prove implementation behaviour\n"
                 "and must never be reported as scientific results.\n";

    if (only.empty() || only == "T4")
    {
        FixtureT4(rngRun, rngSeed);
    }
    if (only.empty() || only == "T6")
    {
        FixtureT6(rngRun, rngSeed);
    }
    if (only.empty() || only == "T12")
    {
        FixtureT12(rngRun, rngSeed);
    }
    if (only.empty() || only == "D002")
    {
        FixtureD002(rngRun, rngSeed);
    }
    if (only.empty() || only == "D003")
    {
        FixtureD003(rngRun, rngSeed);
    }

    std::cout << "\n";
    if (g_failures == 0)
    {
        std::cout << "RESULT: all contract fixtures passed\n";
        return 0;
    }
    std::cout << "RESULT: " << g_failures << " contract assertion(s) FAILED\n";
    return 1;
}
