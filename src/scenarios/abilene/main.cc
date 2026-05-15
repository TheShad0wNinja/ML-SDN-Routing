#include <ns3/applications-module.h>
#include <ns3/core-module.h>
#include <ns3/csma-module.h>
#include <ns3/error-model.h>
#include <ns3/internet-apps-module.h>
#include <ns3/internet-module.h>
#include <ns3/network-module.h>
#include <ns3/ofswitch13-module.h>
#include <ns3/queue.h>

#include <vector>

#include "zmq-openflow-controller.h"

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("Abilene");

uint64_t g_totalRxBytes = 0;
uint64_t g_totalTxBytes = 0;
uint64_t g_pingTx = 0;
uint64_t g_pingRx = 0;
double   g_rttSumMs = 0.0;

void SinkRxCallback(Ptr<const Packet> p, const Address& addr) {
  g_totalRxBytes += p->GetSize();
}

void AppTxCallback(Ptr<const Packet> p) {
  g_totalTxBytes += p->GetSize();
}

void PingTxCallback(uint16_t /*seq*/, Ptr<Packet> /*p*/) {
  ++g_pingTx;
}

void PingRttCallback(uint16_t /*seq*/, Time rtt) {
  ++g_pingRx;
  g_rttSumMs += rtt.GetMilliSeconds();
}

static uint64_t MacToU64(const Mac48Address& addr) {
  uint8_t buf[6];
  addr.CopyTo(buf);
  uint64_t v = 0;
  for (int i = 0; i < 6; ++i) v = (v << 8) | buf[i];
  return v;
}

/* ------------------------------------------------------------------------- */
/*  Link stress helpers                                                      */
/* ------------------------------------------------------------------------- */

void ConfigureQueue(Ptr<NetDevice> nd, const std::string& sizeStr) {
  Ptr<CsmaNetDevice> csma = DynamicCast<CsmaNetDevice>(nd);
  if (!csma) return;
  Ptr<Queue<Packet>> q = csma->GetQueue();
  if (q) {
    q->SetAttribute("MaxSize", QueueSizeValue(QueueSize(sizeStr)));
  }
}

void SetLinkErrorRate(Ptr<NetDevice> nd, double rate) {
  Ptr<CsmaNetDevice> csma = DynamicCast<CsmaNetDevice>(nd);
  if (!csma) return;
  Ptr<RateErrorModel> em = CreateObject<RateErrorModel>();
  em->SetAttribute("ErrorRate", DoubleValue(rate));
  em->SetAttribute("ErrorUnit", EnumValue(RateErrorModel::ERROR_UNIT_PACKET));
  csma->SetAttribute("ReceiveErrorModel", PointerValue(em));
}

struct LinkState {
  Ptr<NetDevice> devA;
  Ptr<NetDevice> devB;
  double normalRate;
};

void BringLinkDown(LinkState* ls) {
  NS_LOG_INFO("Link DOWN at t=" << Simulator::Now().GetSeconds() << "s");
  SetLinkErrorRate(ls->devA, 1.0);
  SetLinkErrorRate(ls->devB, 1.0);
}

void BringLinkUp(LinkState* ls) {
  NS_LOG_INFO("Link UP at t=" << Simulator::Now().GetSeconds() << "s");
  SetLinkErrorRate(ls->devA, ls->normalRate);
  SetLinkErrorRate(ls->devB, ls->normalRate);
}

/* ------------------------------------------------------------------------- */
/*  Main                                                                     */
/* ------------------------------------------------------------------------- */

int main(int argc, char* argv[]) {
  bool trace = false;
  double simTime = 300;
  std::string trafficMode = "random";
  uint32_t seed = 12345;
  bool testMode = true;

  CommandLine cmd(__FILE__);
  cmd.AddValue("trace", "Enable pcap and datapath stats traces", trace);
  cmd.AddValue("simTime", "Simulation duration (s)", simTime);
  cmd.AddValue("trafficMode",
               "Traffic generation mode: random, central, grouped",
               trafficMode);
  cmd.AddValue("seed", "Random seed", seed);
  cmd.AddValue("test", "Run test in faster test mode", testMode);
  cmd.Parse(argc, argv);

  RngSeedManager::SetSeed(seed);

  NS_LOG_INFO("Creating nodes...");

  uint32_t numNodes = 11;

  NodeContainer hosts;
  hosts.Create(numNodes);

  NodeContainer switches;
  switches.Create(numNodes);

  NodeContainer controllers;
  controllers.Create(1);

  NS_LOG_INFO("Building topology...");

  /* --- Link helpers ------------------------------------------------------ */
  // Edge: 1 Gbps, 1 ms  (fat pipe so backbone is the bottleneck)
  CsmaHelper csmaEdge;
  csmaEdge.SetChannelAttribute("DataRate", StringValue("1Gbps"));
  csmaEdge.SetChannelAttribute("Delay", StringValue("1ms"));

  // Primary backbone: 100 Mbps, 10 ms
  CsmaHelper csmaBackbone;
  csmaBackbone.SetChannelAttribute("DataRate", StringValue("100Mbps"));
  csmaBackbone.SetChannelAttribute("Delay", StringValue("10ms"));

  // Parallel "dirty" links (lossy, shallow-buffered)
  CsmaHelper csmaDirty;      // 10 Mbps, 5 ms
  csmaDirty.SetChannelAttribute("DataRate", StringValue("10Mbps"));
  csmaDirty.SetChannelAttribute("Delay", StringValue("5ms"));

  CsmaHelper csmaCongested;  // 50 Mbps, 2 ms
  csmaCongested.SetChannelAttribute("DataRate", StringValue("50Mbps"));
  csmaCongested.SetChannelAttribute("Delay", StringValue("2ms"));

  CsmaHelper csmaLegacy;     // 10 Mbps, 5 ms
  csmaLegacy.SetChannelAttribute("DataRate", StringValue("10Mbps"));
  csmaLegacy.SetChannelAttribute("Delay", StringValue("5ms"));

  NetDeviceContainer swPorts[11];
  NetDeviceContainer hostPorts;
  NetDeviceContainer dev;

  /* --- Host attachments -------------------------------------------------- */
  for (uint32_t i = 0; i < numNodes; ++i) {
    dev = csmaEdge.Install(NodeContainer(hosts.Get(i), switches.Get(i)));
    hostPorts.Add(dev.Get(0));
    swPorts[i].Add(dev.Get(1));
    // Edge switch ports: moderate queue
    ConfigureQueue(dev.Get(1), "100p");
  }

  /* --- Backbone links ---------------------------------------------------- */
  struct Link { uint32_t src; uint32_t dst; };
  std::vector<Link> backboneLinks = {
    {0, 1}, {0, 3}, {1, 2}, {1, 3}, {2, 5},
    {3, 4}, {4, 5}, {4, 7}, {5, 8}, {6, 7},
    {6, 10}, {7, 8}, {8, 9}, {9, 10}
  };

  // Track specific links for scheduled failures
  LinkState ls34{0, 0, 0.0};
  LinkState ls78{0, 0, 0.0};
  LinkState ls45{0, 0, 0.0};

  for (const auto& l : backboneLinks) {
    dev = csmaBackbone.Install(
        NodeContainer(switches.Get(l.src), switches.Get(l.dst)));
    swPorts[l.src].Add(dev.Get(0));
    swPorts[l.dst].Add(dev.Get(1));

    // Deep buffers on primary backbone ports
    ConfigureQueue(dev.Get(0), "500p");
    ConfigureQueue(dev.Get(1), "500p");

    // Per-link loss rates (stress long-haul / critical paths)
    double lossRate = 0.0;
    if (l.src == 0 && l.dst == 3)       lossRate = 0.005;  // SEA-DEN
    else if (l.src == 3 && l.dst == 4)  lossRate = 0.008;  // DEN-MCI (critical)
    else if (l.src == 4 && l.dst == 7)  lossRate = 0.004;  // MCI-IND
    else if (l.src == 6 && l.dst == 10) lossRate = 0.006;  // ORD-JFK
    else if (l.src == 8 && l.dst == 9)  lossRate = 0.003;  // ATL-IAD
    else if (l.src == 9 && l.dst == 10) lossRate = 0.002;  // IAD-JFK

    if (lossRate > 0.0) {
      SetLinkErrorRate(dev.Get(0), lossRate);
      SetLinkErrorRate(dev.Get(1), lossRate);
    }

    // Remember devices for scheduled failures
    if (l.src == 3 && l.dst == 4) {
      ls34.devA = dev.Get(0); ls34.devB = dev.Get(1); ls34.normalRate = lossRate;
    } else if (l.src == 7 && l.dst == 8) {
      ls78.devA = dev.Get(0); ls78.devB = dev.Get(1); ls78.normalRate = lossRate;
    } else if (l.src == 4 && l.dst == 5) {
      ls45.devA = dev.Get(0); ls45.devB = dev.Get(1); ls45.normalRate = lossRate;
    }
  }

  /* --- Parallel links (multipath stress) --------------------------------- */
  // S3-S4 : dirty 10 Mbps, 3% loss, increased buffer
  dev = csmaDirty.Install(NodeContainer(switches.Get(3), switches.Get(4)));
  swPorts[3].Add(dev.Get(0));
  swPorts[4].Add(dev.Get(1));
  ConfigureQueue(dev.Get(0), "50p");
  ConfigureQueue(dev.Get(1), "50p");
  SetLinkErrorRate(dev.Get(0), 0.03);
  SetLinkErrorRate(dev.Get(1), 0.03);

  // S4-S7 : congested 50 Mbps, 1% loss
  dev = csmaCongested.Install(NodeContainer(switches.Get(4), switches.Get(7)));
  swPorts[4].Add(dev.Get(0));
  swPorts[7].Add(dev.Get(1));
  ConfigureQueue(dev.Get(0), "50p");
  ConfigureQueue(dev.Get(1), "50p");
  SetLinkErrorRate(dev.Get(0), 0.01);
  SetLinkErrorRate(dev.Get(1), 0.01);

  // S7-S8 : legacy 10 Mbps, 2% loss
  dev = csmaLegacy.Install(NodeContainer(switches.Get(7), switches.Get(8)));
  swPorts[7].Add(dev.Get(0));
  swPorts[8].Add(dev.Get(1));
  ConfigureQueue(dev.Get(0), "50p");
  ConfigureQueue(dev.Get(1), "50p");
  SetLinkErrorRate(dev.Get(0), 0.02);
  SetLinkErrorRate(dev.Get(1), 0.02);

  /* --- IP stack ---------------------------------------------------------- */
  InternetStackHelper internet;
  internet.Install(hosts);

  Ipv4AddressHelper ipv4Helper;
  ipv4Helper.SetBase("10.1.1.0", "255.255.255.0");
  Ipv4InterfaceContainer hostIpIfaces = ipv4Helper.Assign(hostPorts);

  /* --- OpenFlow domain --------------------------------------------------- */
  NS_LOG_INFO("Setting up OpenFlow13 domain...");

  Ptr<OFSwitch13InternalHelper> ofHelper =
      CreateObject<OFSwitch13InternalHelper>();
  Ptr<ZmqOpenFlowController> controllerApp =
      CreateObject<ZmqOpenFlowController>();

  controllers.Get(0)->AddApplication(controllerApp);
  controllerApp->SetStartTime(Seconds(0));

  ofHelper->InstallController(controllers.Get(0));

  for (uint32_t i = 0; i < numNodes; ++i) {
    ofHelper->InstallSwitch(switches.Get(i), swPorts[i]);
  }
  ofHelper->CreateOpenFlowChannels();

  /* --- Switch-level CPU / TCAM stress ------------------------------------ */
  for (uint32_t i = 0; i < numNodes; ++i) {
    Ptr<OFSwitch13Device> ofDev = switches.Get(i)->GetObject<OFSwitch13Device>();
    if (!ofDev) continue;

    double initialEnergyJ = 1e7;     // 10 MJ – measurable depletion per stats interval
    double energyPerByteJ = 0;

    if (i == 4) {                    // MCI – destination hub, must absorb all central traffic
      ofDev->SetAttribute("CpuCapacity", DataRateValue(DataRate("1Gbps")));
      ofDev->SetAttribute("TcamDelay", TimeValue(MicroSeconds(2)));
      energyPerByteJ = 5e-2;         // High-capacity hub: lower per-byte cost (50 mJ/B)
    } else if (i == 3 || i == 7) {   // DEN, IND – main transit hubs
      ofDev->SetAttribute("CpuCapacity", DataRateValue(DataRate("200Mbps")));
      ofDev->SetAttribute("TcamDelay", TimeValue(MicroSeconds(5)));
      energyPerByteJ = 8e-2;         // Mid-tier transit: moderate per-byte cost (80 mJ/B)
    } else if (i == 8 || i == 9) {   // ATL, IAD – south/east-coast transit
      ofDev->SetAttribute("CpuCapacity", DataRateValue(DataRate("100Mbps")));
      ofDev->SetAttribute("TcamDelay", TimeValue(MicroSeconds(8)));
      energyPerByteJ = 0.1;          // Lower capacity: higher per-byte cost (100 mJ/B)
    } else if (i == 5) {             // HOU – intentionally crippled, creates a measurably bad path
      ofDev->SetAttribute("CpuCapacity", DataRateValue(DataRate("1Mbps")));
      ofDev->SetAttribute("TcamDelay", TimeValue(MicroSeconds(100)));
      energyPerByteJ = 0.15;         // Crippled: highest per-byte cost (150 mJ/B)
    } else {                         // SEA, SNV, LAX, ORD, JFK – edge senders
      ofDev->SetAttribute("CpuCapacity", DataRateValue(DataRate("50Mbps")));
      ofDev->SetAttribute("TcamDelay", TimeValue(MicroSeconds(10)));
      energyPerByteJ = 0.1;          // Edge switches: moderate per-byte cost (100 mJ/B)
    }

    // Set energy model for this switch (DPID = switch index + 1 by convention)
    uint64_t dpid = i + 1;
    controllerApp->SetSwitchEnergyModel(dpid, initialEnergyJ, energyPerByteJ);
  }

  /* --- Host annotations -------------------------------------------------- */
  std::vector<std::string> names = {
    "SEA", "SNV", "LAX", "DEN", "MCI", "HOU",
    "ORD", "IND", "ATL", "IAD", "JFK"
  };

  for (uint32_t i = 0; i < numNodes; ++i) {
    Ptr<NetDevice> nd = hosts.Get(i)->GetDevice(0);
    Mac48Address addr = Mac48Address::ConvertFrom(nd->GetAddress());
    uint64_t mac = MacToU64(addr);
    HostAnnotation ann;
    ann.name = names[i] + "-Host";
    ann.node_type = "host";
    controllerApp->SetHostAnnotation(mac, ann);
  }

  /* --- Traffic generation ------------------------------------------------ */
  NS_LOG_INFO("Generating traffic (" << trafficMode << ")...");

  Ptr<UniformRandomVariable> uv = CreateObject<UniformRandomVariable>();
  ApplicationContainer srcApps;

  uint16_t port = 9000;

  // Packet sinks
  ApplicationContainer sinkApps;
  for (uint32_t i = 0; i < numNodes; ++i) {
    PacketSinkHelper sink("ns3::UdpSocketFactory",
                          InetSocketAddress(Ipv4Address::GetAny(), port));
    ApplicationContainer app = sink.Install(hosts.Get(i));
    app.Start(Seconds(1.0));
    app.Stop(Seconds(simTime));
    sinkApps.Add(app);
  }

  for (uint32_t i = 0; i < sinkApps.GetN(); ++i) {
    sinkApps.Get(i)->TraceConnectWithoutContext("Rx",
                                                MakeCallback(&SinkRxCallback));
  }

  double startTime = 5.0;

  // Continuous cross-country ping monitor
  ApplicationContainer pingApps;
  for (uint32_t srcId = 0; srcId < numNodes; ++srcId) {
    uint32_t dstId = (srcId + 5) % numNodes;
    PingHelper pingHelper(Ipv4Address(hostIpIfaces.GetAddress(dstId)));
    pingHelper.SetAttribute("VerboseMode", EnumValue(Ping::SILENT));
    pingHelper.SetAttribute("Count",       UintegerValue(0));
    pingHelper.SetAttribute("Interval",    TimeValue(Seconds(1.0)));
    pingApps.Add(pingHelper.Install(hosts.Get(srcId)));
  }
  pingApps.Start(Seconds(startTime));
  pingApps.Stop(Seconds(simTime - 1.0));
  for (uint32_t i = 0; i < pingApps.GetN(); ++i) {
    pingApps.Get(i)->TraceConnectWithoutContext("Tx",  MakeCallback(&PingTxCallback));
    pingApps.Get(i)->TraceConnectWithoutContext("Rtt", MakeCallback(&PingRttCallback));
  }

  // Rotating UDP load
  double flowDuration = 30.0;

  for (double t = startTime; t < simTime - 2.0; t += flowDuration) {
    for (uint32_t srcId = 0; srcId < numNodes; ++srcId) {
      uint32_t dstId = srcId;

      if (trafficMode == "central") {
        dstId = 4;  // MCI
      } else if (trafficMode == "random") {
        do {
          dstId = uv->GetInteger(0, numNodes - 1);
        } while (dstId == srcId);
      } else if (trafficMode == "grouped") {
        bool isWest = (srcId <= 3);
        bool isEast = (srcId >= 6);
        double roll = uv->GetValue();
        if (roll < 0.8) {
          if (isWest) {
            dstId = uv->GetInteger(6, 10);
          } else if (isEast) {
            dstId = uv->GetInteger(0, 3);
          } else {
            do { dstId = uv->GetInteger(0, numNodes - 1); }
            while (dstId == srcId);
          }
        } else {
          do { dstId = uv->GetInteger(0, numNodes - 1); }
          while (dstId == srcId);
        }
      }

      if (dstId == srcId) continue;


      uint32_t minRate = testMode ? 1 : 10;
      uint32_t maxRate = testMode ? 1 : 30;
      uint32_t rateInt = uv->GetInteger(minRate, maxRate);
      std::string rateStr = std::to_string(rateInt) + "Mbps";

      OnOffHelper onoff(
          "ns3::UdpSocketFactory",
          InetSocketAddress(hostIpIfaces.GetAddress(dstId), port));
      onoff.SetConstantRate(DataRate(rateStr));
      onoff.SetAttribute("PacketSize", UintegerValue(1024));

      ApplicationContainer app = onoff.Install(hosts.Get(srcId));
      double timeOffset = uv->GetValue(0.0, 0.5);
      app.Start(Seconds(t + timeOffset));
      app.Stop(Seconds(t + flowDuration));
      srcApps.Add(app);
    }
  }

  for (uint32_t i = 0; i < srcApps.GetN(); ++i) {
    srcApps.Get(i)->TraceConnectWithoutContext("Tx", MakeCallback(&AppTxCallback));
  }

  /* --- Scheduled topology churn ------------------------------------------ */
  Simulator::Schedule(Seconds(60.0),  &BringLinkDown, &ls34);
  Simulator::Schedule(Seconds(120.0), &BringLinkUp,  &ls34);
  Simulator::Schedule(Seconds(150.0), &BringLinkDown, &ls78);
  Simulator::Schedule(Seconds(210.0), &BringLinkUp,  &ls78);
  Simulator::Schedule(Seconds(240.0), &BringLinkDown, &ls45);
  Simulator::Schedule(Seconds(285.0), &BringLinkUp,  &ls45);

  /* --- Tracing ----------------------------------------------------------- */
  if (trace) {
    ofHelper->EnableOpenFlowPcap("abilene-stress");
    ofHelper->EnableDatapathStats("abilene-stress-stats");
  }

  /* --- Run --------------------------------------------------------------- */
  NS_LOG_INFO("Starting Simulation...");
  Simulator::Stop(Seconds(simTime));
  Simulator::Run();

  std::cout << "\n=== UDP Load Generator ===" << std::endl;
  std::cout << "  Transmitted : " << g_totalTxBytes << " B ("
            << g_totalTxBytes / 1e6 << " MB)" << std::endl;
  std::cout << "  Received    : " << g_totalRxBytes << " B ("
            << g_totalRxBytes / 1e6 << " MB)" << std::endl;
  if (g_totalTxBytes > 0) {
    std::cout << "  Delivery    : "
              << (g_totalRxBytes * 100.0 / g_totalTxBytes) << "%" << std::endl;
  }

  std::cout << "\n=== Performance Monitor (ping) ===" << std::endl;
  std::cout << "  Sent        : " << g_pingTx << std::endl;
  std::cout << "  Received    : " << g_pingRx << std::endl;
  if (g_pingTx > 0) {
    std::cout << "  Success     : " << (g_pingRx * 100.0 / g_pingTx) << "%" << std::endl;
    std::cout << "  Loss        : "
              << ((g_pingTx - g_pingRx) * 100.0 / g_pingTx) << "%" << std::endl;
  }
  if (g_pingRx > 0) {
    std::cout << "  Avg RTT     : " << (g_rttSumMs / g_pingRx) << " ms" << std::endl;
  }

  Simulator::Destroy();
  NS_LOG_INFO("Simulation Complete.");
  return 0;
}