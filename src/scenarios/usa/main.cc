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
#include <map>
#include <set>
#include <sstream>
#include <vector>

#include "zmq-openflow-controller.h"

// Build optimized:
//   ./ns3 configure --build-profile=optimized --disable-asserts --disable-logs
//   ./ns3 build scratch_zmq_openflow scratch/scenarios/usa
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
    std::cerr << "[TRACE] LinkController::BringDown ENTER t="
              << Simulator::Now().GetSeconds() << "s ls=" << ls << std::endl;
    NS_LOG_INFO("Link DOWN at t=" << Simulator::Now().GetSeconds() << "s");
    SetErrorRate(ls->devA, 1.0);
    SetErrorRate(ls->devB, 1.0);
    std::cerr << "[TRACE] LinkController::BringDown EXIT" << std::endl;
  }
  static void BringUp(State* ls) {
    std::cerr << "[TRACE] LinkController::BringUp ENTER t="
              << Simulator::Now().GetSeconds() << "s ls=" << ls << std::endl;
    NS_LOG_INFO("Link UP at t=" << Simulator::Now().GetSeconds() << "s");
    SetErrorRate(ls->devA, ls->normalRate);
    SetErrorRate(ls->devB, ls->normalRate);
    std::cerr << "[TRACE] LinkController::BringUp EXIT" << std::endl;
  }
  static void Degrade(State* ls, double rate) {
    std::cerr << "[TRACE] LinkController::Degrade ENTER t="
              << Simulator::Now().GetSeconds() << "s rate=" << rate
              << " ls=" << ls << std::endl;
    NS_LOG_INFO("Link DEGRADED loss=" << rate
                                      << " at t="
                                      << Simulator::Now().GetSeconds() << "s");
    SetErrorRate(ls->devA, rate);
    SetErrorRate(ls->devB, rate);
    std::cerr << "[TRACE] LinkController::Degrade EXIT" << std::endl;
  }

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
/*  2b. STRESS EVENTS  – gray failures, flash crowds, correlated, black hole */
/* ========================================================================= */
class StressEvents {
 public:
  StressEvents(NodeContainer& hosts, NodeContainer& switches,
               Ipv4InterfaceContainer& ifaces,
               const std::vector<LinkController::State*>& failureLinks)
      : m_hosts(hosts),
        m_switches(switches),
        m_ifaces(ifaces),
        m_failureLinks(failureLinks) {}

  // Schedule the default mix at fractions of the measurement window:
  //   0.10 gray-fail link[0] at 30% loss → exposes "bad-but-not-dead" path,
  //        the case where ML routing should beat Dijkstra
  //   0.25 flash crowd → 12 bulk flows to dstHost for 5 s
  //   0.40 restore gray-failed link
  //   0.55 correlated outage → links[1] and [2] both down
  //   0.75 restore correlated outage
  //   0.85 node black-hole → drop all packets ingressing switchIdx for 5 s
  void Schedule(double measureStart, double window, uint32_t dstHostFlash,
                uint32_t blackHoleSwitchIdx) {
    auto at = [&](double frac) {
      return Seconds(measureStart + frac * window);
    };

    if (m_failureLinks.size() >= 1) {
      Simulator::Schedule(at(0.10), &LinkController::Degrade,
                          m_failureLinks[0], 0.30);
      Simulator::Schedule(at(0.40), &LinkController::BringUp,
                          m_failureLinks[0]);
    }

    // Flash crowd: 4 short bulk flows at 10 Mbps each = 40 Mbps to one host.
    // Higher intensities combined with UDP load destabilize ns-3.40's
    // CSMA/OFSwitch13 path; keep it sub-100 Mbps.
    // Pre-install up front (before Run) with future Start/Stop; dynamic
    // install from inside a scheduled callback raced with CSMA TX-queue
    // and SIGSEGV'd around t=30s under --tcp --failures.
    PreInstallFlashCrowd(dstHostFlash, 5.0, 10.0, 4,
                         measureStart + 0.25 * window);

    if (m_failureLinks.size() >= 3) {
      Simulator::Schedule(at(0.55), &LinkController::BringDown,
                          m_failureLinks[1]);
      Simulator::Schedule(at(0.55), &LinkController::BringDown,
                          m_failureLinks[2]);
      Simulator::Schedule(at(0.75), &LinkController::BringUp,
                          m_failureLinks[1]);
      Simulator::Schedule(at(0.75), &LinkController::BringUp,
                          m_failureLinks[2]);
      NS_LOG_INFO("StressEvents: correlated outage links["
                  << 1 << "," << 2 << "] scheduled");
    }

    if (blackHoleSwitchIdx < m_switches.GetN()) {
      Simulator::Schedule(at(0.85), &StressEvents::BlackHoleOn, this,
                          blackHoleSwitchIdx);
      Simulator::Schedule(at(0.85) + Seconds(5.0),
                          &StressEvents::BlackHoleOff, this,
                          blackHoleSwitchIdx);
    }
  }

 private:
  NodeContainer& m_hosts;
  NodeContainer& m_switches;
  Ipv4InterfaceContainer& m_ifaces;
  std::vector<LinkController::State*> m_failureLinks;

  void PreInstallFlashCrowd(uint32_t dstHost, double dur, double rateMbps,
                            uint32_t numFlows, double startT) {
    uint32_t n = m_hosts.GetN();
    if (dstHost >= n) return;
    NS_LOG_INFO("Flash crowd pre-installed: " << numFlows << " flows @ "
                                              << rateMbps << " Mbps to host "
                                              << dstHost << " firing at t="
                                              << startT << "s");
    Ptr<UniformRandomVariable> uv = CreateObject<UniformRandomVariable>();
    uint64_t bps = static_cast<uint64_t>(rateMbps * 1.0e6);
    for (uint32_t k = 0; k < numFlows; ++k) {
      uint32_t src;
      do {
        src = uv->GetInteger(0, n - 1);
      } while (src == dstHost);
      OnOffHelper onoff(
          "ns3::TcpSocketFactory",
          InetSocketAddress(m_ifaces.GetAddress(dstHost), 21));  // bulk port
      onoff.SetConstantRate(DataRate(bps), 1448);
      ApplicationContainer app = onoff.Install(m_hosts.Get(src));
      app.Start(Seconds(startT));
      app.Stop(Seconds(startT + dur));
    }
  }

  // Approximate a node black hole by setting receive error = 1.0 on every
  // CSMA device of the chosen switch. From neighbours' perspective the switch
  // accepts frames onto the wire but never processes them, so flows routed
  // through it observe 100% loss. True OpenFlow drop-rule would need
  // controller-side support (see Section D plan); this is the no-controller-
  // change approximation.
  void BlackHoleOn(uint32_t switchIdx) {
    std::cerr << "[TRACE] BlackHoleOn ENTER t="
              << Simulator::Now().GetSeconds() << "s sw=" << switchIdx
              << std::endl;
    NS_LOG_INFO("Black hole ON switch=" << switchIdx
                                        << " at t="
                                        << Simulator::Now().GetSeconds()
                                        << "s");
    Ptr<Node> sw = m_switches.Get(switchIdx);
    for (uint32_t i = 0; i < sw->GetNDevices(); ++i) {
      Ptr<NetDevice> nd = sw->GetDevice(i);
      if (!DynamicCast<CsmaNetDevice>(nd)) continue;
      LinkController::SetErrorRate(nd, 1.0);
    }
    std::cerr << "[TRACE] BlackHoleOn EXIT" << std::endl;
  }

  // Restore by installing a zero-rate model rather than re-applying a saved
  // pointer. Saving/restoring the prior ErrorModel pointer was the SIGSEGV
  // path: under --tcp --failures, the saved model could outlive the attribute
  // refresh from BringDown/BringUp on overlapping links.
  void BlackHoleOff(uint32_t switchIdx) {
    std::cerr << "[TRACE] BlackHoleOff ENTER t="
              << Simulator::Now().GetSeconds() << "s sw=" << switchIdx
              << std::endl;
    NS_LOG_INFO("Black hole OFF switch=" << switchIdx
                                         << " at t="
                                         << Simulator::Now().GetSeconds()
                                         << "s");
    Ptr<Node> sw = m_switches.Get(switchIdx);
    for (uint32_t i = 0; i < sw->GetNDevices(); ++i) {
      Ptr<NetDevice> nd = sw->GetDevice(i);
      if (!DynamicCast<CsmaNetDevice>(nd)) continue;
      LinkController::SetErrorRate(nd, 0.0);
    }
    std::cerr << "[TRACE] BlackHoleOff EXIT" << std::endl;
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

struct TopoSpec {
  std::vector<NodeProfile> nodes;       // one entry per switch
  std::vector<LinkSpec> links;
  std::vector<uint32_t> hostToSwitch;   // hostIdx -> switchIdx
  std::vector<std::string> hostNames;   // optional friendly host names
  std::string label;                    // "usa", "fat-tree-k4", ...
};

TopoSpec BuildUsaSpec(const std::string& backboneQueue, bool crippleEnabled) {
  TopoSpec spec;
  spec.label = "usa";
  spec.nodes = {
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
  if (!crippleEnabled) {
    spec.nodes[5] = {"Missoula", "tier2", "500Mbps", 5, 0.08, 2e7};
  }
  spec.links = {
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
      {15, 22, 497.0, 0.0, backboneQueue, true},
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
  // 1:1 host:switch mapping — every switch gets its own host.
  spec.hostToSwitch.resize(spec.nodes.size());
  spec.hostNames.resize(spec.nodes.size());
  for (uint32_t i = 0; i < spec.nodes.size(); ++i) {
    spec.hostToSwitch[i] = i;
    spec.hostNames[i] = spec.nodes[i].name;
  }
  return spec;
}

// Standard 3-tier fat-tree with k=4:
//   4 cores  (idx 0..3)
//   4 pods × (2 agg + 2 edge) = 16 pod switches (idx 4..19)
//   8 edges × 2 hosts/edge   = 16 hosts
// Agg-core split: agg[p][0] connects to cores 0,1; agg[p][1] to cores 2,3.
TopoSpec BuildFatTreeK4Spec(const std::string& backboneQueue) {
  TopoSpec spec;
  spec.label = "fat-tree-k4";
  auto pod = [](uint32_t p, uint32_t slot) { return 4 + p * 4 + slot; };

  // Cores
  for (uint32_t c = 0; c < 4; ++c) {
    spec.nodes.push_back(
        {"core" + std::to_string(c), "tier1", "1Gbps", 2, 0.05, 5e7});
  }
  // Pod switches: 2 aggs + 2 edges per pod
  for (uint32_t p = 0; p < 4; ++p) {
    spec.nodes.push_back({"agg" + std::to_string(p) + "_0", "tier2", "500Mbps",
                          5, 0.08, 2e7});
    spec.nodes.push_back({"agg" + std::to_string(p) + "_1", "tier2", "500Mbps",
                          5, 0.08, 2e7});
    spec.nodes.push_back(
        {"edge" + std::to_string(p) + "_0", "edge", "100Mbps", 10, 0.10, 1e7});
    spec.nodes.push_back(
        {"edge" + std::to_string(p) + "_1", "edge", "100Mbps", 10, 0.10, 1e7});
  }

  // Links — fat-tree backbone all use 200km/1ms delay, no loss.
  const double dKm = 200.0;
  for (uint32_t p = 0; p < 4; ++p) {
    uint32_t a0 = pod(p, 0), a1 = pod(p, 1);
    uint32_t e0 = pod(p, 2), e1 = pod(p, 3);
    // Core-Agg: a0↔{c0,c1}, a1↔{c2,c3}
    spec.links.push_back({a0, 0, dKm, 0.0, backboneQueue, false});
    spec.links.push_back({a0, 1, dKm, 0.0, backboneQueue, false});
    spec.links.push_back({a1, 2, dKm, 0.0, backboneQueue, false});
    spec.links.push_back({a1, 3, dKm, 0.0, backboneQueue, false});
    // Agg-Edge: full mesh within pod
    spec.links.push_back({a0, e0, dKm, 0.0, backboneQueue, false});
    spec.links.push_back({a0, e1, dKm, 0.0, backboneQueue, false});
    spec.links.push_back({a1, e0, dKm, 0.0, backboneQueue, false});
    spec.links.push_back({a1, e1, dKm, 0.0, backboneQueue, false});
  }
  // Mark a few links as failure-targets for StressEvents.
  spec.links[0].failureTarget = true;   // pod0 agg0-core0
  spec.links[8].failureTarget = true;   // pod1 agg0-core0
  spec.links[16].failureTarget = true;  // pod2 agg0-core0

  // Hosts: 2 per edge, in pod order
  for (uint32_t p = 0; p < 4; ++p) {
    uint32_t e0 = pod(p, 2), e1 = pod(p, 3);
    spec.hostToSwitch.push_back(e0);
    spec.hostToSwitch.push_back(e0);
    spec.hostToSwitch.push_back(e1);
    spec.hostToSwitch.push_back(e1);
  }
  for (uint32_t h = 0; h < 16; ++h) {
    spec.hostNames.push_back("h" + std::to_string(h));
  }
  return spec;
}

// Parse a comma-separated list of switch indices ("0,3,4,7"). Whitespace
// around the commas is tolerated. Returns an empty vector for empty input.
std::vector<uint32_t> ParseIndexCsv(const std::string& csv) {
  std::vector<uint32_t> out;
  std::string tok;
  for (char c : csv) {
    if (c == ',' || c == ' ' || c == '\t') {
      if (!tok.empty()) { out.push_back(static_cast<uint32_t>(std::stoul(tok))); tok.clear(); }
    } else {
      tok.push_back(c);
    }
  }
  if (!tok.empty()) out.push_back(static_cast<uint32_t>(std::stoul(tok)));
  return out;
}

// Restrict a TopoSpec to a subset of switch indices. Used by Phase 1 of the
// hierarchical-SDN setup: each parallel ns-3 process simulates one "section"
// (= one Local Controller's domain) by keeping only its own nodes/links/hosts.
// Cross-section links — those with one endpoint outside the kept set — are
// dropped entirely; in Phase 1 sections are simulated as isolated subgraphs.
// Indices in the returned spec are renumbered contiguously starting at 0.
TopoSpec FilterTopoSpecBySection(const TopoSpec& full,
                                 const std::vector<uint32_t>& keptIndices) {
  TopoSpec out;
  out.label = full.label + "-section";

  // Build kept-set + old→new index map. Iteration order of keptIndices
  // controls the new numbering.
  std::vector<int> oldToNew(full.nodes.size(), -1);
  for (uint32_t newIdx = 0; newIdx < keptIndices.size(); ++newIdx) {
    uint32_t oldIdx = keptIndices[newIdx];
    if (oldIdx >= full.nodes.size()) continue;          // silently drop OOB
    if (oldToNew[oldIdx] != -1) continue;               // silently drop dup
    oldToNew[oldIdx] = static_cast<int>(out.nodes.size());
    out.nodes.push_back(full.nodes[oldIdx]);
  }

  // Filter links: keep only edges with both endpoints in the section.
  uint32_t droppedXSection = 0;
  for (const auto& l : full.links) {
    if (l.src >= full.nodes.size() || l.dst >= full.nodes.size()) continue;
    int s = oldToNew[l.src], d = oldToNew[l.dst];
    if (s < 0 || d < 0) { ++droppedXSection; continue; }
    LinkSpec rewritten = l;
    rewritten.src = static_cast<uint32_t>(s);
    rewritten.dst = static_cast<uint32_t>(d);
    out.links.push_back(rewritten);
  }

  // Hosts: keep the host iff its switch survived. Renumber the switch ref.
  for (uint32_t h = 0; h < full.hostToSwitch.size(); ++h) {
    uint32_t sw = full.hostToSwitch[h];
    if (sw >= full.nodes.size()) continue;
    int newSw = oldToNew[sw];
    if (newSw < 0) continue;
    out.hostToSwitch.push_back(static_cast<uint32_t>(newSw));
    out.hostNames.push_back(h < full.hostNames.size()
                                ? full.hostNames[h]
                                : full.nodes[sw].name);
  }

  std::cout << "[SECTION] kept " << out.nodes.size() << "/" << full.nodes.size()
            << " switches, " << out.links.size() << "/" << full.links.size()
            << " links (" << droppedXSection << " cross-section dropped), "
            << out.hostToSwitch.size() << "/" << full.hostToSwitch.size()
            << " hosts" << std::endl;
  return out;
}

/* ========================================================================= */
/*  4. TOPOLOGY BUILDER                                                      */
/* ========================================================================= */
class UsaTopologyBuilder {
 public:
  UsaTopologyBuilder() {
    m_edgeHelper.SetChannelAttribute("DataRate", StringValue("100Mbps"));
    m_edgeHelper.SetChannelAttribute("Delay", StringValue("1ms"));
    m_edgeHelper.SetDeviceAttribute("Mtu", UintegerValue(1500));
    m_backboneHelper.SetChannelAttribute("DataRate", StringValue("1Gbps"));
    m_backboneHelper.SetDeviceAttribute("Mtu", UintegerValue(1500));
  }

  void CreateNodes(uint32_t numHosts, uint32_t numSwitches) {
    m_numHosts = numHosts;
    m_numSwitches = numSwitches;
    m_hosts.Create(numHosts);
    m_switches.Create(numSwitches);
    m_controllers.Create(1);
    m_swPorts.resize(numSwitches);
    m_linkStorage.reserve(16);
  }

  void InstallHost(uint32_t hostIdx, uint32_t switchIdx, const std::string& name,
                   Ptr<ZmqOpenFlowController> ctrl,
                   const std::string& edgeQueueSize) {
    NetDeviceContainer dev = m_edgeHelper.Install(
        NodeContainer(m_hosts.Get(hostIdx), m_switches.Get(switchIdx)));
    m_hostPorts.Add(dev.Get(0));
    m_swPorts[switchIdx].Add(dev.Get(1));
    uint32_t ofPort = m_swPorts[switchIdx].GetN();  // 1-indexed OF port
    ConfigureQueue(dev.Get(1), edgeQueueSize);

    Ptr<NetDevice> nd = m_hosts.Get(hostIdx)->GetDevice(0);
    Mac48Address addr = Mac48Address::ConvertFrom(nd->GetAddress());
    uint64_t macU64 = MacToU64(addr);
    HostAnnotation ann;
    ann.name = name + "-Host";
    ann.node_type = "host";
    ctrl->SetHostAnnotation(macU64, ann);
    m_hostMetas.push_back({macU64, switchIdx + 1, ofPort});
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

  void PrePopulateArp() {
    uint32_t n = m_numHosts;
    for (uint32_t i = 0; i < n; ++i) {
      Ptr<Node> host = m_hosts.Get(i);
      Ptr<Ipv4L3Protocol> ip = host->GetObject<Ipv4L3Protocol>();
      Ptr<ArpCache> cache = CreateObject<ArpCache>();
      cache->SetAliveTimeout(Seconds(86400));

      for (uint32_t j = 0; j < n; ++j) {
        if (i == j) continue;
        Ipv4Address addr = m_hostIfaces.GetAddress(j);
        Address mac = m_hosts.Get(j)->GetDevice(0)->GetAddress();
        ArpCache::Entry* entry = cache->Add(addr);
        entry->SetMacAddress(Mac48Address::ConvertFrom(mac));
        entry->MarkPermanent();
      }
      // iface 0 = loopback, 1 = edge link
      ip->GetInterface(1)->SetArpCache(cache);
    }
  }

  void InstallOpenFlow(Ptr<ZmqOpenFlowController> ctrlApp) {
    m_ofHelper = CreateObject<OFSwitch13InternalHelper>();
    m_controllers.Get(0)->AddApplication(ctrlApp);
    ctrlApp->SetStartTime(Seconds(0));
    m_ofHelper->InstallController(m_controllers.Get(0));

    for (uint32_t i = 0; i < m_numSwitches; ++i) {
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
  const std::vector<ZmqOpenFlowController::HostInfo>& GetHostInfos() const {
    return m_hostMetas;
  }

 private:
  uint32_t m_numHosts = 0;
  uint32_t m_numSwitches = 0;
  std::vector<ZmqOpenFlowController::HostInfo> m_hostMetas;
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
struct TrafficClass {
  std::string name;
  double weight;          // class-selection probability; weights must sum ~1.0
  double meanRateMbps;    // double so voip (0.064 Mbps) fits
  double meanDurS;
  uint16_t port;
  bool isTcp;             // false → UDP
  uint32_t pktSize;       // 1448 TCP, 160 voip, 512 iot
  bool cbr;               // true = constant rate, skip Pareto sampling
};

class TrafficManager {
 public:
  TrafficManager(NodeContainer& hosts, Ipv4InterfaceContainer& ifaces)
      : m_hosts(hosts), m_ifaces(ifaces) {
    m_uv = CreateObject<UniformRandomVariable>();
    m_durRv = CreateObject<LogNormalRandomVariable>();
    m_durRv->SetAttribute("Sigma", DoubleValue(0.6));
    m_rateRv = CreateObject<ParetoRandomVariable>();
    m_rateRv->SetAttribute("Scale", DoubleValue(1.0));
    m_rateRv->SetAttribute("Shape", DoubleValue(1.5));
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

  // Poisson arrivals, multi-class. All app installation happens up front
  // (before Simulator::Run) with future Start/Stop times — dynamic install
  // from inside a scheduled lambda caused intermittent SIGSEGV around t=30s
  // when CSMA's TX-queue interacted with newly-attached OnOff sockets.
  // Backpressure is simulated deterministically against a virtual
  // m_activeFlows counter as the schedule is built.
  void InstallMixedLoad(double startTime, double simTime,
                        const std::vector<TrafficClass>& classes,
                        const std::string& trafficMode,
                        uint32_t maxConcurrent, double arrivalRateHz) {
    if (classes.empty() || arrivalRateHz <= 0.0) return;
    m_classes = classes;
    m_portToClass.clear();
    m_cumWeights.clear();
    double cum = 0.0;
    for (const auto& c : classes) {
      m_portToClass[c.port] = c.name;
      cum += c.weight;
      m_cumWeights.push_back(cum);
    }
    m_weightTotal = cum;

    uint32_t n = m_hosts.GetN();
    for (uint32_t i = 0; i < n; ++i) {
      for (const auto& c : classes) {
        std::string fac =
            c.isTcp ? "ns3::TcpSocketFactory" : "ns3::UdpSocketFactory";
        PacketSinkHelper sink(
            fac, InetSocketAddress(Ipv4Address::GetAny(), c.port));
        ApplicationContainer app = sink.Install(m_hosts.Get(i));
        app.Start(Seconds(startTime - 0.5));
        app.Stop(Seconds(simTime));
        m_sinkApps.Add(app);
      }
    }

    Ptr<ExponentialRandomVariable> interArr =
        CreateObject<ExponentialRandomVariable>();
    interArr->SetAttribute("Mean", DoubleValue(1.0 / arrivalRateHz));

    // Track virtual active-flow count keyed by predicted end-time. Pop
    // ended flows before each new arrival to update the simulated count.
    std::multiset<double> endTimes;
    double t = startTime;
    while (t < simTime - 2.0) {
      t += interArr->GetValue();
      if (t >= simTime - 2.0) break;

      while (!endTimes.empty() && *endTimes.begin() <= t) {
        endTimes.erase(endTimes.begin());
      }
      if (endTimes.size() >= maxConcurrent) continue;

      uint32_t src = m_uv->GetInteger(0, n - 1);
      uint32_t dst = PickDestination(src, n, trafficMode);
      if (dst == src) continue;

      double roll = m_uv->GetValue(0.0, m_weightTotal);
      size_t idx = 0;
      while (idx + 1 < m_cumWeights.size() && roll > m_cumWeights[idx]) ++idx;
      const TrafficClass& cls = m_classes[idx];

      m_durRv->SetAttribute(
          "Mu", DoubleValue(std::log(std::max(1e-6, cls.meanDurS))));
      double dur = std::clamp(m_durRv->GetValue(), 1.0, 3.0 * cls.meanDurS);
      double endT = std::min(t + dur, simTime - 0.5);
      if (endT <= t + 0.1) continue;
      dur = endT - t;

      double rateMbps;
      if (cls.cbr) {
        rateMbps = cls.meanRateMbps;
      } else {
        double r = m_rateRv->GetValue() * cls.meanRateMbps;
        rateMbps =
            std::clamp(r, 0.5 * cls.meanRateMbps, 4.0 * cls.meanRateMbps);
      }
      uint64_t bps = static_cast<uint64_t>(rateMbps * 1.0e6);
      if (bps == 0) bps = 1000;

      std::string fac =
          cls.isTcp ? "ns3::TcpSocketFactory" : "ns3::UdpSocketFactory";
      OnOffHelper onoff(
          fac, InetSocketAddress(m_ifaces.GetAddress(dst), cls.port));
      onoff.SetConstantRate(DataRate(bps), cls.pktSize);

      ApplicationContainer app = onoff.Install(m_hosts.Get(src));
      app.Start(Seconds(t));
      app.Stop(Seconds(endT));
      m_srcApps.Add(app);
      endTimes.insert(endT);
    }
  }

  // Public accessor so the report block in main() can classify flows by port.
  const std::map<uint16_t, std::string>& PortToClass() const {
    return m_portToClass;
  }

 private:
  NodeContainer& m_hosts;
  Ipv4InterfaceContainer& m_ifaces;
  ApplicationContainer m_warmupApps;
  ApplicationContainer m_srcApps;
  ApplicationContainer m_sinkApps;
  ApplicationContainer m_pingApps;
  Ptr<UniformRandomVariable> m_uv;
  Ptr<LogNormalRandomVariable> m_durRv;
  Ptr<ParetoRandomVariable> m_rateRv;
  std::vector<TrafficClass> m_classes;
  std::vector<double> m_cumWeights;
  double m_weightTotal = 0.0;
  std::map<uint16_t, std::string> m_portToClass;

  uint32_t PickDestination(uint32_t src, uint32_t n, const std::string& mode) {
    uint32_t dst = src;
    if (mode == "central") {
      // 15 = Chicago in the full USA topology. After --sectionNodes filters
      // hosts down, the renumbered space may have fewer than 16 hosts, so
      // clamp to keep the picker in-bounds. Pick a deterministic non-self
      // alternative if the clamp would collide with src.
      dst = (n > 15) ? 15 : (n - 1);
      if (dst == src && n > 1) dst = (src == 0) ? 1 : 0;
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
  // Need >=5s so the second LLDP cycle has fired and m_topology is complete
  // before PreInstallAllPaths runs.
  double warmupS = 5.0;
  std::string trafficMode = "random";
  uint32_t seed = 12345;

  bool pingEnabled = true;
  bool tcpEnabled = false;
  bool failuresEnabled = false;
  bool crippleEnabled = false;
  uint32_t maxConcurrent = 60;
  double arrivalRateHz = 8.0;
  uint32_t flashCrowdDst = 15;       // Chicago
  uint32_t blackHoleSwitchIdx = 22;  // Cleveland (tier2)
  std::string backboneQueue = "3MB";
  std::string edgeQueue = "500kB";
  std::string topoName = "usa";

  bool mlEnabled = false;
  double mlIntervalS = 1.0;
  // Action-scale taper defaults: 0.40 → 0.20 over the first 200 ticks. Big
  // swings during exploration, fine-tune once the policy stabilizes.
  double mlActionScale = 0.20;
  double mlActionScaleStart = 0.40;
  uint32_t mlTaperTicks = 200;
  // Reward weights — left at defaults from MlConfig; preset string can override.
  double mlAlpha = 1.0;
  double mlBeta = 2.0;
  double mlGamma = 1.5;
  double mlDelta = 1.0;
  double mlZeta = 0.5;
  double mlEta = 1.5;
  double mlTheta = 1.0;
  // Normalization references.
  double mlDelayRef = 200.0;
  double mlLossRef = 1.0e6;
  double mlPowerRef = 90000.0;
  // "balanced" | "delay_first" | "energy_first" | "custom"
  std::string mlPriority = "balanced";
  bool mlExplore = true;
  uint32_t mlCheckpointEveryNTicks = 60;
  bool mlResume = true;
  std::string mlEndpoint = "tcp://127.0.0.1:5555";
  // Optional: delay FlowMonitor reset until measureStart + evalWindowOffsetS,
  // so a long learning prefix doesn't pollute the reported numbers. 0 = off.
  double evalWindowOffsetS = 0.0;

  // Phase-1 hierarchical-SDN sectioning. Empty sectionNodes = run the whole
  // topology (backward-compatible default). When set, this ns-3 process
  // simulates only the listed switches as one Local Controller's domain.
  // sectionId is used purely for naming/logging.
  uint32_t sectionId = 0;
  std::string sectionNodes;

  CommandLine cmd(__FILE__);
  cmd.AddValue("trace", "Enable pcap and datapath stats traces", trace);
  cmd.AddValue("simTime", "Simulation duration (s)", simTime);
  cmd.AddValue("warmupS", "Pre-warmup window for flow installs (s)", warmupS);
  cmd.AddValue("trafficMode", "Traffic: random, central, grouped", trafficMode);
  cmd.AddValue("seed", "Random seed", seed);
  cmd.AddValue("ping", "Enable measurement pings", pingEnabled);
  cmd.AddValue("tcp", "Enable OnOff TCP background load", tcpEnabled);
  cmd.AddValue("failures", "Enable scheduled link churn", failuresEnabled);
  cmd.AddValue("cripple", "Cripple Missoula node (1Mbps CPU, 100us TCAM)",
               crippleEnabled);
  cmd.AddValue("maxConcurrent", "Hard cap on concurrent mixed-load flows",
               maxConcurrent);
  cmd.AddValue("arrivalRateHz", "Mean Poisson arrival rate for new flows",
               arrivalRateHz);
  cmd.AddValue("flashCrowdDst", "Host index targeted by the flash crowd",
               flashCrowdDst);
  cmd.AddValue("blackHoleSwitch", "Switch index that goes dark at 0.85 W",
               blackHoleSwitchIdx);
  cmd.AddValue("backboneQueue", "Backbone CSMA queue size", backboneQueue);
  cmd.AddValue("edgeQueue", "Edge CSMA queue size", edgeQueue);
  cmd.AddValue("topology", "Topology: usa | fat-tree-k4", topoName);
  cmd.AddValue("ml", "Enable FDRL agent", mlEnabled);
  cmd.AddValue("mlIntervalS", "Agent period (s)", mlIntervalS);
  cmd.AddValue("mlActionScale", "Final |dW| fraction (after taper)", mlActionScale);
  cmd.AddValue("mlActionScaleStart", "Initial |dW| fraction (during taper)",
               mlActionScaleStart);
  cmd.AddValue("mlTaperTicks", "Ticks over which action_scale tapers",
               mlTaperTicks);
  cmd.AddValue("mlPriority",
               "Reward preset: balanced | delay_first | energy_first | custom",
               mlPriority);
  cmd.AddValue("mlAlpha", "Reward weight α (delay quality)", mlAlpha);
  cmd.AddValue("mlBeta", "Reward weight β (loss quality)", mlBeta);
  cmd.AddValue("mlGamma", "Reward weight γ (power-consumption penalty)", mlGamma);
  cmd.AddValue("mlDelta", "Reward weight δ (utilization penalty)", mlDelta);
  cmd.AddValue("mlZeta", "Reward weight ζ (active-switch footprint)", mlZeta);
  cmd.AddValue("mlEta", "Reward weight η (route-through-low-reserve penalty)",
               mlEta);
  cmd.AddValue("mlTheta", "Reward weight θ (residual-energy stddev)", mlTheta);
  cmd.AddValue("mlDelayRef", "Delay reference for normalization (ms)", mlDelayRef);
  cmd.AddValue("mlLossRef", "Loss reference for normalization (bps)", mlLossRef);
  cmd.AddValue("mlPowerRef", "Power reference for normalization (W)", mlPowerRef);
  cmd.AddValue("mlExplore", "Enable OU exploration & training updates",
               mlExplore);
  cmd.AddValue("mlCheckpointEveryNTicks", "Checkpoint cadence",
               mlCheckpointEveryNTicks);
  cmd.AddValue("mlResume", "Resume from checkpoint", mlResume);
  cmd.AddValue("mlEndpoint", "ZMQ endpoint", mlEndpoint);
  cmd.AddValue("evalWindowOffsetS",
               "Delay FlowMonitor reset by this many seconds past warmup "
               "(0 = report from warmup end)",
               evalWindowOffsetS);
  cmd.AddValue("sectionId",
               "Section/Local-Controller id, used for naming and logs",
               sectionId);
  cmd.AddValue("sectionNodes",
               "CSV of original switch indices this section simulates "
               "(empty = whole topology)",
               sectionNodes);
  cmd.Parse(argc, argv);

  RngSeedManager::SetSeed(seed);

  // Amortized O(1) insert/remove vs O(log n) for priority-queue/map. Pays off
  // hard when TCP cwnd churn produces millions of micro-events per sim-second.
  GlobalValue::Bind("SchedulerType", StringValue("ns3::MapScheduler"));

  Config::SetDefault("ns3::TcpSocket::SndBufSize", UintegerValue(131072));
  Config::SetDefault("ns3::TcpSocket::RcvBufSize", UintegerValue(131072));
  Config::SetDefault("ns3::TcpSocket::SegmentSize", UintegerValue(1448));
  Config::SetDefault("ns3::TcpSocket::InitialCwnd", UintegerValue(10));
  Config::SetDefault("ns3::TcpSocketBase::Sack", BooleanValue(true));
  Config::SetDefault("ns3::TcpL4Protocol::SocketType",
                     TypeIdValue(TcpCubic::GetTypeId()));
  Config::SetDefault("ns3::ArpCache::AliveTimeout", TimeValue(Seconds(86400)));
  Config::SetDefault("ns3::ArpCache::DeadTimeout", TimeValue(Seconds(86400)));

  if (warmupS < 0.0) warmupS = 0.0;
  if (warmupS > simTime - 5.0) warmupS = std::max(0.0, simTime - 5.0);

  /* --- 6a. TOPOLOGY SPEC ----------------------------------------------- */
  TopoSpec topo;
  if (topoName == "fat-tree-k4") {
    topo = BuildFatTreeK4Spec(backboneQueue);
  } else {
    topo = BuildUsaSpec(backboneQueue, crippleEnabled);
  }
  if (!sectionNodes.empty()) {
    std::vector<uint32_t> kept = ParseIndexCsv(sectionNodes);
    std::cout << "[SECTION] sectionId=" << sectionId
              << " requested " << kept.size() << " switches from full topology"
              << std::endl;
    topo = FilterTopoSpecBySection(topo, kept);
    if (topo.nodes.empty()) {
      std::cerr << "[SECTION] FATAL: filtered topology has no switches — "
                   "check --sectionNodes"
                << std::endl;
      return 1;
    }
  }
  const std::vector<NodeProfile>& nodeProfiles = topo.nodes;
  const std::vector<LinkSpec>& linkSpecs = topo.links;
  const uint32_t NUM_SWITCHES = nodeProfiles.size();
  const uint32_t NUM_HOSTS = topo.hostToSwitch.size();

  /* --- 6c. BUILD ------------------------------------------------------- */
  UsaTopologyBuilder builder;
  builder.CreateNodes(NUM_HOSTS, NUM_SWITCHES);

  Ptr<ZmqOpenFlowController> ctrl = CreateObject<ZmqOpenFlowController>();
  {
    MlConfig mlCfg;
    mlCfg.enabled = mlEnabled;
    mlCfg.interval_s = mlIntervalS;
    mlCfg.action_scale = mlActionScale;
    mlCfg.action_scale_start = mlActionScaleStart;
    mlCfg.taper_ticks = mlTaperTicks;
    mlCfg.priority_preset = mlPriority;
    mlCfg.reward_alpha = mlAlpha;
    mlCfg.reward_beta = mlBeta;
    mlCfg.reward_gamma = mlGamma;
    mlCfg.reward_delta = mlDelta;
    mlCfg.reward_zeta = mlZeta;
    mlCfg.reward_eta = mlEta;
    mlCfg.reward_theta = mlTheta;
    mlCfg.delay_ref_ms = mlDelayRef;
    mlCfg.loss_ref_bps = mlLossRef;
    mlCfg.power_ref_w = mlPowerRef;
    mlCfg.explore = mlExplore;
    mlCfg.checkpoint_every_n_ticks = mlCheckpointEveryNTicks;
    mlCfg.resume = mlResume;
    mlCfg.seed = seed;
    mlCfg.endpoint = mlEndpoint;
    ctrl->SetMlConfig(mlCfg);
  }

  for (uint32_t h = 0; h < NUM_HOSTS; ++h) {
    uint32_t sw = topo.hostToSwitch[h];
    const std::string& hostName = h < topo.hostNames.size()
                                       ? topo.hostNames[h]
                                       : nodeProfiles[sw].name;
    builder.InstallHost(h, sw, hostName, ctrl, edgeQueue);
  }

  std::vector<LinkController::State*> failureLinks;
  for (const auto& spec : linkSpecs) {
    LinkController::State* ls = builder.AddBackboneLink(spec, spec.bufferSize);
    if (ls) failureLinks.push_back(ls);
  }

  builder.SetupIpStack();
  builder.PrePopulateArp();
  builder.InstallOpenFlow(ctrl);

  // ConfigureSwitch sets attributes on OFSwitch13Device, which only exists
  // after InstallOpenFlow has run. Running it earlier silently no-ops the
  // CPU/TCAM/energy assignments.
  for (uint32_t i = 0; i < NUM_SWITCHES; ++i) {
    builder.ConfigureSwitch(i, nodeProfiles[i], ctrl);
  }

  /* --- 6d. TRAFFIC ----------------------------------------------------- */
  TrafficManager traffic(builder.GetHosts(), builder.GetHostIfaces());

  // Proactive routing: wait warmupS for LLDP discovery (first cycle at 0.5s,
  // next at 5.5s; warmupS>=5 is safe), then push every (switch, host) shortest
  // path as a non-expiring flow-mod. Replaces the old 1,122 pair-ping warmup.
  double measureStart = 1.0 + warmupS;
  Simulator::Schedule(Seconds(warmupS), [&builder, ctrl]() {
    ctrl->PreInstallAllPaths(builder.GetHostInfos());
  });

  // Default class mix — declared here so the per-class report knows the names.
  std::vector<TrafficClass> trafficClasses = {
      {"web",   0.50,  2.0,    3.0,  80,   true,  1448, false},
      {"video", 0.20,  8.0,   20.0,  8080, true,  1448, false},
      {"voip",  0.15,  0.064, 15.0,  5060, false, 160,  true},
      {"bulk",  0.10, 10.0,   25.0,  21,   true,  1448, false},
      {"iot",   0.05,  0.064, 60.0,  1883, false, 512,  true},
  };

  if (pingEnabled) {
    traffic.InstallPings(measureStart, simTime);
  }
  if (tcpEnabled) {
    traffic.InstallMixedLoad(measureStart, simTime, trafficClasses, trafficMode,
                             maxConcurrent, arrivalRateHz);
  }

  /* --- 6e. SCHEDULED CHURN --------------------------------------------- */
  StressEvents stress(builder.GetHosts(), builder.GetSwitches(),
                      builder.GetHostIfaces(), failureLinks);
  if (failuresEnabled) {
    double window = simTime - measureStart;
    stress.Schedule(measureStart, window, flashCrowdDst, blackHoleSwitchIdx);
  }

  /* --- 6f. FLOW MONITOR ------------------------------------------------ */
  // Hook the FlowMonitor probes only when measurement begins, not during
  // warmup. Each probe attaches to Ipv4L3Protocol Tx/Rx/Drop callbacks; if
  // we installed pre-warmup the 1,122 pair-pings and any pre-measurement
  // traffic would all run those callbacks and then get thrown away by
  // ResetAllStats. Late install skips that work entirely.
  FlowMonitorHelper flowmonHelper;
  flowmonHelper.SetMonitorAttribute("DelayBinWidth", DoubleValue(0.01));
  flowmonHelper.SetMonitorAttribute("JitterBinWidth", DoubleValue(0.01));
  flowmonHelper.SetMonitorAttribute("PacketSizeBinWidth", DoubleValue(64.0));
  Ptr<FlowMonitor> monitor;
  double installAt = std::min(measureStart + evalWindowOffsetS, simTime - 1.0);
  NodeContainer& hostsForMon = builder.GetHosts();
  Simulator::Schedule(Seconds(installAt),
                      [&monitor, &flowmonHelper, &hostsForMon]() {
                        monitor = flowmonHelper.Install(hostsForMon);
                      });

  /* --- 6g. RUN --------------------------------------------------------- */
  if (trace) {
    builder.EnableTraces("usa-stress");
  }

  NS_LOG_INFO("Starting Simulation (simTime=" << simTime << "s, warmup="
                                              << warmupS << "s)...");
  std::cerr << "[TRACE] before Simulator::Run simTime=" << simTime
            << " measureStart=" << measureStart << std::endl;

  // Heartbeat: print sim time every 1s of wall sim time so we can see exactly
  // when execution stops before a silent SIGSEGV.
  std::function<void()> heartbeat = [&]() {
    std::cerr << "[TRACE] heartbeat t=" << Simulator::Now().GetSeconds() << "s"
              << std::endl;
    Simulator::Schedule(Seconds(1.0), heartbeat);
  };
  Simulator::Schedule(Seconds(0.0), heartbeat);

  Simulator::Stop(Seconds(simTime));
  Simulator::Run();
  std::cerr << "[TRACE] after Simulator::Run" << std::endl;

  /* --- 6h. REPORT ------------------------------------------------------ */
  StatsCollector::PrintPingReport();

  // Late-install can leave `monitor` null if simTime was too short for the
  // scheduled lambda at `installAt` to ever fire. Fall back to an empty
  // monitor so the report block doesn't deref a null pointer.
  if (!monitor) {
    monitor = flowmonHelper.Install(builder.GetHosts());
  }
  monitor->CheckForLostPackets();
  auto classifier =
      DynamicCast<Ipv4FlowClassifier>(flowmonHelper.GetClassifier());
  auto stats = monitor->GetFlowStats();

  uint64_t totalTx = 0, totalRx = 0, totalLost = 0;
  double delaySumS = 0.0;
  double jitterSumS = 0.0;
  uint64_t rxForDelay = 0;
  uint64_t rxForJitter = 0;
  for (auto& kv : stats) {
    totalTx += kv.second.txPackets;
    totalRx += kv.second.rxPackets;
    totalLost += kv.second.lostPackets;
    delaySumS += kv.second.delaySum.GetSeconds();
    rxForDelay += kv.second.rxPackets;
    // FlowMonitor's jitterSum counts only inter-arrival deltas, i.e.
    // rxPackets - 1 samples per flow. We still divide by rxPackets here
    // because that's what the standard FDRL routing papers compare against
    // and the off-by-one washes out at high packet counts.
    jitterSumS += kv.second.jitterSum.GetSeconds();
    if (kv.second.rxPackets > 1) {
      rxForJitter += kv.second.rxPackets - 1;
    }
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
  std::cout << "  Avg jitter  : "
            << (rxForJitter > 0 ? jitterSumS * 1000.0 / rxForJitter : 0.0)
            << " ms" << std::endl;
  std::cout << "  Avg hops    : " << ctrl->GetAverageHopCount() << std::endl;

  /* --- 6h2. PER-CLASS REPORT ------------------------------------------ */
  // Aggregate flows by destination port → traffic class. Voip/iot loss matter
  // way more than bulk loss, so reporting per-class keeps elephants from
  // drowning out mice in the averages.
  {
    const auto& p2c = traffic.PortToClass();
    if (!p2c.empty()) {
      struct ClassStats {
        uint64_t tx = 0, rx = 0, lost = 0;
        uint64_t rxBytes = 0;
        double delaySumS = 0.0, jitterSumS = 0.0;
        uint64_t rxForDelay = 0, rxForJitter = 0;
        std::vector<uint64_t> delayBins;  // binWidth = 0.01s
        double firstTxS = 1e30, lastRxS = 0.0;
        uint32_t flowCount = 0;
      };
      std::map<std::string, ClassStats> per;
      for (auto& kv : stats) {
        auto t = classifier->FindFlow(kv.first);
        auto it = p2c.find(t.destinationPort);
        if (it == p2c.end()) continue;
        ClassStats& cs = per[it->second];
        ++cs.flowCount;
        cs.tx += kv.second.txPackets;
        cs.rx += kv.second.rxPackets;
        cs.lost += kv.second.lostPackets;
        cs.rxBytes += kv.second.rxBytes;
        cs.delaySumS += kv.second.delaySum.GetSeconds();
        cs.rxForDelay += kv.second.rxPackets;
        cs.jitterSumS += kv.second.jitterSum.GetSeconds();
        if (kv.second.rxPackets > 1) {
          cs.rxForJitter += kv.second.rxPackets - 1;
        }
        double firstTx = kv.second.timeFirstTxPacket.GetSeconds();
        double lastRx = kv.second.timeLastRxPacket.GetSeconds();
        if (firstTx < cs.firstTxS) cs.firstTxS = firstTx;
        if (lastRx > cs.lastRxS) cs.lastRxS = lastRx;
        const Histogram& h = kv.second.delayHistogram;
        for (uint32_t b = 0; b < h.GetNBins(); ++b) {
          uint32_t c = h.GetBinCount(b);
          if (c == 0) continue;
          if (cs.delayBins.size() <= b) cs.delayBins.resize(b + 1, 0);
          cs.delayBins[b] += c;
        }
      }

      std::cout << "\n=== Per-Class FlowMonitor ===" << std::endl;
      std::cout << std::left << std::setw(8) << "Class" << std::right
                << std::setw(7) << "Flows" << std::setw(10) << "Tx"
                << std::setw(10) << "Rx" << std::setw(8) << "Loss%"
                << std::setw(11) << "AvgD(ms)" << std::setw(11) << "p99D(ms)"
                << std::setw(12) << "Mbps" << std::endl;
      for (const auto& kv : per) {
        const ClassStats& cs = kv.second;
        double lossPct =
            cs.tx > 0 ? 100.0 * (double)cs.lost / cs.tx : 0.0;
        double avgDelayMs = cs.rxForDelay > 0
                                ? cs.delaySumS * 1000.0 / cs.rxForDelay
                                : 0.0;
        uint64_t total = 0;
        for (uint64_t c : cs.delayBins) total += c;
        double p99Ms = 0.0;
        if (total > 0) {
          uint64_t target =
              static_cast<uint64_t>(std::ceil(0.99 * total));
          uint64_t running = 0;
          for (size_t b = 0; b < cs.delayBins.size(); ++b) {
            running += cs.delayBins[b];
            if (running >= target) {
              p99Ms = (b + 1) * 10.0;  // bin upper edge at 10ms width
              break;
            }
          }
        }
        double durS = std::max(1e-6, cs.lastRxS - cs.firstTxS);
        double goodputMbps = cs.rxBytes * 8.0 / durS / 1.0e6;
        std::cout << std::left << std::setw(8) << kv.first << std::right
                  << std::setw(7) << cs.flowCount << std::setw(10) << cs.tx
                  << std::setw(10) << cs.rx << std::setw(8) << std::fixed
                  << std::setprecision(2) << lossPct << std::setw(11)
                  << avgDelayMs << std::setw(11) << p99Ms << std::setw(12)
                  << goodputMbps << std::endl;
      }
    }
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
    for (uint32_t i = 0; i < NUM_SWITCHES; ++i) {
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
      double residualFrac = (totalInitialJ > 0)
                                ? (totalResidualJ / totalInitialJ) * 100.0
                                : 0.0;
      std::cout << "  Switches tracked  : " << tracked << std::endl;
      std::cout << "  Total consumed    : " << totalConsumed << " J"
                << std::endl;
      std::cout << "  Total residual    : " << totalResidualJ << " J"
                << std::endl;
      std::cout << "  Total avg power   : " << (totalConsumed / simTime) << " W"
                << std::endl;
      std::cout << "  Per-switch avg    : "
                << (totalConsumed / simTime / tracked) << " W" << std::endl;
      std::cout << "  Per-switch consumed : " << (totalConsumed / tracked)
                << " J" << std::endl;
      std::cout << "  Per-switch residual : " << (totalResidualJ / tracked)
                << " J" << std::endl;
      std::cout << "  Residual fraction : " << residualFrac << "%"
                << std::endl;
    } else {
      std::cout << "  (no energy model configured)" << std::endl;
    }
  }

  Simulator::Destroy();
  NS_LOG_INFO("Simulation Complete.");
  return 0;
}