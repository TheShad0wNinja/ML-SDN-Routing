#include <ns3/applications-module.h>
#include <ns3/core-module.h>
#include <ns3/csma-module.h>
#include <ns3/error-model.h>
#include <ns3/flow-monitor-module.h>
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
/*  1. STATS COLLECTOR  – ping-only counters; flow stats come from FlowMonitor
 */
/* ========================================================================= */
class StatsCollector {
 public:
  static uint64_t g_pingTx;
  static uint64_t g_pingRx;
  static double g_rttSumMs;

  static void PingTxCallback(uint16_t /*seq*/, Ptr<Packet> /*p*/) {
    ++g_pingTx;
  }
  static void PingRttCallback(uint16_t /*seq*/, Time rtt) {
    ++g_pingRx;
    g_rttSumMs += rtt.GetMilliSeconds();
  }

  static void PrintPingReport() {
    std::cout << "\n=== Liveness Probe (ping) ===" << std::endl;
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
  std::string name;
  std::string nodeType;
  std::string cpuCapacity;
  uint32_t tcamDelayUs;
  double energyPerByteJ;
  double initialEnergyJ;
};

struct LinkSpec {
  uint32_t src;
  uint32_t dst;
  double distanceKm;
  double lossRate;
  std::string bufferSize;
  bool failureTarget;
};

/* ========================================================================= */
/*  4. TOPOLOGY BUILDER                                                      */
/* ========================================================================= */
class UsaTopologyBuilder {
 public:
  UsaTopologyBuilder() {
    m_edgeHelper.SetChannelAttribute("DataRate", StringValue("100Mbps"));
    m_edgeHelper.SetChannelAttribute("Delay", StringValue("1ms"));
    m_backboneHelper.SetChannelAttribute("DataRate", StringValue("1Gbps"));
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
                   Ptr<ZmqOpenFlowController> ctrl,
                   const std::string& edgeQueueSize) {
    NetDeviceContainer dev = m_edgeHelper.Install(
        NodeContainer(m_hosts.Get(idx), m_switches.Get(idx)));
    m_hostPorts.Add(dev.Get(0));
    m_swPorts[idx].Add(dev.Get(1));
    ConfigureQueue(dev.Get(1), edgeQueueSize);

    Ptr<NetDevice> nd = m_hosts.Get(idx)->GetDevice(0);
    Mac48Address addr = Mac48Address::ConvertFrom(nd->GetAddress());
    HostAnnotation ann;
    ann.name = name + "-Host";
    ann.node_type = "host";
    ctrl->SetHostAnnotation(MacToU64(addr), ann);
  }

  LinkController::State* AddBackboneLink(const LinkSpec& spec,
                                         const std::string& backboneQueueSize) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(6) << (spec.distanceKm * 5e-6)
        << "s";
    m_backboneHelper.SetChannelAttribute("Delay", StringValue(oss.str()));

    NetDeviceContainer dev = m_backboneHelper.Install(
        NodeContainer(m_switches.Get(spec.src), m_switches.Get(spec.dst)));
    m_swPorts[spec.src].Add(dev.Get(0));
    m_swPorts[spec.dst].Add(dev.Get(1));

    ConfigureQueue(dev.Get(0), backboneQueueSize);
    ConfigureQueue(dev.Get(1), backboneQueueSize);

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
/*  5. TRAFFIC MANAGER                                                       */
/* ========================================================================= */
class TrafficManager {
 public:
  TrafficManager(NodeContainer& hosts, Ipv4InterfaceContainer& ifaces)
      : m_hosts(hosts), m_ifaces(ifaces) {
    m_uv = CreateObject<UniformRandomVariable>();
  }

  // Pre-warm controller flow tables by sending one ping between every host
  // pair, spread across [startTime, startTime + durationS]. This forces
  // Dijkstra + flow_mod for every path before measurement begins, so the
  // first measured second isn't dominated by reactive-install latency.
  void WarmupFlows(double startTime, double durationS) {
    uint32_t n = m_hosts.GetN();
    uint32_t totalPairs = n * (n - 1);
    if (totalPairs == 0 || durationS <= 0.0) return;
    double slotS = durationS / static_cast<double>(totalPairs);
    uint32_t idx = 0;
    for (uint32_t src = 0; src < n; ++src) {
      for (uint32_t dst = 0; dst < n; ++dst) {
        if (dst == src) continue;
        PingHelper ping(Ipv4Address(m_ifaces.GetAddress(dst)));
        ping.SetAttribute("VerboseMode", EnumValue(Ping::SILENT));
        ping.SetAttribute("Count", UintegerValue(1));
        ApplicationContainer app = ping.Install(m_hosts.Get(src));
        double t = startTime + idx * slotS;
        app.Start(Seconds(t));
        app.Stop(Seconds(t + 1.0));
        m_warmupApps.Add(app);
        ++idx;
      }
    }
  }

  void InstallPings(double startTime, double simTime) {
    uint32_t n = m_hosts.GetN();
    for (uint32_t src = 0; src < n; ++src) {
      uint32_t dst = (src + n / 2) % n;
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
                      const std::string& trafficMode, uint32_t maxRateMbps) {
    uint32_t n = m_hosts.GetN();
    uint16_t port = 9000;

    // Install one sink per host so UDP traffic has a receiver
    for (uint32_t i = 0; i < n; ++i) {
      PacketSinkHelper sink("ns3::TcpSocketFactory",
                            InetSocketAddress(Ipv4Address::GetAny(), port));
      ApplicationContainer app = sink.Install(m_hosts.Get(i));
      app.Start(Seconds(startTime - 0.5));
      app.Stop(Seconds(simTime));
      m_sinkApps.Add(app);
    }

    // Overlap flows: new wave every flowDuration/2, not flowDuration
    double waveStride = flowDuration * 0.5;
    for (double t = startTime; t < simTime - 2.0; t += waveStride) {
      for (uint32_t src = 0; src < n; ++src) {
        uint32_t dst = PickDestination(src, n, trafficMode);
        if (dst == src) continue;

        uint32_t rateInt =
            m_uv->GetInteger(1, std::max<uint32_t>(1, maxRateMbps));
        std::string rateStr = std::to_string(rateInt) + "Mbps";

        OnOffHelper onoff("ns3::TcpSocketFactory",
                          InetSocketAddress(m_ifaces.GetAddress(dst), port));
        onoff.SetConstantRate(DataRate(rateStr));
        onoff.SetAttribute("PacketSize", UintegerValue(1024));

        ApplicationContainer app = onoff.Install(m_hosts.Get(src));
        // Spread starts across 5 s, not 0.5 s, to avoid synchronized bursts
        double offset = m_uv->GetValue(0.0, 5.0);
        app.Start(Seconds(t + offset));
        app.Stop(Seconds(t + flowDuration));
        m_srcApps.Add(app);
      }
    }
  }

 private:
  NodeContainer& m_hosts;
  Ipv4InterfaceContainer& m_ifaces;
  ApplicationContainer m_warmupApps;
  ApplicationContainer m_srcApps;
  ApplicationContainer m_sinkApps;
  ApplicationContainer m_pingApps;
  Ptr<UniformRandomVariable> m_uv;

  uint32_t PickDestination(uint32_t src, uint32_t n, const std::string& mode) {
    uint32_t dst = src;
    if (mode == "central") {
      dst = 15;
    } else if (mode == "random") {
      do {
        dst = m_uv->GetInteger(0, n - 1);
      } while (dst == src);
    } else if (mode == "grouped") {
      bool isWest = (src <= 10);
      bool isEast = (src >= 22);
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
/*  6. MAIN                                                                   */
/* ========================================================================= */
int main(int argc, char* argv[]) {
  bool trace = false;
  double simTime = 60.0;
  double warmupS = 10.0;
  std::string trafficMode = "random";
  uint32_t seed = 12345;

  bool pingEnabled = true;
  bool udpEnabled = false;
  bool failuresEnabled = false;
  bool crippleEnabled = false;
  uint32_t udpRateCap = 5;  // Mbps per flow cap
  double udpFlowDuration = 30.0;
  std::string backboneQueue = "2000p";
  std::string edgeQueue = "200p";

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
  cmd.AddValue("warmupS", "Pre-warmup window for flow installs (s)", warmupS);
  cmd.AddValue("trafficMode", "Traffic: random, central, grouped", trafficMode);
  cmd.AddValue("seed", "Random seed", seed);
  cmd.AddValue("ping", "Enable measurement pings", pingEnabled);
  cmd.AddValue("udp", "Enable OnOff UDP background load", udpEnabled);
  cmd.AddValue("failures", "Enable scheduled link churn", failuresEnabled);
  cmd.AddValue("cripple", "Cripple Missoula node (1Mbps CPU, 100us TCAM)",
               crippleEnabled);
  cmd.AddValue("udpRateCap", "Max per-flow UDP rate in Mbps", udpRateCap);
  cmd.AddValue("udpFlowDuration", "UDP flow length (s)", udpFlowDuration);
  cmd.AddValue("backboneQueue", "Backbone CSMA queue size", backboneQueue);
  cmd.AddValue("edgeQueue", "Edge CSMA queue size", edgeQueue);
  cmd.AddValue("ml", "Enable FDRL agent", mlEnabled);
  cmd.AddValue("mlIntervalS", "Agent period (s)", mlIntervalS);
  cmd.AddValue("mlActionScale", "Max |dW| fraction", mlActionScale);
  cmd.AddValue("mlAlpha", "Reward weight on delay", mlAlpha);
  cmd.AddValue("mlBeta", "Reward penalty on loss", mlBeta);
  cmd.AddValue("mlGamma", "Reward weight on energy", mlGamma);
  cmd.AddValue("mlCheckpointEveryNTicks", "Checkpoint cadence",
               mlCheckpointEveryNTicks);
  cmd.AddValue("mlResume", "Resume from checkpoint", mlResume);
  cmd.AddValue("mlEndpoint", "ZMQ endpoint", mlEndpoint);
  cmd.Parse(argc, argv);

  RngSeedManager::SetSeed(seed);

  if (warmupS < 0.0) warmupS = 0.0;
  if (warmupS > simTime - 5.0) warmupS = std::max(0.0, simTime - 5.0);

  /* --- 6a. NODE CONFIGURATION TABLE ------------------------------------ */
  const uint32_t NUM_NODES = 34;
  std::vector<NodeProfile> nodeProfiles = {
      {"Vancouver", "edge", "100Mbps", 10, 0.10, 1e7},
      {"Seattle", "tier1", "1Gbps", 2, 0.05, 5e7},
      {"Portland", "edge", "100Mbps", 10, 0.10, 1e7},
      {"Sunnyvale", "tier2", "500Mbps", 5, 0.08, 2e7},
      {"LosAngeles", "tier2", "500Mbps", 5, 0.08, 2e7},
      {"Missoula", "crippled", "1Mbps", 100, 0.15, 5e6},
      {"SaltLakeCity", "tier1", "1Gbps", 2, 0.05, 5e7},
      {"Phoenix", "edge", "100Mbps", 10, 0.10, 1e7},
      {"Denver", "tier2", "500Mbps", 5, 0.08, 2e7},
      {"Albuqerque", "edge", "100Mbps", 10, 0.10, 1e7},
      {"ElPaso", "tier2", "500Mbps", 5, 0.08, 2e7},
      {"Minneapolis", "edge", "100Mbps", 10, 0.10, 1e7},
      {"KansasCity", "tier2", "500Mbps", 5, 0.08, 2e7},
      {"Dallas", "edge", "100Mbps", 10, 0.10, 1e7},
      {"Houston", "tier1", "1Gbps", 2, 0.05, 5e7},
      {"Chicago", "tier1", "1Gbps", 2, 0.05, 5e7},
      {"Indianapolis", "edge", "100Mbps", 10, 0.10, 1e7},
      {"Louisville", "edge", "100Mbps", 10, 0.10, 1e7},
      {"Nashville", "tier2", "500Mbps", 5, 0.08, 2e7},
      {"Memphis", "edge", "100Mbps", 10, 0.10, 1e7},
      {"Jackson", "edge", "100Mbps", 10, 0.10, 1e7},
      {"BatonRouge", "edge", "100Mbps", 10, 0.10, 1e7},
      {"Cleveland", "tier2", "500Mbps", 5, 0.08, 2e7},
      {"Pittsburgh", "edge", "100Mbps", 10, 0.10, 1e7},
      {"Atlanta", "tier2", "500Mbps", 5, 0.08, 2e7},
      {"Jacksonville", "tier2", "500Mbps", 5, 0.08, 2e7},
      {"Buffalo", "edge", "100Mbps", 10, 0.10, 1e7},
      {"Ashburn", "edge", "100Mbps", 10, 0.10, 1e7},
      {"Raleigh", "edge", "100Mbps", 10, 0.10, 1e7},
      {"WashingtonDC", "tier2", "500Mbps", 5, 0.08, 2e7},
      {"Miami", "edge", "100Mbps", 10, 0.10, 1e7},
      {"Philadelphia", "edge", "100Mbps", 10, 0.10, 1e7},
      {"NewYork", "edge", "100Mbps", 10, 0.10, 1e7},
      {"Boston", "edge", "100Mbps", 10, 0.10, 1e7},
  };

  // Decripple Missoula unless explicitly requested. Crippled node adds
  // 100us TCAM + 1Mbps CPU; useful for failure-mode tests, noise for routing
  // comparisons.
  if (!crippleEnabled) {
    nodeProfiles[5] = {"Missoula", "tier2", "500Mbps", 5, 0.08, 2e7};
  }

  /* --- 6b. LINK CONFIGURATION TABLE ------------------------------------ */
  std::vector<LinkSpec> linkSpecs = {
      {4, 7, 575.0, 0.0, backboneQueue, false},
      {7, 10, 557.0, 0.0, backboneQueue, false},
      {10, 9, 369.0, 0.0, backboneQueue, false},
      {10, 14, 1087.0, 0.004, backboneQueue, false},
      {9, 8, 537.0, 0.0, backboneQueue, false},
      {8, 12, 898.0, 0.002, backboneQueue, true},
      {5, 11, 1617.0, 0.003, backboneQueue, false},
      {11, 15, 572.0, 0.0, backboneQueue, false},
      {14, 13, 362.0, 0.0, backboneQueue, false},
      {13, 12, 729.0, 0.0, backboneQueue, false},
      {12, 15, 662.0, 0.0, backboneQueue, false},
      {15, 16, 263.0, 0.0, backboneQueue, true},
      {14, 21, 413.0, 0.0, backboneQueue, false},
      {21, 25, 913.0, 0.002, backboneQueue, false},
      {25, 30, 525.0, 0.0, backboneQueue, false},
      {25, 24, 458.0, 0.0, backboneQueue, false},
      {24, 18, 346.0, 0.0, backboneQueue, false},
      {24, 28, 572.0, 0.0, backboneQueue, false},
      {14, 20, 569.0, 0.0, backboneQueue, false},
      {20, 19, 316.0, 0.0, backboneQueue, false},
      {19, 18, 306.0, 0.0, backboneQueue, false},
      {18, 17, 249.0, 0.0, backboneQueue, false},
      {17, 16, 172.0, 0.0, backboneQueue, false},
      {28, 29, 375.0, 0.0, backboneQueue, false},
      {29, 27, 55.0, 0.0, backboneQueue, false},
      {29, 31, 199.0, 0.0, backboneQueue, false},
      {31, 32, 130.0, 0.0, backboneQueue, false},
      {15, 22, 497.0, 0.0, backboneQueue, false},
      {22, 26, 279.0, 0.0, backboneQueue, false},
      {26, 33, 644.0, 0.0, backboneQueue, false},
      {22, 23, 185.0, 0.0, backboneQueue, false},
      {23, 27, 360.0, 0.0, backboneQueue, false},
      {32, 33, 306.0, 0.0, backboneQueue, false},
      {2, 3, 907.0, 0.002, backboneQueue, false},
      {3, 4, 503.0, 0.0, backboneQueue, false},
      {3, 6, 955.0, 0.003, backboneQueue, false},
      {8, 6, 598.0, 0.0, backboneQueue, false},
      {4, 6, 934.0, 0.003, backboneQueue, false},
      {0, 1, 194.0, 0.0, backboneQueue, false},
      {1, 5, 635.0, 0.0, backboneQueue, false},
      {1, 2, 233.0, 0.0, backboneQueue, false},
      {1, 6, 1128.0, 0.003, backboneQueue, false},
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
    builder.InstallHost(i, nodeProfiles[i].name, ctrl, edgeQueue);
  }

  std::vector<LinkController::State*> failureLinks;
  for (const auto& spec : linkSpecs) {
    LinkController::State* ls = builder.AddBackboneLink(spec, spec.bufferSize);
    if (ls) failureLinks.push_back(ls);
  }

  builder.SetupIpStack();
  builder.InstallOpenFlow(ctrl);

  // ConfigureSwitch sets attributes on OFSwitch13Device, which only exists
  // after InstallOpenFlow has run. Running it earlier silently no-ops the
  // CPU/TCAM/energy assignments.
  for (uint32_t i = 0; i < NUM_NODES; ++i) {
    builder.ConfigureSwitch(i, nodeProfiles[i], ctrl);
  }

  /* --- 6d. TRAFFIC ----------------------------------------------------- */
  TrafficManager traffic(builder.GetHosts(), builder.GetHostIfaces());

  // Warmup runs from t=1.0 to t=1.0+warmupS so the controller can install
  // every host-pair flow before measurement begins.
  double measureStart = 1.0 + warmupS;
  if (warmupS > 0.0) {
    traffic.WarmupFlows(1.0, warmupS);
  }

  if (pingEnabled) {
    traffic.InstallPings(measureStart, simTime);
  }
  if (udpEnabled) {
    traffic.InstallUdpLoad(measureStart, simTime, udpFlowDuration, trafficMode,
                           udpRateCap);
  }

  /* --- 6e. SCHEDULED CHURN --------------------------------------------- */
  // Scale failure events to fractions of the measurement window so the
  // schedule still makes sense at simTime=60 or simTime=300.
  if (failuresEnabled && failureLinks.size() >= 3) {
    double window = simTime - measureStart;
    auto at = [&](double frac) {
      return Seconds(measureStart + frac * window);
    };
    Simulator::Schedule(at(0.20), &LinkController::BringDown, failureLinks[0]);
    Simulator::Schedule(at(0.40), &LinkController::BringUp, failureLinks[0]);
    Simulator::Schedule(at(0.50), &LinkController::BringDown, failureLinks[1]);
    Simulator::Schedule(at(0.70), &LinkController::BringUp, failureLinks[1]);
    Simulator::Schedule(at(0.80), &LinkController::BringDown, failureLinks[2]);
    Simulator::Schedule(at(0.95), &LinkController::BringUp, failureLinks[2]);
  }

  /* --- 6f. FLOW MONITOR ------------------------------------------------ */
  FlowMonitorHelper flowmonHelper;
  Ptr<FlowMonitor> monitor = flowmonHelper.InstallAll();
  // Discard anything that flew during warmup so the report reflects only
  // the measurement window.
  Simulator::Schedule(Seconds(measureStart),
                      [&]() { monitor->ResetAllStats(); });

  /* --- 6g. RUN --------------------------------------------------------- */
  if (trace) {
    builder.EnableTraces("usa-stress");
  }

  NS_LOG_INFO("Starting Simulation (simTime=" << simTime << "s, warmup="
                                              << warmupS << "s)...");
  Simulator::Stop(Seconds(simTime));
  Simulator::Run();

  /* --- 6h. REPORT ------------------------------------------------------ */
  StatsCollector::PrintPingReport();

  monitor->CheckForLostPackets();
  auto classifier =
      DynamicCast<Ipv4FlowClassifier>(flowmonHelper.GetClassifier());
  auto stats = monitor->GetFlowStats();

  uint64_t totalTx = 0, totalRx = 0, totalLost = 0;
  double delaySumS = 0.0;
  uint64_t rxForDelay = 0;
  for (auto& kv : stats) {
    totalTx += kv.second.txPackets;
    totalRx += kv.second.rxPackets;
    totalLost += kv.second.lostPackets;
    delaySumS += kv.second.delaySum.GetSeconds();
    rxForDelay += kv.second.rxPackets;
  }

  std::cout << "\n=== FlowMonitor Summary (post-warmup window) ==="
            << std::endl;
  std::cout << "  Flows       : " << stats.size() << std::endl;
  std::cout << "  Tx packets  : " << totalTx << std::endl;
  std::cout << "  Rx packets  : " << totalRx << std::endl;
  std::cout << "  Lost packets: " << totalLost << std::endl;
  if (totalTx > 0) {
    std::cout << "  Delivery    : " << (totalRx * 100.0 / totalTx) << "%"
              << std::endl;
  }
  if (rxForDelay > 0) {
    std::cout << "  Avg delay   : " << (delaySumS * 1000.0 / rxForDelay)
              << " ms" << std::endl;
  }

  /* --- 6i. ENERGY REPORT ---------------------------------------------- */
  // Power averaged over the full sim run. Useful for comparing routing
  // policies (e.g. plain Dijkstra vs FDRL agent) under identical traffic.
  {
    double totalInitialJ = 0.0;
    double totalResidualJ = 0.0;
    uint32_t tracked = 0;
    std::cout << "\n=== Switch Energy (consumed over " << simTime
              << "s) ===" << std::endl;
    std::cout << std::left << std::setw(14) << "Switch" << std::right
              << std::setw(14) << "Consumed (J)" << std::setw(14)
              << "Avg Power (W)" << std::endl;
    for (uint32_t i = 0; i < NUM_NODES; ++i) {
      uint64_t dpid = i + 1;
      double init = ctrl->GetSwitchInitialEnergyJ(dpid);
      double resid = ctrl->GetSwitchResidualEnergyJ(dpid);
      if (init < 0 || resid < 0) continue;
      double consumed = init - resid;
      double avgW = (simTime > 0) ? consumed / simTime : 0.0;
      totalInitialJ += init;
      totalResidualJ += resid;
      ++tracked;
      std::cout << std::left << std::setw(14) << nodeProfiles[i].name
                << std::right << std::setw(14) << std::fixed
                << std::setprecision(2) << consumed << std::setw(14) << avgW
                << std::endl;
    }
    if (tracked > 0 && simTime > 0) {
      double totalConsumed = totalInitialJ - totalResidualJ;
      std::cout << "  Switches tracked  : " << tracked << std::endl;
      std::cout << "  Total consumed    : " << totalConsumed << " J"
                << std::endl;
      std::cout << "  Total avg power   : " << (totalConsumed / simTime) << " W"
                << std::endl;
      std::cout << "  Per-switch avg    : "
                << (totalConsumed / simTime / tracked) << " W" << std::endl;
    } else {
      std::cout << "  (no energy model configured)" << std::endl;
    }
  }

  Simulator::Destroy();
  NS_LOG_INFO("Simulation Complete.");
  return 0;
}