#ifndef SCRATCH_SCENARIO_CLI_H
#define SCRATCH_SCENARIO_CLI_H

#include <cstdint>
#include <string>

#include "controller/zmq-openflow-controller.h"
#include "ns3/command-line.h"

namespace ns3 {

// Common CLI knobs for scenario binaries. Scenario-specific flags
struct ScenarioOptions {
  // run-control
  bool trace = false;
  double simTime = 60.0;
  double warmupS = 5.0;
  uint32_t seed = 12345;

  // traffic
  std::string trafficMode = "random";
  bool pingEnabled = true;
  bool tcpEnabled = false;
  bool failuresEnabled = false;
  uint32_t maxConcurrent = 60;
  double arrivalRateHz = 8.0;
  uint32_t flashCrowdDst = 15;      
  uint32_t blackHoleSwitchIdx = 22;

  // queues
  std::string backboneQueue = "3MB";
  std::string edgeQueue = "500kB";

  // single/multi controller
  bool multiController = false;
  uint16_t mlPortBase = 5555;
  uint32_t sectionId = 0;
  std::string sectionNodes;

  // ML
  bool mlEnabled = false;
  double mlIntervalS = 1.0;
  double mlActionScale = 0.20;
  double mlActionScaleStart = 0.40;
  uint32_t mlTaperTicks = 200;
  std::string mlPriority = "balanced";
  double mlAlpha = 1.0;
  double mlBeta = 2.0;
  double mlGamma = 1.5;
  double mlDelta = 1.0;
  double mlZeta = 0.5;
  double mlEta = 1.5;
  double mlTheta = 1.0;
  double mlDelayRef = 200.0;
  double mlLossRef = 1.0e6;
  double mlPowerRef = 90000.0;
  bool mlExplore = true;
  uint32_t mlCheckpointEveryNTicks = 60;
  bool mlResume = true;
  std::string mlEndpoint = "tcp://127.0.0.1:5555";
  double evalWindowOffsetS = 0.0;

  // Builds an MlConfig ready to hand to a controller. Resolves mlPriority
  // string → enum.
  MlConfig BuildMlConfig() const;
};

// Registers all the fields above with the CLI. Scenarios add their own
// flags before or after this call.
void RegisterScenarioCli(CommandLine& cmd, ScenarioOptions& opts);

}  // namespace ns3

#endif  // SCRATCH_SCENARIO_CLI_H
