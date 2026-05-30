#ifndef SCRATCH_SCENARIO_STRESS_H
#define SCRATCH_SCENARIO_STRESS_H

#include <functional>
#include <vector>

#include "ns3/applications-module.h"
#include "ns3/command-line.h"
#include "ns3/internet-module.h"
#include "ns3/network-module.h"
#include "ns3/random-variable-stream.h"

namespace ns3 {

// CLI options owned by StressEvents. 0 sentinels mean "use topology default".
struct StressOptions {
  bool enabled = false;
  uint32_t flashCrowdDst = 0;
  uint32_t blackHoleSwitchIdx = 0;
  uint32_t killSwitchIdx = 0;  // IoT battery-drain / node-death target
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

// Schedules a seed-varied crisis suite — gray failures, flash crowds,
// correlated outages, a node black-hole, an IoT battery-drain death, and
// mobility-emulating link flaps — at jittered fractions of the measurement
// window. Timing and targets are drawn from a UniformRandomVariable that
// inherits the global RngSeedManager seed, so every --seed yields a different
// (but reproducible) crisis trace.
class StressEvents {
 public:
  StressEvents(NodeContainer& hosts, NodeContainer& switches,
               Ipv4InterfaceContainer& ifaces,
               const std::vector<LinkController::State*>& failureLinks);

  // Schedule the seed-varied mix across the measurement window. Nominal layout
  // (each fire time jittered ±, each target drawn from the relevant pool):
  //   ~0.10 gray-fail a random failure link at 30% loss, restore ~0.40
  //   ~0.25 flash crowd (pre-installed up front) to dstHostFlash
  //   ~0.55 correlated outage on two random failure links, restore ~0.75
  //   ~0.85 black-hole on blackHoleSwitchIdx for ~5 s
  //   ~0.65 battery-drain death of killSwitchIdx (via killFn → ForceDeplete)
  //   link-flap cadence across the window (mobility emulation)
  // killFn(switchIdx) is supplied by the runner and resolves the owning
  // controller to deplete that switch's energy; null disables the kill crisis.
  void Schedule(double measureStart, double window, uint32_t dstHostFlash,
                uint32_t blackHoleSwitchIdx, uint32_t killSwitchIdx,
                std::function<void(uint32_t)> killFn);

 private:
  NodeContainer& m_hosts;
  NodeContainer& m_switches;
  Ipv4InterfaceContainer& m_ifaces;
  std::vector<LinkController::State*> m_failureLinks;
  Ptr<UniformRandomVariable> m_rng;  // seeded via global RngSeedManager

  // Draw a window fraction = base + U(-jitter, +jitter), clamped to [lo, hi].
  double Frac(double base, double jitter, double lo, double hi);

  void PreInstallFlashCrowd(uint32_t dstHost, double dur, double rateMbps,
                            uint32_t numFlows, double startT);
  void BlackHoleOn(uint32_t switchIdx);
  void BlackHoleOff(uint32_t switchIdx);
  // Toggle one failure link down/up repeatedly across [startFrac, endFrac] of
  // the window to emulate a node drifting in and out of radio range.
  void ScheduleFlap(LinkController::State* link, double measureStart,
                    double window, double startFrac, double endFrac,
                    double downSecs, double periodSecs);
};

}  // namespace ns3

#endif  // SCRATCH_SCENARIO_STRESS_H
