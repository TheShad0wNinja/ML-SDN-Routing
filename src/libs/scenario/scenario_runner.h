#ifndef SCRATCH_SCENARIO_RUNNER_H
#define SCRATCH_SCENARIO_RUNNER_H

#include <string>

#include "scenario/scenario_cli.h"
#include "scenario/scenario_topo.h"

namespace ns3 {

// Runs a scenario end-to-end given parsed options and a topology spec.
// Handles section normalization + validation, controller layout (single or
// multi-controller), host/link wiring, traffic, stress, FlowMonitor, and the
// report block. Scenarios reduce to: parse args, build topo, call this.
//
// Returns 0 on success, non-zero on validation errors (caller can return it
// from main()).
int RunScenario(ScenarioOptions& opts, TopoSpec topo,
                const std::string& tracePrefix);

}  // namespace ns3

#endif  // SCRATCH_SCENARIO_RUNNER_H
