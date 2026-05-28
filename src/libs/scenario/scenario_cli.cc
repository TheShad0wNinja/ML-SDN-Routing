#include "scenario/scenario_cli.h"

namespace ns3 {

void RegisterScenarioCli(CommandLine& cmd, ScenarioOptions& o) {
  cmd.AddValue("trace", "Enable pcap and datapath stats traces", o.trace);
  cmd.AddValue("simTime", "Simulation duration (s)", o.simTime);
  cmd.AddValue("warmupS", "Pre-warmup window for flow installs (s)", o.warmupS);
  cmd.AddValue("seed", "Random seed", o.seed);
  cmd.AddValue("backboneQueue", "Backbone CSMA queue size", o.backboneQueue);
  cmd.AddValue("edgeQueue", "Edge CSMA queue size", o.edgeQueue);
  cmd.AddValue("evalWindowOffsetS",
               "Delay FlowMonitor reset by this many seconds past warmup "
               "(0 = report from warmup end)",
               o.evalWindowOffsetS);

  o.traffic.Register(cmd);
  o.stress.Register(cmd);
  o.ml.Register(cmd);
  o.multi.Register(cmd);
  o.RegisterExtras(cmd);
}

}  // namespace ns3
