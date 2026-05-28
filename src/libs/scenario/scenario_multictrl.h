#ifndef SCRATCH_SCENARIO_MULTICTRL_H
#define SCRATCH_SCENARIO_MULTICTRL_H

#include <cstdint>
#include <string>
#include <vector>

#include "controller/zmq-openflow-controller.h"
#include "ns3/command-line.h"
#include "scenario/scenario_ml.h"
#include "scenario/scenario_topo.h"

namespace ns3 {

// CLI options owned by the multi-controller (federated) module.
struct MultiCtrlOptions {
  bool enabled = false;       // --multiController: M in-process controllers
  uint16_t mlPortBase = 5555;
  // sections=1 means "no partition, run the full topology". sections>1 must
  // match topo.sections.size() exactly; the scenario errors out otherwise.
  uint32_t sections = 1;
  // Which section this worker simulates (federated phase-1). Ignored when
  // sections==1.
  uint32_t sectionId = 0;
  void Register(CommandLine& cmd);
};

// Result of SetupControllers: parallel arrays of controllers and the
// switches each one owns, plus a switch→section map for the report block.
struct ControllerLayout {
  std::vector<Ptr<ZmqOpenFlowController>> ctrls;
  std::vector<std::vector<uint32_t>> switchesPerCtrl;
  std::vector<int> switchToSection;  // size = topo.nodes.size()
};

// Build the controller layout for a given topology + options. Handles both
// the single-controller path (one ctrl, all switches) and the
// multi-controller path (one ctrl per section, distinct ZMQ endpoints and
// seed offsets). Returns an empty layout on validation failure (caller checks
// .ctrls.empty()).
ControllerLayout SetupControllers(const MultiCtrlOptions& multi,
                                  const MlOptions& ml,
                                  const TopoSpec& topo,
                                  uint32_t baseSeed);

}  // namespace ns3

#endif  // SCRATCH_SCENARIO_MULTICTRL_H
