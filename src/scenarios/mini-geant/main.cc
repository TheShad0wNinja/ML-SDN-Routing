#include "zmq-openflow-controller.h"
#include <ns3/core-module.h>
#include <ns3/csma-module.h>
#include <ns3/internet-apps-module.h>
#include <ns3/internet-module.h>
#include <ns3/mobility-module.h>
#include <ns3/netanim-module.h>
#include <ns3/network-module.h>
#include <ns3/ofswitch13-module.h>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("MiniGeantScene");

int main(int argc, char* argv[]) {
    bool trace = false;
    double simTime = 600; // 10 minutes

    CommandLine cmd(__FILE__);
    cmd.AddValue("trace", "Enable pcap and datapath stats traces", trace);
    cmd.AddValue("simTime", "Simulation time in seconds", simTime);
    cmd.Parse(argc, argv);

    NS_LOG_INFO("Creating nodes...");

    NodeContainer hosts;
    hosts.Create(4); // H0, H1, H2, H3

    NodeContainer switches;
    switches.Create(6); // S0, S1, S2, S3, S4, S5

    NodeContainer controllers;
    controllers.Create(1);

    NS_LOG_INFO("Configuring Link Helpers (Costs)...");

    // csmaCost10: 100Mbps
    CsmaHelper csmaCost10;
    csmaCost10.SetChannelAttribute("DataRate", StringValue("100Mbps"));

    // csmaCost20: 50Mbps
    CsmaHelper csmaCost20;
    csmaCost20.SetChannelAttribute("DataRate", StringValue("50Mbps"));

    // csmaCost50: 10Mbps
    CsmaHelper csmaCost50;
    csmaCost50.SetChannelAttribute("DataRate", StringValue("10Mbps"));

    // csmaEdge: 1Gbps
    CsmaHelper csmaEdge;
    csmaEdge.SetChannelAttribute("DataRate", StringValue("1Gbps"));

    NetDeviceContainer swPorts[6];
    NetDeviceContainer hostPorts;
    NetDeviceContainer dev;

    NS_LOG_INFO("Building Topology...");

    // Switch Connections (Mesh)
    // S0 to S1 (csmaCost10)
    dev = csmaCost10.Install(NodeContainer(switches.Get(0), switches.Get(1)));
    swPorts[0].Add(dev.Get(0));
    swPorts[1].Add(dev.Get(1));

    // S1 to S2 (csmaCost10)
    dev = csmaCost10.Install(NodeContainer(switches.Get(1), switches.Get(2)));
    swPorts[1].Add(dev.Get(0));
    swPorts[2].Add(dev.Get(1));

    // S1 to S4 (csmaCost20)
    dev = csmaCost20.Install(NodeContainer(switches.Get(1), switches.Get(4)));
    swPorts[1].Add(dev.Get(0));
    swPorts[4].Add(dev.Get(1));

    // S4 to S3 (csmaCost50)
    dev = csmaCost50.Install(NodeContainer(switches.Get(4), switches.Get(3)));
    swPorts[4].Add(dev.Get(0));
    swPorts[3].Add(dev.Get(1));

    // S2 to S5 (csmaCost20)
    dev = csmaCost20.Install(NodeContainer(switches.Get(2), switches.Get(5)));
    swPorts[2].Add(dev.Get(0));
    swPorts[5].Add(dev.Get(1));

    // S5 to S3 (csmaCost10)
    dev = csmaCost10.Install(NodeContainer(switches.Get(5), switches.Get(3)));
    swPorts[5].Add(dev.Get(0));
    swPorts[3].Add(dev.Get(1));

    // S1 to S5 (csmaCost20)
    dev = csmaCost20.Install(NodeContainer(switches.Get(1), switches.Get(5)));
    swPorts[1].Add(dev.Get(0));
    swPorts[5].Add(dev.Get(1));

    // S0 to S2 (csmaCost50)
    dev = csmaCost50.Install(NodeContainer(switches.Get(0), switches.Get(2)));
    swPorts[0].Add(dev.Get(0));
    swPorts[2].Add(dev.Get(1));

    // Host Attachments
    // H0 -> S0
    dev = csmaEdge.Install(NodeContainer(hosts.Get(0), switches.Get(0)));
    hostPorts.Add(dev.Get(0));
    swPorts[0].Add(dev.Get(1));

    // H1 -> S3
    dev = csmaEdge.Install(NodeContainer(hosts.Get(1), switches.Get(3)));
    hostPorts.Add(dev.Get(0));
    swPorts[3].Add(dev.Get(1));

    // H2 -> S1
    dev = csmaEdge.Install(NodeContainer(hosts.Get(2), switches.Get(1)));
    hostPorts.Add(dev.Get(0));
    swPorts[1].Add(dev.Get(1));

    // H3 -> S4
    dev = csmaEdge.Install(NodeContainer(hosts.Get(3), switches.Get(4)));
    hostPorts.Add(dev.Get(0));
    swPorts[4].Add(dev.Get(1));

    NS_LOG_INFO("Installing Internet Stack and IP Addressing...");
    InternetStackHelper internet;
    internet.Install(hosts);

    Ipv4AddressHelper ipv4Helper;
    ipv4Helper.SetBase("10.1.0.0", "255.255.0.0");
    Ipv4InterfaceContainer hostIpIfaces = ipv4Helper.Assign(hostPorts);

    NS_LOG_INFO("Setting up OpenFlow13 domain...");
    Ptr<OFSwitch13InternalHelper> internal = CreateObject<OFSwitch13InternalHelper>();
    Ptr<ZmqOpenFlowController> controllerApp = CreateObject<ZmqOpenFlowController>();

    controllers.Get(0)->AddApplication(controllerApp);
    controllerApp->SetStartTime(Seconds(0));
    internal->InstallController(controllers.Get(0));

    for (uint32_t i = 0; i < 6; ++i)
    {
        internal->InstallSwitch(switches.Get(i), swPorts[i]);
    }

    internal->CreateOpenFlowChannels();

    NS_LOG_INFO("Configuring Traffic (Ping)...");
    ApplicationContainer pingApps;

    for (uint32_t i = 1; i < hosts.GetN(); i++) {
        // uint32_t dest = (i + 1) % hosts.GetN();
        uint32_t dest = 0;
        PingHelper pingHelper(Ipv4Address(hostIpIfaces.GetAddress(dest)));
        pingHelper.SetAttribute("VerboseMode", EnumValue(Ping::QUIET));
        pingHelper.SetAttribute("Count", UintegerValue(0));
        pingHelper.SetAttribute("Interval", TimeValue(MilliSeconds(10)));
        pingHelper.SetAttribute("Size", UintegerValue(1024));
        pingApps.Add(pingHelper.Install(hosts.Get(i)));
    }
    pingApps.Start(Seconds(5.0));
    pingApps.Stop(Seconds(simTime - 1.0));

    NS_LOG_INFO("Starting simulation...");
    Simulator::Stop(Seconds(simTime));
    Simulator::Run();
    Simulator::Destroy();
    NS_LOG_INFO("Done.");

    return 0;
}

