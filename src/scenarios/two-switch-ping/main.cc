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

NS_LOG_COMPONENT_DEFINE("ZmqControllerMain");

int main(int argc, char* argv[])
{
    bool trace = false;
    double simTime = 1000;

    CommandLine cmd(__FILE__);
    cmd.AddValue("trace", "Enable pcap and datapath stats traces", trace);
    cmd.Parse(argc, argv);

    NS_LOG_INFO("Creating nodes...");

    NodeContainer hosts;
    hosts.Create(4);

    NodeContainer switches;
    switches.Create(2);

    NodeContainer controllers;
    controllers.Create(1);

    NS_LOG_INFO("Building topology...");

    CsmaHelper csma;
    csma.SetChannelAttribute("DataRate", StringValue("1Mbps"));
    csma.SetChannelAttribute("Delay", StringValue("1ms"));

    NetDeviceContainer swPorts[2];
    NetDeviceContainer hostPorts;
    NetDeviceContainer dev;

    // h0 to sw0
    dev = csma.Install(NodeContainer(hosts.Get(0), switches.Get(0)));
    hostPorts.Add(dev.Get(0));
    swPorts[0].Add(dev.Get(1));

    // h1 to sw0
    dev = csma.Install(NodeContainer(hosts.Get(1), switches.Get(0)));
    hostPorts.Add(dev.Get(0));
    swPorts[0].Add(dev.Get(1));

    // h2 to sw1
    dev = csma.Install(NodeContainer(hosts.Get(2), switches.Get(1)));
    hostPorts.Add(dev.Get(0));
    swPorts[1].Add(dev.Get(1));

    // h3 to sw1
    dev = csma.Install(NodeContainer(hosts.Get(3), switches.Get(1)));
    hostPorts.Add(dev.Get(0));
    swPorts[1].Add(dev.Get(1));

    // sw0 to sw1
    dev = csma.Install(NodeContainer(switches.Get(0), switches.Get(1)));
    swPorts[0].Add(dev.Get(0));
    swPorts[1].Add(dev.Get(1));

    // Install TCP/IP stack into hosts
    InternetStackHelper internet;
    internet.Install(hosts);

    Ipv4AddressHelper ipv4Helper;
    ipv4Helper.SetBase("10.1.1.0", "255.255.255.0");
    Ipv4InterfaceContainer hostIpIfaces = ipv4Helper.Assign(hostPorts);

    NS_LOG_INFO("Setting up OpenFlow13 domain...");

    Ptr<OFSwitch13InternalHelper> internal = CreateObject<OFSwitch13InternalHelper>();
    Ptr<ZmqOpenFlowController> controllerApp = CreateObject<ZmqOpenFlowController>();

    // Install the controller app on the controller node
    controllers.Get(0)->AddApplication(controllerApp);
    controllerApp->SetStartTime(Seconds(0));

    // Register the node as the OpenFlow controller
    internal->InstallController(controllers.Get(0));

    internal->InstallSwitch(switches.Get(0), swPorts[0]);
    internal->InstallSwitch(switches.Get(1), swPorts[1]);
    internal->CreateOpenFlowChannels();

    // NetAnim positions
    MobilityHelper mobility;
    mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    mobility.Install(hosts);
    mobility.Install(switches);
    mobility.Install(controllers);

    AnimationInterface anim("scratch/data/traces/netanim-zmq.xml");
    for (uint32_t i = 0; i < hosts.GetN(); i++)
    {
        auto host = hosts.Get(i);
        anim.UpdateNodeColor(host, 255, 255, 255);
        anim.SetConstantPosition(host, 5 + 10 * i, 30);
    }
    for (uint32_t i = 0; i < switches.GetN(); i++)
    {
        auto sw = switches.Get(i);
        anim.UpdateNodeColor(sw, 0, 255, 255);
        anim.SetConstantPosition(sw, 10 + 10 * i, 20);
    }
    for (uint32_t i = 0; i < controllers.GetN(); i++)
    {
        auto ctrl = controllers.Get(i);
        anim.UpdateNodeColor(ctrl, 255, 255, 0);
        anim.SetConstantPosition(ctrl, 15 + 5 * i, 10);
    }

    // Have all nodes seen by controller
    // ApplicationContainer discoveryApps;
    // for (uint32_t i = 0; i < hosts.GetN(); i++) {
    //     uint32_t dest = (i + 1) % hosts.GetN();
    //     PingHelper pingHelper(Ipv4Address(hostIpIfaces.GetAddress(dest)));
    //     pingHelper.SetAttribute("VerboseMode", EnumValue(Ping::QUIET));
    //     pingHelper.SetAttribute("Count", UintegerValue(1));
    //     discoveryApps.Add(pingHelper.Install(hosts.Get(i)));
    // }
    // discoveryApps.Start(Seconds(2.0));
    // discoveryApps.Stop(Seconds(5.0));

    // Actual ping test
    ApplicationContainer pingApps;
    for (uint32_t i = 1; i < hosts.GetN(); i++) {
        uint32_t dest = 0;
        PingHelper pingHelper(Ipv4Address(hostIpIfaces.GetAddress(dest)));
        pingHelper.SetAttribute("VerboseMode", EnumValue(Ping::QUIET));
        pingHelper.SetAttribute("Count", UintegerValue(0));
        pingApps.Add(pingHelper.Install(hosts.Get(i)));
    }
    pingApps.Start(Seconds(5.0));
    pingApps.Stop(Seconds(simTime - 1.0));

    if (trace)
    {
        internal->EnableOpenFlowPcap("of13");
        internal->EnableDatapathStats("of13-stats");
    }

    NS_LOG_INFO("Starting Simulation...");
    Simulator::Stop(Seconds(simTime));
    Simulator::Run();
    Simulator::Destroy();

    NS_LOG_INFO("Simulation Complete.");
    return 0;
}
