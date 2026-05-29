#ifndef SCRATCH_SCENARIO_ML_H
#define SCRATCH_SCENARIO_ML_H

#include <cstdint>
#include <string>

#include "controller/zmq-openflow-controller.h"
#include "ns3/command-line.h"

namespace ns3 {

// CLI options owned by the ML/FDRL module. Aggregated into ScenarioOptions.
struct MlOptions {
  bool enabled = false;
  double intervalS = 1.0;
  double actionScale = 0.20;
  double actionScaleStart = 0.40;
  uint32_t taperTicks = 200;
  std::string priority = "balanced";
  double alpha = 1.0, beta = 2.0, gamma = 1.5, delta = 1.0;
  double zeta = 0.5, eta = 1.5, theta = 1.0, kappa = 1.0;
  double delayRefMs = 200.0, lossRefBps = 1.0e6, powerRefW = 100.0;
  bool explore = true;
  bool learn = true;
  uint32_t checkpointEveryNTicks = 60;
  bool resume = true;
  std::string endpoint = "tcp://127.0.0.1:5555";
  // Pin Python agent's exploration sigma; negative = use default decay.
  double noiseSigmaInit = -1.0;

  void Register(CommandLine& cmd);

  // Build a runtime MlConfig. `seed` is the base seed; callers in
  // multi-controller mode add the controller index for diverse exploration.
  MlConfig BuildMlConfig(uint32_t seed) const;
};

}  // namespace ns3

#endif  // SCRATCH_SCENARIO_ML_H
