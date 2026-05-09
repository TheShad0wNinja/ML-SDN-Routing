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

    // csmaCost10: 100Mbps, 2ms delay
    CsmaHelper csmaCost10;
    csmaCost10.SetChannelAttribute("DataRate", StringValue("100Mbps"));
    csmaCost10.SetChannelAttribute("Delay", StringValue("2ms"));

    // csmaCost20: 50Mbps, 10ms delay
    CsmaHelper csmaCost20;
    csmaCost20.SetChannelAttribute("DataRate", StringValue("50Mbps"));
    csmaCost20.SetChannelAttribute("Delay", StringValue("10ms"));

    // csmaCost50: 10Mbps, 50ms delay
    CsmaHelper csmaCost50;
    csmaCost50.SetChannelAttribute("DataRate", StringValue("10Mbps"));
    csmaCost50.SetChannelAttribute("Delay", StringValue("50ms"));

    // csmaEdge: 1Gbps, 1ms delay
    CsmaHelper csmaEdge;
    csmaEdge.SetChannelAttribute("DataRate", StringValue("1Gbps"));
    csmaEdge.SetChannelAttribute("Delay", StringValue("1ms"));

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

    // NS_LOG_INFO("Configuring Mobility and NetAnim...");
    // MobilityHelper mobility;
    // mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    // mobility.Install(hosts);
    // mobility.Install(switches);
    // mobility.Install(controllers);

    // Geographic-like coordinates
    // Switches
    // Ptr<ListPositionAllocator> positionAlloc = CreateObject<ListPositionAllocator>();
    // positionAlloc->Add(Vector(10.0, 30.0, 0.0)); // S0
    // positionAlloc->Add(Vector(30.0, 30.0, 0.0)); // S1
    // positionAlloc->Add(Vector(50.0, 30.0, 0.0)); // S2
    // positionAlloc->Add(Vector(70.0, 0.0, 0.0));  // S3
    // positionAlloc->Add(Vector(50.0, 10.0, 0.0)); // S4
    // positionAlloc->Add(Vector(70.0, 30.0, 0.0)); // S5

    // // Hosts
    // positionAlloc->Add(Vector(0.0, 30.0, 0.0));  // H0
    // positionAlloc->Add(Vector(80.0, 0.0, 0.0));  // H1
    // positionAlloc->Add(Vector(30.0, 40.0, 0.0)); // H2
    // positionAlloc->Add(Vector(50.0, 0.0, 0.0));  // H3
    
    // // Controller
    // positionAlloc->Add(Vector(40.0, 50.0, 0.0)); // C0

    // mobility.SetPositionAllocator(positionAlloc);
    // mobility.Install(hosts);
    // mobility.Install(switches);
    // mobility.Install(controllers);

    // AnimationInterface anim("mini-geant.xml");
    // Switch icons (optional but helpful)
    // for (uint32_t i = 0; i < 6; ++i) {
    //     anim.SetConstantPosition(switches.Get(i), positionAlloc->GetNext().x, positionAlloc->GetNext().y);
    // }
    // Re-doing it properly to match Node pointers or IDs if needed, 
    // but anim.SetConstantPosition is redundant if Mobility is installed correctly and updated.
    // However, for NetAnim, it's often better to just let it pick up from Mobility.

    // if (trace)
    // {
    //     internal->EnableOpenFlowPcap("mini-geant-of");
    //     csmaEdge.EnablePcapAll("mini-geant-host");
    // }

    // H0 to H1 ping
    // V4PingHelper pingH0H1(hostIpIfaces.GetAddress(1));
    // pingH0H1.SetAttribute("Verbose", BooleanValue(true));
    // ApplicationContainer apps = pingH0H1.Install(hosts.Get(0));
    // apps.Start(Seconds(1.0));
    // apps.Stop(Seconds(simTime));

    // // H2 to H3 ping
    // V4PingHelper pingH2H3(hostIpIfaces.GetAddress(3));
    // pingH2H3.SetAttribute("Verbose", BooleanValue(true));
    // apps = pingH2H3.Install(hosts.Get(2));
    // apps.Start(Seconds(2.0));
    // apps.Stop(Seconds(simTime));

    NS_LOG_INFO("Configuring Traffic (Ping)...");
    ApplicationContainer pingApps;

    for (uint32_t i = 0; i < hosts.GetN(); i++) {
        uint32_t dest = (i + 1) % hosts.GetN();
        PingHelper pingHelper(Ipv4Address(hostIpIfaces.GetAddress(dest)));
        pingHelper.SetAttribute("VerboseMode", EnumValue(Ping::QUIET));
        pingHelper.SetAttribute("Count", UintegerValue(0));
        pingApps.Add(pingHelper.Install(hosts.Get(i)));
    }
    pingApps.Start(Seconds(2.0));
    pingApps.Stop(Seconds(simTime - 1.0));

    
    // After initial discovery pings, have host 0 ping host 2
    // PingHelper pingH0H2(Ipv4Address(hostIpIfaces.GetAddress(2)));
    // pingH0H2.SetAttribute("VerboseMode", EnumValue(Ping::QUIET));
    // ApplicationContainer h0h2 = pingH0H2.Install(hosts.Get(0));
    // h0h2.Start(Seconds(5.0));
    // h0h2.Stop(Seconds(simTime - 1.0));


    // h0 to h2
    // PingHelper pingHelper(Ipv4Address(hostIpIfaces.GetAddress(2)));
    // pingHelper.SetAttribute("VerboseMode", EnumValue(Ping::QUIET));
    // pingApps.Add(pingHelper.Install(hosts.Get(0)));


    NS_LOG_INFO("Starting simulation...");
    Simulator::Stop(Seconds(simTime));
    Simulator::Run();
    Simulator::Destroy();
    NS_LOG_INFO("Done.");

    return 0;
}

