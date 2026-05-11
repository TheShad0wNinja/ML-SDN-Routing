/*
 * stats-test — validates the full stats-collection and DDPG metric pipeline.
 *
 * Topology  (ring of 4 switches, 2 hosts per switch):
 *
 *   H0 H1          H2 H3
 *    |  |            |  |
 *   [S0]---10ms---[S1]
 *    |               |
 *   2ms             2ms
 *    |               |
 *   [S3]---50ms---[S2]
 *    |  |            |  |
 *   H6 H7          H4 H5
 *
 * Stats interval: 5 s  (vs default 60 s)
 * Simulation time: 120 s
 *
 * Host annotations: name (H0–H7) and node_type for topology viewer display only.
 *
 * Switch energy model: each switch starts with 10 kJ and consumes 1 nJ per byte
 * forwarded. The DDPG model can use residual_energy_j to bias routing away from
 * energy-depleted forwarding paths.
 */

#include "zmq-openflow-controller.h"
#include <array>
#include <ns3/applications-module.h>
#include <ns3/core-module.h>
#include <ns3/csma-module.h>
#include <ns3/internet-module.h>
#include <ns3/network-module.h>
#include <ns3/ofswitch13-module.h>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("StatsTestScene");

// Helper: pack a 6-byte MAC byte-string into a uint64_t (big-endian)
static uint64_t MacToU64(const Mac48Address& addr)
{
    uint8_t buf[6];
    addr.CopyTo(buf);
    uint64_t v = 0;
    for (int i = 0; i < 6; ++i) v = (v << 8) | buf[i];
    return v;
}

int main(int argc, char* argv[])
{
    bool trace    = false;
    double simTime = 120.0;

    CommandLine cmd(__FILE__);
    cmd.AddValue("trace",   "Enable pcap/datapath traces", trace);
    cmd.AddValue("simTime", "Simulation duration (s)",     simTime);
    cmd.Parse(argc, argv);

    NS_LOG_INFO("stats-test: creating nodes");

    // 8 hosts, 4 switches, 1 controller
    NodeContainer hosts;    hosts.Create(8);
    NodeContainer switches; switches.Create(4);
    NodeContainer ctrl;     ctrl.Create(1);

    // ----------------------------------------------------------------
    // Link helpers — three cost tiers matching GÉANT-inspired costs
    // ----------------------------------------------------------------
    CsmaHelper csmaFast, csmaMed, csmaSlow, csmaEdge;

    csmaFast.SetChannelAttribute("DataRate", StringValue("100Mbps"));
    csmaFast.SetChannelAttribute("Delay",    StringValue("2ms"));

    csmaMed.SetChannelAttribute("DataRate", StringValue("100Mbps"));
    csmaMed.SetChannelAttribute("Delay",    StringValue("10ms"));

    csmaSlow.SetChannelAttribute("DataRate", StringValue("10Mbps"));
    csmaSlow.SetChannelAttribute("Delay",    StringValue("50ms"));

    csmaEdge.SetChannelAttribute("DataRate", StringValue("100Mbps"));
    csmaEdge.SetChannelAttribute("Delay",    StringValue("1ms"));

    // ----------------------------------------------------------------
    // Wire up hosts → switches  (edge links)
    // swPorts[i] accumulates the switch-side NetDevices for switch i
    // ----------------------------------------------------------------
    NetDeviceContainer swPorts[4];
    NetDeviceContainer hostPorts;

    auto connectHostToSwitch = [&](Ptr<Node> host, Ptr<Node> sw,
                                   NetDeviceContainer& swPortsRef,
                                   CsmaHelper& helper) {
        NetDeviceContainer d = helper.Install(NodeContainer(host, sw));
        hostPorts.Add(d.Get(0));
        swPortsRef.Add(d.Get(1));
    };

    // S0 hosts: H0, H1
    connectHostToSwitch(hosts.Get(0), switches.Get(0), swPorts[0], csmaEdge);
    connectHostToSwitch(hosts.Get(1), switches.Get(0), swPorts[0], csmaEdge);
    // S1 hosts: H2, H3
    connectHostToSwitch(hosts.Get(2), switches.Get(1), swPorts[1], csmaEdge);
    connectHostToSwitch(hosts.Get(3), switches.Get(1), swPorts[1], csmaEdge);
    // S2 hosts: H4, H5  (server switch)
    connectHostToSwitch(hosts.Get(4), switches.Get(2), swPorts[2], csmaEdge);
    connectHostToSwitch(hosts.Get(5), switches.Get(2), swPorts[2], csmaEdge);
    // S3 hosts: H6, H7
    connectHostToSwitch(hosts.Get(6), switches.Get(3), swPorts[3], csmaEdge);
    connectHostToSwitch(hosts.Get(7), switches.Get(3), swPorts[3], csmaEdge);

    // ----------------------------------------------------------------
    // Inter-switch links (ring):  S0-S1  S1-S2  S2-S3  S3-S0
    // ----------------------------------------------------------------
    auto connectSwitches = [&](Ptr<Node> a, Ptr<Node> b,
                                NetDeviceContainer& aPorts,
                                NetDeviceContainer& bPorts,
                                CsmaHelper& helper) {
        NetDeviceContainer d = helper.Install(NodeContainer(a, b));
        aPorts.Add(d.Get(0));
        bPorts.Add(d.Get(1));
    };

    connectSwitches(switches.Get(0), switches.Get(1), swPorts[0], swPorts[1], csmaMed);   // S0-S1: 10ms
    connectSwitches(switches.Get(1), switches.Get(2), swPorts[1], swPorts[2], csmaFast);  // S1-S2: 2ms
    connectSwitches(switches.Get(2), switches.Get(3), swPorts[2], swPorts[3], csmaSlow);  // S2-S3: 50ms
    connectSwitches(switches.Get(3), switches.Get(0), swPorts[3], swPorts[0], csmaFast);  // S3-S0: 2ms

    // ----------------------------------------------------------------
    // IP stack on hosts
    // ----------------------------------------------------------------
    InternetStackHelper inet;
    inet.Install(hosts);

    Ipv4AddressHelper ipv4;
    ipv4.SetBase("10.2.1.0", "255.255.255.0");
    Ipv4InterfaceContainer hostIfaces = ipv4.Assign(hostPorts);

    // ----------------------------------------------------------------
    // OpenFlow domain
    // ----------------------------------------------------------------
    Ptr<OFSwitch13InternalHelper> ofHelper = CreateObject<OFSwitch13InternalHelper>();
    Ptr<ZmqOpenFlowController>    ctrlApp  = CreateObject<ZmqOpenFlowController>();

    ctrl.Get(0)->AddApplication(ctrlApp);
    ctrlApp->SetStartTime(Seconds(0));

    // Stats every 5 s for fast verification
    ctrlApp->SetStatsInterval(5.0);

    ofHelper->InstallController(ctrl.Get(0));
    for (int i = 0; i < 4; ++i)
        ofHelper->InstallSwitch(switches.Get(i), swPorts[i]);
    ofHelper->CreateOpenFlowChannels();

    // ----------------------------------------------------------------
    // Host display annotations — name + node_type for topology viewer only
    // ----------------------------------------------------------------
    const std::array<std::string, 8> hostTypes = {
        "iot_sensor", "iot_sensor", "iot_sensor",
        "iot_gateway", "iot_gateway", "iot_gateway",
        "host", "host"
    };
    for (uint32_t i = 0; i < 8; ++i) {
        Ptr<NetDevice> nd = hosts.Get(i)->GetDevice(0);
        Mac48Address addr = Mac48Address::ConvertFrom(nd->GetAddress());
        uint64_t mac = MacToU64(addr);
        HostAnnotation ann;
        ann.name      = "H" + std::to_string(i);
        ann.node_type = hostTypes[i];
        ctrlApp->SetHostAnnotation(mac, ann);
    }

    // ----------------------------------------------------------------
    // Switch energy models  (dpid is 1-indexed: S0=1, S1=2, S2=3, S3=4)
    // Each switch starts with 10 kJ and consumes 1 nJ per forwarded byte.
    // ----------------------------------------------------------------
    ctrlApp->SetSwitchEnergyModel(1, 10000.0, 1e-9);
    ctrlApp->SetSwitchEnergyModel(2, 10000.0, 1e-9);
    ctrlApp->SetSwitchEnergyModel(3, 10000.0, 1e-9);
    ctrlApp->SetSwitchEnergyModel(4, 10000.0, 1e-9);

    // ----------------------------------------------------------------
    // Traffic — UDP OnOff between host pairs to generate real load
    // Start staggered to avoid convoy.  All hosts → H4 (server host)
    // Also some cross-traffic between pairs.
    // ----------------------------------------------------------------
    uint16_t basePort = 9000;
    ApplicationContainer sinkApps;
    ApplicationContainer srcApps;

    // Traffic sinks
    //   H4 (iot_gateway on S2) — primary server/sink for most flows
    //   H0 (iot_sensor  on S0) — sink for H6
    //   H2 (iot_sensor  on S1) — sink for H7
    // H4 is NOT the destination of H6 and H7.
    PacketSinkHelper sink("ns3::UdpSocketFactory",
                          InetSocketAddress(Ipv4Address::GetAny(), basePort));
    sinkApps.Add(sink.Install(hosts.Get(4)));  // H4: primary server sink

    PacketSinkHelper sink2("ns3::UdpSocketFactory",
                           InetSocketAddress(Ipv4Address::GetAny(), static_cast<uint16_t>(basePort + 1)));
    sinkApps.Add(sink2.Install(hosts.Get(0)));  // H0: receives from H6
    sinkApps.Add(sink2.Install(hosts.Get(2)));  // H2: receives from H7

    sinkApps.Start(Seconds(1.0));
    sinkApps.Stop(Seconds(simTime));

    // Traffic sources
    // Flows to H4 (the "server"): H1, H2, H3, H5
    // Cross-traffic (not to H4): H6->H0, H7->H2
    struct FlowSpec {
        uint32_t srcIdx;
        uint32_t dstIdx;
        uint16_t port;
        std::string rate;
        double startT;
    };

    std::vector<FlowSpec> flows = {
        // Flows converging on H4 (server switch S2)
        {1, 4, basePort,                              "5Mbps",  3.0},  // H1(S0) -> H4(S2)
        {2, 4, basePort,                              "8Mbps",  4.0},  // H2(S1) -> H4(S2)
        {3, 4, basePort,                              "3Mbps",  5.0},  // H3(S1) -> H4(S2)
        {5, 4, basePort,                              "10Mbps", 3.5},  // H5(S2) -> H4(S2) — same switch
        // Cross-ring flows (not destined for H4)
        {6, 0, static_cast<uint16_t>(basePort + 1),  "4Mbps",  4.5},  // H6(S3) -> H0(S0)
        {7, 2, static_cast<uint16_t>(basePort + 1),  "6Mbps",  5.5},  // H7(S3) -> H2(S1)
    };

    for (const auto& f : flows) {
        Ipv4Address dstAddr = hostIfaces.GetAddress(f.dstIdx);
        OnOffHelper onoff("ns3::UdpSocketFactory",
                          InetSocketAddress(dstAddr, f.port));
        onoff.SetConstantRate(DataRate(f.rate));
        onoff.SetAttribute("PacketSize", UintegerValue(1024));
        ApplicationContainer app = onoff.Install(hosts.Get(f.srcIdx));
        app.Start(Seconds(f.startT));
        app.Stop(Seconds(simTime - 1.0));
        srcApps.Add(app);
    }

    // ----------------------------------------------------------------
    // Traces (optional)
    // ----------------------------------------------------------------
    if (trace) {
        ofHelper->EnableOpenFlowPcap("of13-stats-test");
        ofHelper->EnableDatapathStats("of13-stats-test-dp");
    }

    // ----------------------------------------------------------------
    // Run
    // ----------------------------------------------------------------
    NS_LOG_INFO("stats-test: starting simulation (" << simTime << " s)");
    Simulator::Stop(Seconds(simTime));
    Simulator::Run();
    Simulator::Destroy();

    NS_LOG_INFO("stats-test: done — check scratch/data/state/sdn_state.json");
    return 0;
}
