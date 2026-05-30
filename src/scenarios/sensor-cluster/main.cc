#include "ns3/core-module.h"

#include "scenario/scenario_cli.h"
#include "scenario/scenario_runner.h"
#include "topology.h"

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("SensorCluster");

int main(int argc, char* argv[]) {
  ScenarioOptions opts;
  CommandLine cmd(__FILE__);
  RegisterScenarioCli(cmd, opts);
  cmd.Parse(argc, argv);

  TopoSpec topo = BuildSensorClusterSpec(opts.backboneQueue);
  return RunScenario(opts, std::move(topo), "sensor-cluster");
}
