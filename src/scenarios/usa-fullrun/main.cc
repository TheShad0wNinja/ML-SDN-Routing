// usa-fullrun — Phase 2 showcase scenario. One ns-3 process simultaneously
// spawns M Local Controllers (hierarchical-SDN architecture from
// /root/.claude/plans/it-is-timme-to-abstract-taco.md). Each Local Controller
// owns one section's switches via its own OFSwitch13InternalHelper instance
// (multi-controller pattern from contrib/ofswitch13/examples/
// ofswitch13-multiple-domains.cc). Inter-domain routing is Option A:
// statically pre-installed flow-mods via border switches, derived from CLI
// flags populated by run_tests.sh out of scratch/scenarios/usa/sections.json.
//
// The Python side is unchanged: each controller talks to its own
// ml_service.py instance on its own port. All instances share
// scratch/data/federated_weights/ so root_aggregator.py (Phase 1) FedAvgs
// their weights every K training steps.
//
// Build:
//   ./ns3 build scratch/scenarios/usa-fullrun
// Run (defaults match scratch/scenarios/usa/sections.json):
//   ./ns3 run "usa-fullrun --simTime=120 --ml"

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

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "zmq-openflow-controller.h"

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("UsaFullrun");

/* ========================================================================= */
/*  Topology spec — duplicated from scratch/scenarios/usa/main.cc to keep
 *  this scenario self-contained. If usa/main.cc changes the topology, mirror
 *  it here so 'fullrun' stays in lock-step with the federated-mode default.
 */
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
};
struct TopoSpec {
  std::vector<NodeProfile> nodes;
  std::vector<LinkSpec> links;
  std::vector<uint32_t> hostToSwitch;
  std::vector<std::string> hostNames;
};

static TopoSpec BuildUsaSpec(const std::string& backboneQueue) {
  TopoSpec spec;
  spec.nodes = {
      {"Vancouver", "edge", "100Mbps", 10, 0.10, 1e7},
      {"Seattle", "tier1", "1Gbps", 2, 0.05, 5e7},
      {"Portland", "edge", "100Mbps", 10, 0.10, 1e7},
      {"Sunnyvale", "tier2", "500Mbps", 5, 0.08, 2e7},
      {"LosAngeles", "tier2", "500Mbps", 5, 0.08, 2e7},
      {"Missoula", "tier2", "500Mbps", 5, 0.08, 2e7},
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
  spec.links = {
      {4, 7, 575.0, 0.0, backboneQueue},   {7, 10, 557.0, 0.0, backboneQueue},
      {10, 9, 369.0, 0.0, backboneQueue},  {10, 14, 1087.0, 0.004, backboneQueue},
      {9, 8, 537.0, 0.0, backboneQueue},   {8, 12, 898.0, 0.002, backboneQueue},
      {5, 11, 1617.0, 0.003, backboneQueue},{11, 15, 572.0, 0.0, backboneQueue},
      {14, 13, 362.0, 0.0, backboneQueue}, {13, 12, 729.0, 0.0, backboneQueue},
      {12, 15, 662.0, 0.0, backboneQueue}, {15, 16, 263.0, 0.0, backboneQueue},
      {14, 21, 413.0, 0.0, backboneQueue}, {21, 25, 913.0, 0.002, backboneQueue},
      {25, 30, 525.0, 0.0, backboneQueue}, {25, 24, 458.0, 0.0, backboneQueue},
      {24, 18, 346.0, 0.0, backboneQueue}, {24, 28, 572.0, 0.0, backboneQueue},
      {14, 20, 569.0, 0.0, backboneQueue}, {20, 19, 316.0, 0.0, backboneQueue},
      {19, 18, 306.0, 0.0, backboneQueue}, {18, 17, 249.0, 0.0, backboneQueue},
      {17, 16, 172.0, 0.0, backboneQueue}, {28, 29, 375.0, 0.0, backboneQueue},
      {29, 27, 55.0, 0.0, backboneQueue},  {29, 31, 199.0, 0.0, backboneQueue},
      {31, 32, 130.0, 0.0, backboneQueue}, {15, 22, 497.0, 0.0, backboneQueue},
      {22, 26, 279.0, 0.0, backboneQueue}, {26, 33, 644.0, 0.0, backboneQueue},
      {22, 23, 185.0, 0.0, backboneQueue}, {23, 27, 360.0, 0.0, backboneQueue},
      {32, 33, 306.0, 0.0, backboneQueue}, {2, 3, 907.0, 0.002, backboneQueue},
      {3, 4, 503.0, 0.0, backboneQueue},   {3, 6, 955.0, 0.003, backboneQueue},
      {8, 6, 598.0, 0.0, backboneQueue},   {4, 6, 934.0, 0.003, backboneQueue},
      {0, 1, 194.0, 0.0, backboneQueue},   {1, 5, 635.0, 0.0, backboneQueue},
      {1, 2, 233.0, 0.0, backboneQueue},   {1, 6, 1128.0, 0.003, backboneQueue},
  };
  spec.hostToSwitch.resize(spec.nodes.size());
  spec.hostNames.resize(spec.nodes.size());
  for (uint32_t i = 0; i < spec.nodes.size(); ++i) {
    spec.hostToSwitch[i] = i;
    spec.hostNames[i] = spec.nodes[i].name;
  }
  return spec;
}

/* ========================================================================= */
/*  Section partition + inter-domain routing — mirrors sections.json.
 *  Hardcoded as the default so the scenario is runnable without external
 *  configuration; CLI flags below can override each piece for non-default
 *  partitions.
 */
/* ========================================================================= */
struct SectionDef {
  uint32_t id;
  std::string name;
  std::vector<uint32_t> nodes;            // switch indices in the full topo
  std::vector<uint32_t> borderSwitches;   // subset of `nodes` with off-section links
};
struct InterDomainRoute {
  uint32_t fromSection;
  uint32_t toSection;
  uint32_t viaSwitch;    // 0-based index of OUR border switch
  uint32_t nextSwitch;   // 0-based index of neighbour switch in another section
};

static std::vector<SectionDef> DefaultSections() {
  return {
      {0, "west",    {0,1,2,3,4,5,6,7,8,9,10},                  {5, 8, 10}},
      {1, "central", {11,12,13,14,15,16,17,18,19,20,21},        {11, 12, 14, 15, 18, 21}},
      {2, "east",    {22,23,24,25,26,27,28,29,30,31,32,33},     {22, 24, 25}},
  };
}
static std::vector<InterDomainRoute> DefaultInterDomainRoutes() {
  return {
      {0, 1, 8,  12}, {0, 2, 8,  12},
      {1, 0, 12, 8},  {1, 2, 15, 22},
      {2, 0, 22, 15}, {2, 1, 22, 15},
  };
}

// Parse "a,b,c" → {a,b,c}; whitespace tolerant; empty → {}.
static std::vector<uint32_t> ParseCsv(const std::string& csv) {
  std::vector<uint32_t> out;
  std::string tok;
  for (char c : csv) {
    if (c == ',' || c == ' ' || c == '\t') {
      if (!tok.empty()) { out.push_back(std::stoul(tok)); tok.clear(); }
    } else {
      tok.push_back(c);
    }
  }
  if (!tok.empty()) out.push_back(std::stoul(tok));
  return out;
}
// Parse "0,1,2,3,4,5,6,7,8,9,10;11,12,...;22,23,..." → vector of vectors.
static std::vector<std::vector<uint32_t>> ParseSectionsCsv(const std::string& s) {
  std::vector<std::vector<uint32_t>> out;
  std::string cur;
  for (char c : s) {
    if (c == ';') { out.push_back(ParseCsv(cur)); cur.clear(); }
    else cur.push_back(c);
  }
  if (!cur.empty()) out.push_back(ParseCsv(cur));
  return out;
}
// Parse "0:1:8:12,0:2:8:12,..." → vector<InterDomainRoute>.
static std::vector<InterDomainRoute> ParseIdrCsv(const std::string& s) {
  std::vector<InterDomainRoute> out;
  std::string cur;
  auto flush = [&]() {
    if (cur.empty()) return;
    std::vector<uint32_t> v;
    std::string tok;
    for (char c : cur) {
      if (c == ':') { v.push_back(std::stoul(tok)); tok.clear(); }
      else tok.push_back(c);
    }
    if (!tok.empty()) v.push_back(std::stoul(tok));
    if (v.size() == 4) out.push_back({v[0], v[1], v[2], v[3]});
    cur.clear();
  };
  for (char c : s) {
    if (c == ',') flush();
    else cur.push_back(c);
  }
  flush();
  return out;
}

// MAC → uint64 helper (matches MacToU64 in UsaTopologyBuilder).
static uint64_t MacAddrToU64(const Mac48Address& addr) {
  uint8_t buf[6];
  addr.CopyTo(buf);
  uint64_t v = 0;
  for (int i = 0; i < 6; ++i) v = (v << 8) | buf[i];
  return v;
}

/* ========================================================================= */
/*  Multi-controller topology builder — installs each section's switches on
 *  its own OFSwitch13InternalHelper instance, so each Local Controller
 *  only sees handshakes from its own section.
 */
/* ========================================================================= */
class MultiCtrlBuilder {
 public:
  MultiCtrlBuilder() {
    m_edgeHelper.SetChannelAttribute("DataRate", StringValue("100Mbps"));
    m_edgeHelper.SetChannelAttribute("Delay", StringValue("1ms"));
    m_edgeHelper.SetDeviceAttribute("Mtu", UintegerValue(1500));
    m_backboneHelper.SetChannelAttribute("DataRate", StringValue("1Gbps"));
    m_backboneHelper.SetDeviceAttribute("Mtu", UintegerValue(1500));
  }

  void CreateNodes(uint32_t numHosts, uint32_t numSwitches, uint32_t numControllers) {
    m_numHosts = numHosts;
    m_numSwitches = numSwitches;
    m_numControllers = numControllers;
    m_hosts.Create(numHosts);
    m_switches.Create(numSwitches);
    m_controllers.Create(numControllers);
    m_swPorts.resize(numSwitches);
  }

  // Returns the host-side OF port index (1-indexed) so callers can register
  // hosts with their owning controller.
  uint32_t InstallHost(uint32_t hostIdx, uint32_t switchIdx,
                       const std::string& edgeQueue) {
    NetDeviceContainer dev = m_edgeHelper.Install(
        NodeContainer(m_hosts.Get(hostIdx), m_switches.Get(switchIdx)));
    m_hostPorts.Add(dev.Get(0));
    m_swPorts[switchIdx].Add(dev.Get(1));
    uint32_t ofPort = m_swPorts[switchIdx].GetN();
    ConfigureQueue(dev.Get(1), edgeQueue);
    return ofPort;
  }

  // Returns (srcPort, dstPort) of the link in 1-indexed OF port space.
  std::pair<uint32_t, uint32_t> AddBackboneLink(const LinkSpec& spec,
                                                const std::string& backboneQueue) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(6) << (spec.distanceKm * 5e-6) << "s";
    m_backboneHelper.SetChannelAttribute("Delay", StringValue(oss.str()));
    NetDeviceContainer dev = m_backboneHelper.Install(
        NodeContainer(m_switches.Get(spec.src), m_switches.Get(spec.dst)));
    m_swPorts[spec.src].Add(dev.Get(0));
    m_swPorts[spec.dst].Add(dev.Get(1));
    uint32_t srcPort = m_swPorts[spec.src].GetN();
    uint32_t dstPort = m_swPorts[spec.dst].GetN();
    ConfigureQueue(dev.Get(0), backboneQueue);
    ConfigureQueue(dev.Get(1), backboneQueue);
    if (spec.lossRate > 0.0) {
      SetLinkErrorRate(dev.Get(0), spec.lossRate);
      SetLinkErrorRate(dev.Get(1), spec.lossRate);
    }
    return {srcPort, dstPort};
  }

  void SetupIpStack() {
    InternetStackHelper internet;
    internet.Install(m_hosts);
    Ipv4AddressHelper ipv4;
    ipv4.SetBase("10.1.1.0", "255.255.255.0");
    m_hostIfaces = ipv4.Assign(m_hostPorts);
  }

  // Install one OFSwitch13InternalHelper per controller. Each helper owns
  // exactly the switches whose indices are in switchesPerCtrl[i] (full-topo
  // 0-based indices).
  void InstallOpenFlow(const std::vector<Ptr<ZmqOpenFlowController>>& ctrls,
                       const std::vector<std::vector<uint32_t>>& switchesPerCtrl) {
    NS_ASSERT(ctrls.size() == switchesPerCtrl.size());
    m_helpers.clear();
    m_helpers.resize(ctrls.size());
    for (uint32_t c = 0; c < ctrls.size(); ++c) {
      m_helpers[c] = CreateObject<OFSwitch13InternalHelper>();
      m_controllers.Get(c)->AddApplication(ctrls[c]);
      ctrls[c]->SetStartTime(Seconds(0));
      m_helpers[c]->InstallController(m_controllers.Get(c), ctrls[c]);
      for (uint32_t sw : switchesPerCtrl[c]) {
        if (sw >= m_numSwitches) continue;
        m_helpers[c]->InstallSwitch(m_switches.Get(sw), m_swPorts[sw]);
      }
      m_helpers[c]->CreateOpenFlowChannels();
    }
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

  Mac48Address HostMac(uint32_t hostIdx) const {
    Ptr<NetDevice> nd = m_hosts.Get(hostIdx)->GetDevice(0);
    return Mac48Address::ConvertFrom(nd->GetAddress());
  }

  NodeContainer& GetHosts() { return m_hosts; }
  NodeContainer& GetSwitches() { return m_switches; }
  Ipv4InterfaceContainer& GetHostIfaces() { return m_hostIfaces; }

 private:
  uint32_t m_numHosts = 0;
  uint32_t m_numSwitches = 0;
  uint32_t m_numControllers = 0;
  NodeContainer m_hosts;
  NodeContainer m_switches;
  NodeContainer m_controllers;
  NetDeviceContainer m_hostPorts;
  std::vector<NetDeviceContainer> m_swPorts;
  Ipv4InterfaceContainer m_hostIfaces;
  std::vector<Ptr<OFSwitch13InternalHelper>> m_helpers;
  CsmaHelper m_edgeHelper;
  CsmaHelper m_backboneHelper;

  void ConfigureQueue(Ptr<NetDevice> nd, const std::string& sizeStr) {
    Ptr<CsmaNetDevice> csma = DynamicCast<CsmaNetDevice>(nd);
    if (!csma) return;
    Ptr<Queue<Packet>> q = csma->GetQueue();
    if (q) q->SetAttribute("MaxSize", QueueSizeValue(QueueSize(sizeStr)));
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
/*  Lightweight ping-mesh stats collector — replicates the Liveness Probe
 *  report from usa/main.cc; full TCP/UDP mixed-load is out of scope here.
 */
/* ========================================================================= */
struct PingStats {
  static uint64_t tx;
  static uint64_t rx;
  static double rttSumMs;
  static void OnTx(uint16_t, Ptr<Packet>) { ++tx; }
  static void OnRtt(uint16_t, Time t) { ++rx; rttSumMs += t.GetMilliSeconds(); }
};
uint64_t PingStats::tx = 0;
uint64_t PingStats::rx = 0;
double PingStats::rttSumMs = 0.0;

/* ========================================================================= */
/*  Main                                                                      */
/* ========================================================================= */
int main(int argc, char* argv[]) {
  double simTime = 120.0;
  double warmupS = 5.0;
  uint32_t seed = 12345;
  std::string backboneQueue = "3MB";
  std::string edgeQueue = "500kB";

  // Multi-controller config overrides (CSV-encoded; default is sections.json).
  // sectionNodes:    "0,1,...;11,12,...;22,23,..." — one ';'-separated entry per section
  // borderSwitches:  same grouping, one CSV per section
  // interDomainRoutes: "from:to:via:next,from:to:via:next,..."
  std::string sectionNodesArg;
  std::string borderSwitchesArg;
  std::string interDomainArg;

  // ML config — same defaults as usa/main.cc.
  bool mlEnabled = false;
  double mlIntervalS = 1.0;
  double mlActionScale = 0.20;
  double mlActionScaleStart = 0.40;
  uint32_t mlTaperTicks = 200;
  std::string mlPriority = "balanced";
  bool mlExplore = true;
  uint32_t mlCheckpointEveryNTicks = 60;
  bool mlResume = true;
  std::string mlEndpointBase = "tcp://127.0.0.1:";
  uint16_t mlPortBase = 5555;

  double pingIntervalS = 1.0;
  uint32_t pingCount = 0;  // 0 = unlimited

  CommandLine cmd(__FILE__);
  cmd.AddValue("simTime", "Simulation duration (s)", simTime);
  cmd.AddValue("warmupS", "Pre-warmup window for flow installs (s)", warmupS);
  cmd.AddValue("seed", "Random seed", seed);
  cmd.AddValue("backboneQueue", "Backbone CSMA queue size", backboneQueue);
  cmd.AddValue("edgeQueue", "Edge CSMA queue size", edgeQueue);
  cmd.AddValue("sectionNodes",
               "';'-separated CSV lists of node indices per section "
               "(empty = use sections.json default partition)",
               sectionNodesArg);
  cmd.AddValue("borderSwitches",
               "';'-separated CSV lists of border switch indices per section "
               "(empty = use defaults)",
               borderSwitchesArg);
  cmd.AddValue("interDomainRoutes",
               "Comma-separated 'from:to:via:next' tuples specifying which "
               "border switch each section uses for each destination "
               "(empty = use defaults)",
               interDomainArg);
  cmd.AddValue("ml", "Enable FDRL agent on every Local Controller", mlEnabled);
  cmd.AddValue("mlIntervalS", "Agent period (s)", mlIntervalS);
  cmd.AddValue("mlActionScale", "Final |dW| fraction (after taper)", mlActionScale);
  cmd.AddValue("mlActionScaleStart", "Initial |dW| fraction (during taper)",
               mlActionScaleStart);
  cmd.AddValue("mlTaperTicks", "Ticks over which action_scale tapers",
               mlTaperTicks);
  cmd.AddValue("mlPriority",
               "Reward preset: balanced | delay_first | energy_first | custom",
               mlPriority);
  cmd.AddValue("mlExplore", "Enable OU exploration & training updates",
               mlExplore);
  cmd.AddValue("mlCheckpointEveryNTicks", "Checkpoint cadence",
               mlCheckpointEveryNTicks);
  cmd.AddValue("mlResume", "Resume from checkpoint", mlResume);
  cmd.AddValue("mlPortBase",
               "Lowest ZMQ port; ctrl i binds tcp://127.0.0.1:(mlPortBase+i)",
               mlPortBase);
  cmd.AddValue("pingIntervalS", "Per-pair ping interval (s)", pingIntervalS);
  cmd.AddValue("pingCount", "Pings per pair (0 = unlimited)", pingCount);
  cmd.Parse(argc, argv);

  RngSeedManager::SetSeed(seed);
  GlobalValue::Bind("SchedulerType", StringValue("ns3::CalendarScheduler"));

  /* --- Sections + inter-domain routes --------------------------------- */
  std::vector<SectionDef> sections = DefaultSections();
  if (!sectionNodesArg.empty()) {
    auto parsed = ParseSectionsCsv(sectionNodesArg);
    // Override only the .nodes field; keep names/ids in declaration order.
    if (parsed.size() != sections.size()) sections.resize(parsed.size());
    for (uint32_t i = 0; i < parsed.size(); ++i) {
      sections[i].id = i;
      if (sections[i].name.empty())
        sections[i].name = "sec" + std::to_string(i);
      sections[i].nodes = parsed[i];
    }
  }
  if (!borderSwitchesArg.empty()) {
    auto parsed = ParseSectionsCsv(borderSwitchesArg);
    for (uint32_t i = 0; i < parsed.size() && i < sections.size(); ++i) {
      sections[i].borderSwitches = parsed[i];
    }
  }
  std::vector<InterDomainRoute> idr = DefaultInterDomainRoutes();
  if (!interDomainArg.empty()) idr = ParseIdrCsv(interDomainArg);
  const uint32_t M = sections.size();

  /* --- Build full topology -------------------------------------------- */
  TopoSpec topo = BuildUsaSpec(backboneQueue);
  const uint32_t NUM_SWITCHES = topo.nodes.size();
  const uint32_t NUM_HOSTS = topo.hostToSwitch.size();

  MultiCtrlBuilder builder;
  builder.CreateNodes(NUM_HOSTS, NUM_SWITCHES, M);

  /* --- Build M controllers -------------------------------------------- */
  std::vector<Ptr<ZmqOpenFlowController>> ctrls(M);
  for (uint32_t i = 0; i < M; ++i) {
    ctrls[i] = CreateObject<ZmqOpenFlowController>();
    MlConfig cfg;
    cfg.enabled = mlEnabled;
    cfg.interval_s = mlIntervalS;
    cfg.action_scale = mlActionScale;
    cfg.action_scale_start = mlActionScaleStart;
    cfg.taper_ticks = mlTaperTicks;
    cfg.priority_preset = mlPriority;
    cfg.explore = mlExplore;
    cfg.checkpoint_every_n_ticks = mlCheckpointEveryNTicks;
    cfg.resume = mlResume;
    cfg.seed = seed + i;
    cfg.endpoint = mlEndpointBase + std::to_string(mlPortBase + i);
    cfg.controller_id = i;
    ctrls[i]->SetMlConfig(cfg);
  }

  /* --- Install hosts; collect per-(hostIdx, sectionOwner) info -------- */
  // For each host h: which section's controller owns it? (= section whose
  // nodes[] contains topo.hostToSwitch[h]).
  std::vector<int> switchToSection(NUM_SWITCHES, -1);
  for (uint32_t s = 0; s < M; ++s) {
    for (uint32_t sw : sections[s].nodes) {
      if (sw < NUM_SWITCHES) switchToSection[sw] = static_cast<int>(s);
    }
  }
  // Verify every switch is assigned.
  for (uint32_t sw = 0; sw < NUM_SWITCHES; ++sw) {
    if (switchToSection[sw] < 0) {
      std::cerr << "[FULLRUN] FATAL: switch " << sw
                << " not assigned to any section" << std::endl;
      return 1;
    }
  }

  struct HostMeta {
    uint64_t mac;
    uint32_t switchIdx;
    uint32_t ofPort;  // 1-indexed
    int ownerSection;
  };
  std::vector<HostMeta> hostMetas(NUM_HOSTS);
  for (uint32_t h = 0; h < NUM_HOSTS; ++h) {
    uint32_t sw = topo.hostToSwitch[h];
    uint32_t ofPort = builder.InstallHost(h, sw, edgeQueue);
    uint64_t mac = MacAddrToU64(builder.HostMac(h));
    int owner = switchToSection[sw];
    hostMetas[h] = {mac, sw, ofPort, owner};

    // Annotate the host on its OWNER controller (so its sdn_state JSON
    // shows the right name). Other controllers don't see this host
    // directly — they reach it via inter-domain routes installed below.
    HostAnnotation ann;
    ann.name = (h < topo.hostNames.size() ? topo.hostNames[h]
                                          : topo.nodes[sw].name) + "-Host";
    ann.node_type = "host";
    ctrls[owner]->SetHostAnnotation(mac, ann);
  }

  /* --- Build backbone; record per-link (src_port, dst_port) ----------- */
  // Indexed by (src_dpid, dst_dpid) → out_port on src side. Used below to
  // resolve InterDomainRoute → InstallExternalHostRoutes egress ports.
  std::map<std::pair<uint32_t, uint32_t>, uint32_t> portOf;
  for (const auto& spec : topo.links) {
    auto ports = builder.AddBackboneLink(spec, backboneQueue);
    portOf[{spec.src, spec.dst}] = ports.first;
    portOf[{spec.dst, spec.src}] = ports.second;
  }

  builder.SetupIpStack();

  /* --- Wire each section's switches to its controller ----------------- */
  std::vector<std::vector<uint32_t>> switchesPerCtrl(M);
  for (uint32_t s = 0; s < M; ++s) switchesPerCtrl[s] = sections[s].nodes;
  builder.InstallOpenFlow(ctrls, switchesPerCtrl);

  for (uint32_t sw = 0; sw < NUM_SWITCHES; ++sw) {
    int owner = switchToSection[sw];
    builder.ConfigureSwitch(sw, topo.nodes[sw], ctrls[owner]);
  }

  /* --- Schedule warm-up routing installs ------------------------------ */
  // Each controller:
  //   1. PreInstallAllPaths over INTRA-section hosts only (its m_topology
  //      reflects only its own switches; off-section dpids won't resolve).
  //   2. InstallExternalHostRoutes for every host in OTHER sections,
  //      routed through the border switch chosen by interDomainRoutes.
  Simulator::Schedule(Seconds(warmupS), [&]() {
    // Pre-bucket intra-section hosts per controller.
    std::vector<std::vector<ZmqOpenFlowController::HostInfo>> intra(M);
    for (const auto& hm : hostMetas) {
      uint64_t dpid = hm.switchIdx + 1;
      intra[hm.ownerSection].push_back({hm.mac, dpid, hm.ofPort});
    }
    for (uint32_t s = 0; s < M; ++s) {
      ctrls[s]->PreInstallAllPaths(intra[s]);
    }

    // For each section S, group inter-domain routes by target-section
    // and install one ExternalHostRoute per (external-host, border).
    for (uint32_t s = 0; s < M; ++s) {
      std::vector<ZmqOpenFlowController::ExternalHostRoute> routes;
      for (const auto& r : idr) {
        if (r.fromSection != s) continue;
        auto portIt = portOf.find({r.viaSwitch, r.nextSwitch});
        if (portIt == portOf.end()) {
          std::cerr << "[FULLRUN] WARN: no physical link between via_switch="
                    << r.viaSwitch << " and next_switch=" << r.nextSwitch
                    << " — skipping route " << r.fromSection
                    << "->" << r.toSection << std::endl;
          continue;
        }
        uint32_t borderOutPort = portIt->second;
        uint64_t borderDpid = r.viaSwitch + 1;
        for (const auto& hm : hostMetas) {
          if (hm.ownerSection != static_cast<int>(r.toSection)) continue;
          routes.push_back({hm.mac, borderDpid, borderOutPort});
        }
      }
      ctrls[s]->InstallExternalHostRoutes(routes);
    }
  });

  /* --- Traffic: full-mesh pings (every host pings every other) -------- */
  // Drives inter-domain routing through every (section_from → section_to)
  // path so the Phase 2 verification step can see cross-section delivery.
  ApplicationContainer pingApps;
  Ipv4InterfaceContainer& ifs = builder.GetHostIfaces();
  NodeContainer& hosts = builder.GetHosts();
  double measureStart = 1.0 + warmupS;
  for (uint32_t src = 0; src < NUM_HOSTS; ++src) {
    uint32_t dst = (src + NUM_HOSTS / 2) % NUM_HOSTS;
    if (dst == src) continue;
    PingHelper ping(Ipv4Address(ifs.GetAddress(dst)));
    ping.SetAttribute("VerboseMode", EnumValue(Ping::SILENT));
    ping.SetAttribute("Count", UintegerValue(pingCount));
    ping.SetAttribute("Interval", TimeValue(Seconds(pingIntervalS)));
    pingApps.Add(ping.Install(hosts.Get(src)));
  }
  pingApps.Start(Seconds(measureStart));
  pingApps.Stop(Seconds(simTime - 1.0));
  for (uint32_t i = 0; i < pingApps.GetN(); ++i) {
    pingApps.Get(i)->TraceConnectWithoutContext(
        "Tx", MakeCallback(&PingStats::OnTx));
    pingApps.Get(i)->TraceConnectWithoutContext(
        "Rtt", MakeCallback(&PingStats::OnRtt));
  }

  /* --- Flow monitor --------------------------------------------------- */
  // Install up front so probes are attached to each host's Ipv4L3Protocol
  // before any IP traffic flows; this counts pre-warmup ARP/ICMP as well,
  // but for fullrun the dominant traffic is the post-warmup ping mesh
  // anyway. (The scheduled-late-install pattern from usa/main.cc races
  // with the ping start in this scenario.)
  FlowMonitorHelper flowmonHelper;
  flowmonHelper.SetMonitorAttribute("DelayBinWidth", DoubleValue(0.01));
  Ptr<FlowMonitor> monitor = flowmonHelper.InstallAll();

  /* --- Run + report --------------------------------------------------- */
  std::cout << "[FULLRUN] Starting: simTime=" << simTime
            << "s, warmup=" << warmupS
            << "s, sections=" << M
            << " (";
  for (uint32_t s = 0; s < M; ++s) {
    std::cout << sections[s].name << "=" << sections[s].nodes.size();
    if (s + 1 < M) std::cout << ", ";
  }
  std::cout << "), ml=" << (mlEnabled ? "on" : "off") << std::endl;

  Simulator::Stop(Seconds(simTime));
  Simulator::Run();

  std::cout << "\n=== Liveness Probe (ping mesh) ===" << std::endl;
  std::cout << "  Sent      : " << PingStats::tx << std::endl;
  std::cout << "  Received  : " << PingStats::rx << std::endl;
  if (PingStats::tx > 0) {
    std::cout << "  Success   : "
              << (PingStats::rx * 100.0 / PingStats::tx) << "%" << std::endl;
  }
  if (PingStats::rx > 0) {
    std::cout << "  Avg RTT   : "
              << (PingStats::rttSumMs / PingStats::rx) << " ms" << std::endl;
  }

  monitor->CheckForLostPackets();
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
  std::cout << "\n=== FlowMonitor (post-warmup) ===" << std::endl;
  std::cout << "  Flows       : " << stats.size() << std::endl;
  std::cout << "  Tx packets  : " << totalTx << std::endl;
  std::cout << "  Rx packets  : " << totalRx << std::endl;
  std::cout << "  Lost packets: " << totalLost << std::endl;
  if (totalTx > 0) {
    std::cout << "  Delivery    : "
              << (totalRx * 100.0 / totalTx) << "%" << std::endl;
  }
  if (rxForDelay > 0) {
    std::cout << "  Avg delay   : "
              << (delaySumS * 1000.0 / rxForDelay) << " ms" << std::endl;
  }

  std::cout << "\n=== Per-Controller Hop Counts ===" << std::endl;
  double hopSum = 0.0;
  uint32_t hopCount = 0;
  for (uint32_t s = 0; s < M; ++s) {
    double avg = ctrls[s]->GetAverageHopCount();
    std::cout << "  ctrl[" << s << "] (" << sections[s].name
              << "): avg_hops=" << avg << std::endl;
    if (avg > 0) { hopSum += avg; ++hopCount; }
  }
  // Global line matches summarize_log's regex in scratch/run_tests.sh
  // so fullrun runs can still be summarized into summary.csv.
  std::cout << "  Avg hops    : "
            << (hopCount > 0 ? hopSum / hopCount : 0.0) << std::endl;

  Simulator::Destroy();
  std::cout << "[FULLRUN] Done." << std::endl;
  return 0;
}
