#ifndef SCRATCH_SCENARIO_TRAFFIC_H
#define SCRATCH_SCENARIO_TRAFFIC_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "ns3/applications-module.h"
#include "ns3/command-line.h"
#include "ns3/internet-module.h"
#include "ns3/network-module.h"

namespace ns3 {

class StatsCollector {
 public:
  static uint64_t g_pingTx;
  static uint64_t g_pingRx;
  static double g_rttSumMs;

  static void PingTxCallback(uint16_t seq, Ptr<Packet> p);
  static void PingRttCallback(uint16_t seq, Time rtt);
  static void PrintPingReport();
};

struct TrafficClass {
  std::string name;
  double weight = 0.0;       // selection probability; weights sum ~1.0
  double meanRateMbps = 0.0;
  double meanDurS = 1.0;
  uint16_t port = 0;
  bool isTcp = true;
  uint32_t pktSize = 1448;
  bool cbr = false;
};

// CLI options owned by the TrafficManager module. Aggregated into
// ScenarioOptions and registered automatically.
struct TrafficOptions {
  std::string mode = "random";   // random | central | grouped
  bool ping = true;
  bool mixedLoad = false;
  uint32_t maxConcurrent = 30;
  double arrivalRateHz = 4.0;
  // centralHostIdx: 0 = use the topology's defaultCentralHost. Override on the
  // CLI to force a specific host index.
  uint32_t centralHostIdx = 0;

  // Programmatic override for the class mix (no CLI flag — scenarios set
  // this before parse if they want a different workload). Empty = use
  // DefaultClasses().
  std::vector<TrafficClass> classes;

  // Default class mix: web/video/voip/bulk/iot — a generic five-class
  // workload that's reasonable across topologies.
  static std::vector<TrafficClass> DefaultClasses();

  void Register(CommandLine& cmd);
};

class TrafficManager {
 public:
  // centralHostIdx is the destination used by trafficMode="central". Pass the
  // resolved index (topology default or CLI override) here.
  // hostGroups (optional, parallel to hosts): per-host group id used by
  // trafficMode="grouped". Empty or all-equal → grouped degrades to random.
  TrafficManager(NodeContainer& hosts, Ipv4InterfaceContainer& ifaces,
                 uint32_t centralHostIdx,
                 std::vector<uint32_t> hostGroups = {});

  void WarmupFlows(double startTime, double durationS);
  void InstallPings(double startTime, double simTime);
  void InstallMixedLoad(double startTime, double simTime,
                        const std::vector<TrafficClass>& classes,
                        const std::string& trafficMode, uint32_t maxConcurrent,
                        double arrivalRateHz);

  const std::map<uint16_t, std::string>& PortToClass() const {
    return m_portToClass;
  }

 private:
  uint32_t PickDestination(uint32_t src, uint32_t n, const std::string& mode);

  NodeContainer& m_hosts;
  Ipv4InterfaceContainer& m_ifaces;
  uint32_t m_centralHostIdx;
  std::vector<uint32_t> m_hostGroups;
  // For each group id, the list of host indices in that group. Built lazily
  // from m_hostGroups in PickDestination's grouped branch.
  std::vector<std::vector<uint32_t>> m_groupMembers;
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
};

}  // namespace ns3

#endif  // SCRATCH_SCENARIO_TRAFFIC_H
