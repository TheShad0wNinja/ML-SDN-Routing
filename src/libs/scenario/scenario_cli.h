#ifndef SCRATCH_SCENARIO_CLI_H
#define SCRATCH_SCENARIO_CLI_H

#include <cstdint>
#include <string>

#include "ns3/command-line.h"
#include "scenario/scenario_ml.h"
#include "scenario/scenario_multictrl.h"
#include "scenario/scenario_stress.h"
#include "scenario/scenario_traffic.h"

namespace ns3 {

// Scenarios assemble their CLI by aggregating per-tool Options structs.
// Each tool owns its flags via its own Register() method; adding a new tool
// only requires defining a new Options struct and embedding it here — no
// edits to RegisterScenarioCli().
//
// Scenario-specific flags go in a derived struct's RegisterExtras() override:
//
//   struct UsaOpts : ScenarioOptions {
//     bool cripple = false;
//     void RegisterExtras(CommandLine& c) override {
//       c.AddValue("cripple", "Cripple Missoula node", cripple);
//     }
//   };
struct ScenarioOptions {
  bool trace = false;
  double simTime = 60.0;
  double warmupS = 5.0;
  uint32_t seed = 12345;
  std::string backboneQueue = "3MB";
  std::string edgeQueue = "500kB";
  double evalWindowOffsetS = 0.0;

  TrafficOptions  traffic;
  StressOptions   stress;
  MlOptions       ml;
  MultiCtrlOptions multi;

  virtual void RegisterExtras(CommandLine& /*cmd*/) {}
  virtual ~ScenarioOptions() = default;
};

// Registers run-control flags, then each module's own flags, then the
// scenario's extras. Single call per scenario.
void RegisterScenarioCli(CommandLine& cmd, ScenarioOptions& opts);

}  // namespace ns3

#endif  // SCRATCH_SCENARIO_CLI_H
