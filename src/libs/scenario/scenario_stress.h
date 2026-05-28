#ifndef SCRATCH_SCENARIO_STRESS_H
#define SCRATCH_SCENARIO_STRESS_H

#include <vector>

#include "ns3/applications-module.h"
#include "ns3/command-line.h"
#include "ns3/internet-module.h"
#include "ns3/network-module.h"

namespace ns3 {

// CLI options owned by StressEvents. 0 sentinels mean "use topology default".
struct StressOptions {
  bool enabled = false;
  uint32_t flashCrowdDst = 0;
  uint32_t blackHoleSwitchIdx = 0;
  void Register(CommandLine& cmd);
};

class LinkController {
 public:
  struct State {
    Ptr<NetDevice> devA;
    Ptr<NetDevice> devB;
    double normalRate = 0.0;
  };

  static void BringDown(State* ls);
  static void BringUp(State* ls);
  static void Degrade(State* ls, double rate);
  static void SetErrorRate(Ptr<NetDevice> nd, double rate);
};

// Schedules gray failures, flash crowds, correlated outages, and a node
// black-hole at fractions of the measurement window.
class StressEvents {
 public:
  StressEvents(NodeContainer& hosts, NodeContainer& switches,
               Ipv4InterfaceContainer& ifaces,
               const std::vector<LinkController::State*>& failureLinks);

  // Schedule the default mix at fractions of the measurement window:
  //   0.10 gray-fail link[0] at 30% loss
  //   0.25 flash crowd (pre-installed up front) to dstHostFlash
  //   0.40 restore gray-failed link
  //   0.55 correlated outage on links[1] and [2]
  //   0.75 restore correlated outage
  //   0.85 black-hole on switch blackHoleSwitchIdx for 5 s
  void Schedule(double measureStart, double window, uint32_t dstHostFlash,
                uint32_t blackHoleSwitchIdx);

 private:
  NodeContainer& m_hosts;
  NodeContainer& m_switches;
  Ipv4InterfaceContainer& m_ifaces;
  std::vector<LinkController::State*> m_failureLinks;

  void PreInstallFlashCrowd(uint32_t dstHost, double dur, double rateMbps,
                            uint32_t numFlows, double startT);
  void BlackHoleOn(uint32_t switchIdx);
  void BlackHoleOff(uint32_t switchIdx);
};

}  // namespace ns3

#endif  // SCRATCH_SCENARIO_STRESS_H
