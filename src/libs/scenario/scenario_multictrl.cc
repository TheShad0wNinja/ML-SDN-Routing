#include "scenario/scenario_multictrl.h"

#include <iostream>
#include <numeric>

namespace ns3 {

void MultiCtrlOptions::Register(CommandLine& cmd) {
  cmd.AddValue("multiController",
               "Run one ZmqOpenFlowController per section (requires "
               "topology.sections.size() > 1; auto-enables ml + resume)",
               enabled);
  cmd.AddValue("mlPortBase",
               "Lowest ZMQ port; controller i dials "
               "tcp://127.0.0.1:(mlPortBase+i) in --multiController mode",
               mlPortBase);
  cmd.AddValue("sections",
               "Expected partition count. 1 = full topology (default). "
               "N>1 must equal the scenario's defined section count.",
               sections);
  cmd.AddValue("sectionId",
               "Which section this worker simulates (federated phase-1, "
               "ignored when sections=1)",
               sectionId);
}

ControllerLayout SetupControllers(const MultiCtrlOptions& multi,
                                  const MlOptions& ml,
                                  const TopoSpec& topo,
                                  uint32_t baseSeed) {
  ControllerLayout out;
  const uint32_t numSwitches = topo.nodes.size();
  out.switchToSection.assign(numSwitches, -1);

  if (multi.enabled) {
    if (topo.sections.size() < 2) {
      std::cerr << "FATAL: --multiController requires the topology to define "
                   ">= 2 sections (got " << topo.sections.size() << ")\n";
      return out;
    }
    for (uint32_t i = 0; i < topo.sections.size(); ++i) {
      MlConfig cfg = ml.BuildMlConfig(baseSeed + i);
      cfg.controller_id = i;
      cfg.endpoint =
          "tcp://127.0.0.1:" + std::to_string(multi.mlPortBase + i);
      auto ctrl = CreateObject<ZmqOpenFlowController>();
      ctrl->SetMlConfig(cfg);
      out.ctrls.push_back(ctrl);
      out.switchesPerCtrl.push_back(topo.sections[i].nodes);
      for (uint32_t sw : topo.sections[i].nodes) {
        if (sw < numSwitches) out.switchToSection[sw] = static_cast<int>(i);
      }
    }
    for (uint32_t sw = 0; sw < numSwitches; ++sw) {
      if (out.switchToSection[sw] < 0) {
        std::cerr << "FATAL: switch " << sw
                  << " not assigned to any section\n";
        out.ctrls.clear();
        return out;
      }
    }
  } else {
    auto ctrl = CreateObject<ZmqOpenFlowController>();
    ctrl->SetMlConfig(ml.BuildMlConfig(baseSeed));
    out.ctrls.push_back(ctrl);
    std::vector<uint32_t> all(numSwitches);
    std::iota(all.begin(), all.end(), 0);
    out.switchesPerCtrl.push_back(std::move(all));
    for (uint32_t sw = 0; sw < numSwitches; ++sw)
      out.switchToSection[sw] = 0;
  }
  return out;
}

}  // namespace ns3
