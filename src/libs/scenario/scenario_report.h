#ifndef SCRATCH_SCENARIO_REPORT_H
#define SCRATCH_SCENARIO_REPORT_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "controller/zmq-openflow-controller.h"
#include "ns3/flow-monitor-helper.h"
#include "ns3/flow-monitor.h"
#include "ns3/ipv4-flow-classifier.h"
#include "scenario/scenario_topo.h"

namespace ns3 {

// Bundle of inputs a scenario gathers post-run for the report block. The
// scenario fills the required fields and leaves optional ones empty;
// PrintScenarioReports() prints only the sections it has data for.
struct ScenarioReportInputs {
  // Required.
  Ptr<FlowMonitor> monitor;
  Ptr<Ipv4FlowClassifier> classifier;
  const std::vector<Ptr<ZmqOpenFlowController>>* ctrls = nullptr;
  const std::vector<NodeProfile>* nodes = nullptr;
  double simTime = 0.0;

  // Single-controller mode: leave sections null + switchToSection empty.
  // Multi-controller mode: set both to enable per-section hop counts and
  // per-section energy attribution.
  const std::vector<SectionDef>* sections = nullptr;
  std::vector<int> switchToSection;

  // Optional: per-class FlowMonitor table only prints when portToClass is
  // non-empty (i.e. --mixedLoad was enabled and the mixed-load classes ran).
  const std::map<uint16_t, std::string>* portToClass = nullptr;
};

// Prints, in order:
//   - FlowMonitor summary (totals + Avg hops or Per-Controller Hop Counts)
//   - Per-Class FlowMonitor table (if portToClass non-empty)
//   - Switch Energy report
void PrintScenarioReports(const ScenarioReportInputs& in);

}  // namespace ns3

#endif  // SCRATCH_SCENARIO_REPORT_H
