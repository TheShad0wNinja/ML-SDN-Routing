#include "ns3/core-module.h"

#include "scenario/scenario_cli.h"
#include "scenario/scenario_runner.h"
#include "topologies.h"

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("USA");

namespace {
struct UsaOptions : ScenarioOptions {
  bool cripple = false;
  void RegisterExtras(CommandLine& cmd) override {
    cmd.AddValue("cripple",
                 "Cripple Missoula node (1Mbps CPU, 100us TCAM)", cripple);
  }
};
}  // namespace

int main(int argc, char* argv[]) {
  UsaOptions opts;
  CommandLine cmd(__FILE__);
  RegisterScenarioCli(cmd, opts);
  cmd.Parse(argc, argv);

  TopoSpec topo = BuildUsaSpec(opts.backboneQueue, opts.cripple);
  return RunScenario(opts, std::move(topo), "usa-stress");
}
