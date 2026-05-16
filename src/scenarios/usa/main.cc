#include <ns3/applications-module.h>
#include <ns3/core-module.h>
#include <ns3/csma-module.h>
#include <ns3/error-model.h>
#include <ns3/internet-apps-module.h>
#include <ns3/internet-module.h>
#include <ns3/network-module.h>
#include <ns3/ofswitch13-module.h>
#include <ns3/queue.h>

#include <iomanip>
#include <sstream>
#include <vector>

#include "zmq-openflow-controller.h"

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("USA");

/* ========================================================================= */
/*  1. STATS COLLECTOR  – centralized metrics                                 */
/* ========================================================================= */
class StatsCollector {
 public:
  static uint64_t g_totalRxBytes;
  static uint64_t g_totalTxBytes;
  static uint64_t g_pingTx;
  static uint64_t g_pingRx;
  static double g_rttSumMs;

  static void SinkRxCallback(Ptr<const Packet> p, const Address& addr) {
    g_totalRxBytes += p->GetSize();
  }
  static void AppTxCallback(Ptr<const Packet> p) {
    g_totalTxBytes += p->GetSize();
  }
  static void PingTxCallback(uint16_t /*seq*/, Ptr<Packet> /*p*/) {
    ++g_pingTx;
  }
  static void PingRttCallback(uint16_t /*seq*/, Time rtt) {
    ++g_pingRx;
    g_rttSumMs += rtt.GetMilliSeconds();
  }

  static void PrintReport() {
    std::cout << "\n=== UDP Load Generator ===" << std::endl;
    std::cout << "  Transmitted : " << g_totalTxBytes << " B ("
              << g_totalTxBytes / 1e6 << " MB)" << std::endl;
    std::cout << "  Received    : " << g_totalRxBytes << " B ("
              << g_totalRxBytes / 1e6 << " MB)" << std::endl;
    if (g_totalTxBytes > 0) {
      std::cout << "  Delivery    : "
                << (g_totalRxBytes * 100.0 / g_totalTxBytes) << "%"
                << std::endl;
    }
    std::cout << "\n=== Performance Monitor (ping) ===" << std::endl;
    std::cout << "  Sent        : " << g_pingTx << std::endl;
    std::cout << "  Received    : " << g_pingRx << std::endl;
    if (g_pingTx > 0) {
      std::cout << "  Success     : " << (g_pingRx * 100.0 / g_pingTx) << "%"
                << std::endl;
      std::cout << "  Loss        : "
                << ((g_pingTx - g_pingRx) * 100.0 / g_pingTx) << "%"
                << std::endl;
    }
    if (g_pingRx > 0) {
      std::cout << "  Avg RTT     : " << (g_rttSumMs / g_pingRx) << " ms"
                << std::endl;
    }
  }
};

uint64_t StatsCollector::g_totalRxBytes = 0;
uint64_t StatsCollector::g_totalTxBytes = 0;
uint64_t StatsCollector::g_pingTx = 0;
uint64_t StatsCollector::g_pingRx = 0;
double StatsCollector::g_rttSumMs = 0.0;

/* ========================================================================= */
/*  2. LINK CONTROLLER  – bring links up / down                             */
/* ========================================================================= */
class LinkController {
 public:
  struct State {
    Ptr<NetDevice> devA;
    Ptr<NetDevice> devB;
    double normalRate;
  };

  static void BringDown(State* ls) {
    NS_LOG_INFO("Link DOWN at t=" << Simulator::Now().GetSeconds() << "s");
    SetErrorRate(ls->devA, 1.0);
    SetErrorRate(ls->devB, 1.0);
  }
  static void BringUp(State* ls) {
    NS_LOG_INFO("Link UP at t=" << Simulator::Now().GetSeconds() << "s");
    SetErrorRate(ls->devA, ls->normalRate);
    SetErrorRate(ls->devB, ls->normalRate);
  }

 private:
  static void SetErrorRate(Ptr<NetDevice> nd, double rate) {
    Ptr<CsmaNetDevice> csma = DynamicCast<CsmaNetDevice>(nd);
    if (!csma) return;
    Ptr<RateErrorModel> em = CreateObject<RateErrorModel>();
    em->SetAttribute("ErrorRate", DoubleValue(rate));
    em->SetAttribute("ErrorUnit", EnumValue(RateErrorModel::ERROR_UNIT_PACKET));
    csma->SetAttribute("ReceiveErrorModel", PointerValue(em));
  }
};

/* ========================================================================= */
/*  3. CONFIGURATION STRUCTURES                                            */
/* ========================================================================= */
struct NodeProfile {
  std::string name;         // City / node name
  std::string nodeType;     // "tier1", "tier2", "edge", "crippled"
  std::string cpuCapacity;  // e.g. "1Gbps"
  uint32_t tcamDelayUs;     // Microseconds
  double energyPerByteJ;    // Joules / byte
  double initialEnergyJ;    // Starting energy reservoir
};

struct LinkSpec {
  uint32_t src;            // Source switch index
  uint32_t dst;            // Destination switch index
  double distanceKm;       // Geographic distance
  double lossRate;         // Packet error rate (0.0 = perfect)
  std::string bufferSize;  // Queue limit, e.g. "500p"
  bool failureTarget;      // true = eligible for scheduled churn
};

/* ========================================================================= */
/*  4. TOPOLOGY BUILDER  – nodes, links, OpenFlow, energy models             */
/* ========================================================================= */
class UsaTopologyBuilder {
 public:
  UsaTopologyBuilder() {
    m_edgeHelper.SetChannelAttribute("DataRate", StringValue("1Gbps"));
    m_edgeHelper.SetChannelAttribute("Delay", StringValue("1ms"));

    m_backboneHelper.SetChannelAttribute("DataRate", StringValue("10Gbps"));
  }

  void CreateNodes(uint32_t numNodes) {
    m_numNodes = numNodes;
    m_hosts.Create(numNodes);
    m_switches.Create(numNodes);
    m_controllers.Create(1);
    m_swPorts.resize(numNodes);
    m_linkStorage.reserve(16);
  }

  void InstallHost(uint32_t idx, const std::string& name,
                   Ptr<ZmqOpenFlowController> ctrl) {
    NetDeviceContainer dev = m_edgeHelper.Install(
        NodeContainer(m_hosts.Get(idx), m_switches.Get(idx)));
    m_hostPorts.Add(dev.Get(0));
    m_swPorts[idx].Add(dev.Get(1));
    ConfigureQueue(dev.Get(1), "100p");

    Ptr<NetDevice> nd = m_hosts.Get(idx)->GetDevice(0);
    Mac48Address addr = Mac48Address::ConvertFrom(nd->GetAddress());
    HostAnnotation ann;
    ann.name = name + "-Host";
    ann.node_type = "host";
    ctrl->SetHostAnnotation(MacToU64(addr), ann);
  }

  LinkController::State* AddBackboneLink(const LinkSpec& spec) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(6) << (spec.distanceKm * 5e-6)
        << "s";  // 5 µs / km (2/3 c)
    m_backboneHelper.SetChannelAttribute("Delay", StringValue(oss.str()));

    NetDeviceContainer dev = m_backboneHelper.Install(
        NodeContainer(m_switches.Get(spec.src), m_switches.Get(spec.dst)));
    m_swPorts[spec.src].Add(dev.Get(0));
    m_swPorts[spec.dst].Add(dev.Get(1));

    ConfigureQueue(dev.Get(0), spec.bufferSize);
    ConfigureQueue(dev.Get(1), spec.bufferSize);

    if (spec.lossRate > 0.0) {
      SetLinkErrorRate(dev.Get(0), spec.lossRate);
      SetLinkErrorRate(dev.Get(1), spec.lossRate);
    }

    if (spec.failureTarget) {
      LinkController::State st;
      st.devA = dev.Get(0);
      st.devB = dev.Get(1);
      st.normalRate = spec.lossRate;
      m_linkStorage.push_back(st);
      return &m_linkStorage.back();
    }
    return nullptr;
  }

  void ConfigureSwitch(uint32_t idx, const NodeProfile& profile,
                       Ptr<ZmqOpenFlowController> ctrl) {
    Ptr<OFSwitch13Device> ofDev =
        m_switches.Get(idx)->GetObject<OFSwitch13Device>();
    if (!ofDev) return;

    ofDev->SetAttribute("CpuCapacity",
                        DataRateValue(DataRate(profile.cpuCapacity)));
    ofDev->SetAttribute("TcamDelay",
                        TimeValue(MicroSeconds(profile.tcamDelayUs)));

    uint64_t dpid = idx + 1;
    ctrl->SetSwitchEnergyModel(dpid, profile.initialEnergyJ,
                               profile.energyPerByteJ);
  }

  void SetupIpStack() {
    InternetStackHelper internet;
    internet.Install(m_hosts);
    Ipv4AddressHelper ipv4;
    ipv4.SetBase("10.1.1.0", "255.255.255.0");
    m_hostIfaces = ipv4.Assign(m_hostPorts);
  }

  void InstallOpenFlow(Ptr<ZmqOpenFlowController> ctrlApp) {
    m_ofHelper = CreateObject<OFSwitch13InternalHelper>();
    m_controllers.Get(0)->AddApplication(ctrlApp);
    ctrlApp->SetStartTime(Seconds(0));
    m_ofHelper->InstallController(m_controllers.Get(0));

    for (uint32_t i = 0; i < m_numNodes; ++i) {
      m_ofHelper->InstallSwitch(m_switches.Get(i), m_swPorts[i]);
    }
    m_ofHelper->CreateOpenFlowChannels();
  }

  void EnableTraces(const std::string& prefix) {
    m_ofHelper->EnableOpenFlowPcap(prefix);
    m_ofHelper->EnableDatapathStats(prefix + "-stats");
  }

  // Accessors
  NodeContainer& GetHosts() { return m_hosts; }
  NodeContainer& GetSwitches() { return m_switches; }
  Ipv4InterfaceContainer& GetHostIfaces() { return m_hostIfaces; }

 private:
  uint32_t m_numNodes = 0;
  NodeContainer m_hosts;
  NodeContainer m_switches;
  NodeContainer m_controllers;
  NetDeviceContainer m_hostPorts;
  std::vector<NetDeviceContainer> m_swPorts;
  Ipv4InterfaceContainer m_hostIfaces;
  Ptr<OFSwitch13InternalHelper> m_ofHelper;
  CsmaHelper m_edgeHelper;
  CsmaHelper m_backboneHelper;
  std::vector<LinkController::State> m_linkStorage;

  static uint64_t MacToU64(const Mac48Address& addr) {
    uint8_t buf[6];
    addr.CopyTo(buf);
    uint64_t v = 0;
    for (int i = 0; i < 6; ++i) v = (v << 8) | buf[i];
    return v;
  }

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
};

/* ========================================================================= */
/*  5. TRAFFIC MANAGER  – sinks, pings, rotating UDP                        */
/* ========================================================================= */
class TrafficManager {
 public:
  TrafficManager(NodeContainer& hosts, Ipv4InterfaceContainer& ifaces)
      : m_hosts(hosts), m_ifaces(ifaces) {
    m_uv = CreateObject<UniformRandomVariable>();
  }

  void InstallSinks(uint16_t port, double simTime) {
    for (uint32_t i = 0; i < m_hosts.GetN(); ++i) {
      PacketSinkHelper sink("ns3::UdpSocketFactory",
                            InetSocketAddress(Ipv4Address::GetAny(), port));
      ApplicationContainer app = sink.Install(m_hosts.Get(i));
      app.Start(Seconds(1.0));
      app.Stop(Seconds(simTime));
      m_sinkApps.Add(app);
    }
    for (uint32_t i = 0; i < m_sinkApps.GetN(); ++i) {
      m_sinkApps.Get(i)->TraceConnectWithoutContext(
          "Rx", MakeCallback(&StatsCollector::SinkRxCallback));
    }
  }

  void InstallPings(double startTime, double simTime) {
    uint32_t n = m_hosts.GetN();
    for (uint32_t src = 0; src < n; ++src) {
      uint32_t dst = (src + n / 2) % n;  // opposite side of ring
      PingHelper ping(Ipv4Address(m_ifaces.GetAddress(dst)));
      ping.SetAttribute("VerboseMode", EnumValue(Ping::SILENT));
      ping.SetAttribute("Count", UintegerValue(0));
      ping.SetAttribute("Interval", TimeValue(Seconds(1.0)));
      m_pingApps.Add(ping.Install(m_hosts.Get(src)));
    }
    m_pingApps.Start(Seconds(startTime));
    m_pingApps.Stop(Seconds(simTime - 1.0));
    for (uint32_t i = 0; i < m_pingApps.GetN(); ++i) {
      m_pingApps.Get(i)->TraceConnectWithoutContext(
          "Tx", MakeCallback(&StatsCollector::PingTxCallback));
      m_pingApps.Get(i)->TraceConnectWithoutContext(
          "Rtt", MakeCallback(&StatsCollector::PingRttCallback));
    }
  }

  void InstallUdpLoad(double startTime, double simTime, double flowDuration,
                      const std::string& trafficMode, bool testMode) {
    uint32_t n = m_hosts.GetN();
    uint16_t port = 9000;

    for (double t = startTime; t < simTime - 2.0; t += flowDuration) {
      for (uint32_t src = 0; src < n; ++src) {
        uint32_t dst = PickDestination(src, n, trafficMode);
        if (dst == src) continue;

        uint32_t minRate = testMode ? 1 : 10;
        uint32_t maxRate = testMode ? 1 : 30;
        uint32_t rateInt = m_uv->GetInteger(minRate, maxRate);
        std::string rateStr = std::to_string(rateInt) + "Mbps";

        OnOffHelper onoff("ns3::UdpSocketFactory",
                          InetSocketAddress(m_ifaces.GetAddress(dst), port));
        onoff.SetConstantRate(DataRate(rateStr));
        onoff.SetAttribute("PacketSize", UintegerValue(1024));

        ApplicationContainer app = onoff.Install(m_hosts.Get(src));
        double offset = m_uv->GetValue(0.0, 0.5);
        app.Start(Seconds(t + offset));
        app.Stop(Seconds(t + flowDuration));
        m_srcApps.Add(app);
      }
    }
    for (uint32_t i = 0; i < m_srcApps.GetN(); ++i) {
      m_srcApps.Get(i)->TraceConnectWithoutContext(
          "Tx", MakeCallback(&StatsCollector::AppTxCallback));
    }
  }

 private:
  NodeContainer& m_hosts;
  Ipv4InterfaceContainer& m_ifaces;
  ApplicationContainer m_srcApps;
  ApplicationContainer m_sinkApps;
  ApplicationContainer m_pingApps;
  Ptr<UniformRandomVariable> m_uv;

  uint32_t PickDestination(uint32_t src, uint32_t n, const std::string& mode) {
    uint32_t dst = src;
    if (mode == "central") {
      dst = 15;  // Chicago
    } else if (mode == "random") {
      do {
        dst = m_uv->GetInteger(0, n - 1);
      } while (dst == src);
    } else if (mode == "grouped") {
      bool isWest = (src <= 10);  // Vancouver .. ElPaso
      bool isEast = (src >= 22);  // Cleveland .. Boston
      double roll = m_uv->GetValue();
      if (roll < 0.8) {
        if (isWest)
          dst = m_uv->GetInteger(22, n - 1);
        else if (isEast)
          dst = m_uv->GetInteger(0, 10);
        else
          do {
            dst = m_uv->GetInteger(0, n - 1);
          } while (dst == src);
      } else {
        do {
          dst = m_uv->GetInteger(0, n - 1);
        } while (dst == src);
      }
    }
    return dst;
  }
};

/* ========================================================================= */
/*  6. MAIN  – centralized config tables; edit here to change behavior       */
/* ========================================================================= */
int main(int argc, char* argv[]) {
  bool trace = false;
  double simTime = 300;
  std::string trafficMode = "random";
  uint32_t seed = 12345;
  bool testMode = true;

  bool mlEnabled = false;
  double mlIntervalS = 1.0;
  double mlActionScale = 0.15;
  double mlAlpha = 1.0;
  double mlBeta = 10.0;
  double mlGamma = 0.1;
  uint32_t mlCheckpointEveryNTicks = 60;
  bool mlResume = true;
  std::string mlEndpoint = "tcp://127.0.0.1:5555";

  CommandLine cmd(__FILE__);
  cmd.AddValue("trace", "Enable pcap and datapath stats traces", trace);
  cmd.AddValue("simTime", "Simulation duration (s)", simTime);
  cmd.AddValue("trafficMode", "Traffic: random, central, grouped", trafficMode);
  cmd.AddValue("seed", "Random seed", seed);
  cmd.AddValue("test", "Fast test mode", testMode);
  cmd.AddValue("ml", "Enable FDRL agent", mlEnabled);
  cmd.AddValue("mlIntervalS", "Agent period (s)", mlIntervalS);
  cmd.AddValue("mlActionScale", "Max |ΔW| fraction", mlActionScale);
  cmd.AddValue("mlAlpha", "Reward weight on delay", mlAlpha);
  cmd.AddValue("mlBeta", "Reward penalty on loss", mlBeta);
  cmd.AddValue("mlGamma", "Reward weight on energy", mlGamma);
  cmd.AddValue("mlCheckpointEveryNTicks", "Checkpoint cadence",
               mlCheckpointEveryNTicks);
  cmd.AddValue("mlResume", "Resume from checkpoint", mlResume);
  cmd.AddValue("mlEndpoint", "ZMQ endpoint", mlEndpoint);
  cmd.Parse(argc, argv);

  RngSeedManager::SetSeed(seed);

  /* --- 6a. NODE CONFIGURATION TABLE ------------------------------------ */
  // EDIT HERE: change cpuCapacity, tcamDelayUs, energyPerByteJ, etc.
  const uint32_t NUM_NODES = 34;
  std::vector<NodeProfile> nodeProfiles = {
      // { name,           type,       cpu,       tcam, e/B,   initE }
      {"Vancouver", "edge", "100Mbps", 10, 0.10, 1e7},
      {"Seattle", "tier1", "1Gbps", 2, 0.05, 1e7},
      {"Portland", "edge", "100Mbps", 10, 0.10, 1e7},
      {"Sunnyvale", "tier2", "500Mbps", 5, 0.08, 1e7},
      {"LosAngeles", "tier2", "500Mbps", 5, 0.08, 1e7},
      {"Missoula", "crippled", "1Mbps", 100, 0.15, 1e7},
      {"SaltLakeCity", "tier1", "1Gbps", 2, 0.05, 1e7},
      {"Phoenix", "edge", "100Mbps", 10, 0.10, 1e7},
      {"Denver", "tier2", "500Mbps", 5, 0.08, 1e7},
      {"Albuqerque", "edge", "100Mbps", 10, 0.10, 1e7},
      {"ElPaso", "tier2", "500Mbps", 5, 0.08, 1e7},
      {"Minneapolis", "edge", "100Mbps", 10, 0.10, 1e7},
      {"KansasCity", "tier2", "500Mbps", 5, 0.08, 1e7},
      {"Dallas", "edge", "100Mbps", 10, 0.10, 1e7},
      {"Houston", "tier1", "1Gbps", 2, 0.05, 1e7},
      {"Chicago", "tier1", "1Gbps", 2, 0.05, 1e7},
      {"Indianapolis", "edge", "100Mbps", 10, 0.10, 1e7},
      {"Louisville", "edge", "100Mbps", 10, 0.10, 1e7},
      {"Nashville", "tier2", "500Mbps", 5, 0.08, 1e7},
      {"Memphis", "edge", "100Mbps", 10, 0.10, 1e7},
      {"Jackson", "edge", "100Mbps", 10, 0.10, 1e7},
      {"BatonRouge", "edge", "100Mbps", 10, 0.10, 1e7},
      {"Cleveland", "tier2", "500Mbps", 5, 0.08, 1e7},
      {"Pittsburgh", "edge", "100Mbps", 10, 0.10, 1e7},
      {"Atlanta", "tier2", "500Mbps", 5, 0.08, 1e7},
      {"Jacksonville", "tier2", "500Mbps", 5, 0.08, 1e7},
      {"Buffalo", "edge", "100Mbps", 10, 0.10, 1e7},
      {"Ashburn", "edge", "100Mbps", 10, 0.10, 1e7},
      {"Raleigh", "edge", "100Mbps", 10, 0.10, 1e7},
      {"WashingtonDC", "tier2", "500Mbps", 5, 0.08, 1e7},
      {"Miami", "edge", "100Mbps", 10, 0.10, 1e7},
      {"Philadelphia", "edge", "100Mbps", 10, 0.10, 1e7},
      {"NewYork", "edge", "100Mbps", 10, 0.10, 1e7},
      {"Boston", "edge", "100Mbps", 10, 0.10, 1e7},
  };

  /* --- 6b. LINK CONFIGURATION TABLE ------------------------------------ */
  // EDIT HERE: change distanceKm, lossRate, bufferSize, or failureTarget
  std::vector<LinkSpec> linkSpecs = {
      // {src, dst, distance, loss,   buffer,  fail?}
      {4, 7, 575.0, 0.0, "500p", false},       // LA-Phoenix
      {7, 10, 557.0, 0.0, "500p", false},      // Phoenix-ElPaso
      {10, 9, 369.0, 0.0, "500p", false},      // ElPaso-Albuqerque
      {10, 14, 1087.0, 0.004, "500p", false},  // ElPaso-Houston
      {9, 8, 537.0, 0.0, "500p", false},       // Albuqerque-Denver
      {8, 12, 898.0, 0.002, "500p", true},     // Denver-KansasCity   **FAIL**
      {5, 11, 1617.0, 0.003, "500p", false},   // Missoula-Minneapolis
      {11, 15, 572.0, 0.0, "500p", false},     // Minneapolis-Chicago
      {14, 13, 362.0, 0.0, "500p", false},     // Houston-Dallas
      {13, 12, 729.0, 0.0, "500p", false},     // Dallas-KansasCity
      {12, 15, 662.0, 0.0, "500p", false},     // KansasCity-Chicago
      {15, 16, 263.0, 0.0, "500p", true},      // Chicago-Indianapolis **FAIL**
      {14, 21, 413.0, 0.0, "500p", false},     // Houston-BatonRouge
      {21, 25, 913.0, 0.002, "500p", false},   // BatonRouge-Jacksonville
      {25, 30, 525.0, 0.0, "500p", false},     // Jacksonville-Miami
      {25, 24, 458.0, 0.0, "500p", false},     // Jacksonville-Atlanta
      {24, 18, 346.0, 0.0, "500p", false},     // Atlanta-Nashville
      {24, 28, 572.0, 0.0, "500p", false},     // Atlanta-Raleigh
      {14, 20, 569.0, 0.0, "500p", false},     // Houston-Jackson
      {20, 19, 316.0, 0.0, "500p", false},     // Jackson-Memphis
      {19, 18, 306.0, 0.0, "500p", false},     // Memphis-Nashville
      {18, 17, 249.0, 0.0, "500p", false},     // Nashville-Louisville
      {17, 16, 172.0, 0.0, "500p", false},     // Louisville-Indianapolis
      {28, 29, 375.0, 0.0, "500p", false},     // Raleigh-WashingtonDC
      {29, 27, 55.0, 0.0, "500p", false},      // WashingtonDC-Ashburn
      {29, 31, 199.0, 0.0, "500p", false},     // WashingtonDC-Philadelphia
      {31, 32, 130.0, 0.0, "500p", false},     // Philadelphia-NewYork
      {15, 22, 497.0, 0.0, "500p", false},     // Chicago-Cleveland
      {22, 26, 279.0, 0.0, "500p", false},     // Cleveland-Buffalo
      {26, 33, 644.0, 0.0, "500p", false},     // Buffalo-Boston
      {22, 23, 185.0, 0.0, "500p", false},     // Cleveland-Pittsburgh
      {23, 27, 360.0, 0.0, "500p", false},     // Pittsburgh-Ashburn
      {32, 33, 306.0, 0.0, "500p", false},     // NewYork-Boston
      {2, 3, 907.0, 0.002, "500p", false},     // Portland-Sunnyvale
      {3, 4, 503.0, 0.0, "500p", false},       // Sunnyvale-LA
      {3, 6, 955.0, 0.003, "500p", false},     // Sunnyvale-SaltLakeCity
      {8, 6, 598.0, 0.0, "500p", false},       // Denver-SaltLakeCity
      {4, 6, 934.0, 0.003, "500p", false},     // LA-SaltLakeCity
      {0, 1, 194.0, 0.0, "500p", false},       // Vancouver-Seattle
      {1, 5, 635.0, 0.0, "500p", false},       // Seattle-Missoula
      {1, 2, 233.0, 0.0, "500p", false},       // Seattle-Portland
      {1, 6, 1128.0, 0.003, "500p", false},    // Seattle-SaltLakeCity
  };

  /* --- 6c. BUILD ------------------------------------------------------- */
  UsaTopologyBuilder builder;
  builder.CreateNodes(NUM_NODES);

  Ptr<ZmqOpenFlowController> ctrl = CreateObject<ZmqOpenFlowController>();
  {
    MlConfig mlCfg;
    mlCfg.enabled = mlEnabled;
    mlCfg.interval_s = mlIntervalS;
    mlCfg.action_scale = mlActionScale;
    mlCfg.reward_alpha = mlAlpha;
    mlCfg.reward_beta = mlBeta;
    mlCfg.reward_gamma = mlGamma;
    mlCfg.checkpoint_every_n_ticks = mlCheckpointEveryNTicks;
    mlCfg.resume = mlResume;
    mlCfg.seed = seed;
    mlCfg.endpoint = mlEndpoint;
    ctrl->SetMlConfig(mlCfg);
  }

  for (uint32_t i = 0; i < NUM_NODES; ++i) {
    builder.InstallHost(i, nodeProfiles[i].name, ctrl);
    builder.ConfigureSwitch(i, nodeProfiles[i], ctrl);
  }

  std::vector<LinkController::State*> failureLinks;
  for (const auto& spec : linkSpecs) {
    LinkController::State* ls = builder.AddBackboneLink(spec);
    if (ls) failureLinks.push_back(ls);
  }

  builder.SetupIpStack();
  builder.InstallOpenFlow(ctrl);

  /* --- 6d. TRAFFIC ----------------------------------------------------- */
  TrafficManager traffic(builder.GetHosts(), builder.GetHostIfaces());
  // traffic.InstallSinks(9000, simTime);
  traffic.InstallPings(5.0, simTime);
  // traffic.InstallUdpLoad(5.0, simTime, 30.0, trafficMode, testMode);

  /* --- 6e. SCHEDULED CHURN --------------------------------------------- */
  // EDIT HERE: change failure timing or add more events
  if (failureLinks.size() >= 3) {
    Simulator::Schedule(Seconds(60.0), &LinkController::BringDown,
                        failureLinks[0]);
    Simulator::Schedule(Seconds(120.0), &LinkController::BringUp,
                        failureLinks[0]);
    Simulator::Schedule(Seconds(150.0), &LinkController::BringDown,
                        failureLinks[1]);
    Simulator::Schedule(Seconds(210.0), &LinkController::BringUp,
                        failureLinks[1]);
    Simulator::Schedule(Seconds(240.0), &LinkController::BringDown,
                        failureLinks[2]);
    Simulator::Schedule(Seconds(285.0), &LinkController::BringUp,
                        failureLinks[2]);
  }

  /* --- 6f. RUN --------------------------------------------------------- */
  if (trace) {
    builder.EnableTraces("usa-stress");
  }

  NS_LOG_INFO("Starting Simulation...");
  Simulator::Stop(Seconds(simTime));
  Simulator::Run();
  StatsCollector::PrintReport();
  Simulator::Destroy();
  NS_LOG_INFO("Simulation Complete.");
  return 0;
}