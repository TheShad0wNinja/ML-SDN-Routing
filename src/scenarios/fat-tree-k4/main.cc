#include <ns3/applications-module.h>
#include <ns3/core-module.h>
#include <ns3/csma-module.h>
#include <ns3/flow-monitor-module.h>
#include <ns3/internet-apps-module.h>
#include <ns3/internet-module.h>
#include <ns3/network-module.h>
#include <ns3/ofswitch13-module.h>

#include <algorithm>
#include <iostream>
#include <numeric>
#include <vector>

#include "controller/zmq-openflow-controller.h"
#include "scenario/scenario_builder.h"
#include "scenario/scenario_cli.h"
#include "scenario/scenario_report.h"
#include "scenario/scenario_stress.h"
#include "scenario/scenario_topo.h"
#include "scenario/scenario_traffic.h"
#include "topology.h"

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("FatTreeK4");

int main(int argc, char* argv[]) {
  ScenarioOptions opts;
  // Defaults tuned for the smaller fat-tree topology (16 hosts vs USA's 34).
  opts.flashCrowdDst = 0;
  opts.blackHoleSwitchIdx = 4;  // first aggregation switch

  CommandLine cmd(__FILE__);
  RegisterScenarioCli(cmd, opts);
  cmd.Parse(argc, argv);

  if (opts.multiController) {
    std::cerr << "fat-tree-k4 has no section partition; "
                 "--multiController is unsupported.\n";
    return 1;
  }

  RngSeedManager::SetSeed(opts.seed);
  GlobalValue::Bind("SchedulerType", StringValue("ns3::MapScheduler"));

  Config::SetDefault("ns3::TcpSocket::SndBufSize", UintegerValue(131072));
  Config::SetDefault("ns3::TcpSocket::RcvBufSize", UintegerValue(131072));
  Config::SetDefault("ns3::TcpSocket::SegmentSize", UintegerValue(1448));
  Config::SetDefault("ns3::TcpSocket::InitialCwnd", UintegerValue(10));
  Config::SetDefault("ns3::TcpSocketBase::Sack", BooleanValue(true));
  Config::SetDefault("ns3::TcpL4Protocol::SocketType",
                     TypeIdValue(TcpCubic::GetTypeId()));
  Config::SetDefault("ns3::ArpCache::AliveTimeout", TimeValue(Seconds(86400)));
  Config::SetDefault("ns3::ArpCache::DeadTimeout", TimeValue(Seconds(86400)));

  if (opts.warmupS < 0.0) opts.warmupS = 0.0;
  if (opts.warmupS > opts.simTime - 5.0)
    opts.warmupS = std::max(0.0, opts.simTime - 5.0);

  TopoSpec topo = BuildFatTreeK4Spec(opts.backboneQueue);
  if (!opts.sectionNodes.empty()) {
    topo = FilterTopoSpecBySection(topo, ParseIndexCsv(opts.sectionNodes));
    if (topo.nodes.empty()) return 1;
  }
  const uint32_t NUM_SWITCHES = topo.nodes.size();
  const uint32_t NUM_HOSTS = topo.hostToSwitch.size();

  auto ctrl = CreateObject<ZmqOpenFlowController>();
  ctrl->SetMlConfig(opts.BuildMlConfig());
  std::vector<Ptr<ZmqOpenFlowController>> ctrls = {ctrl};
  std::vector<uint32_t> allSwitches(NUM_SWITCHES);
  std::iota(allSwitches.begin(), allSwitches.end(), 0);

  ScenarioBuilder builder;
  builder.CreateNodes(NUM_HOSTS, NUM_SWITCHES, 1);

  for (uint32_t h = 0; h < NUM_HOSTS; ++h) {
    uint32_t sw = topo.hostToSwitch[h];
    builder.InstallHost(h, sw, opts.edgeQueue);
    HostAnnotation ann;
    ann.name = (h < topo.hostNames.size() ? topo.hostNames[h]
                                          : topo.nodes[sw].name) + "-Host";
    ann.node_type = "host";
    ctrl->SetHostAnnotation(builder.GetHostInfos()[h].mac, ann);
  }
  for (const auto& spec : topo.links) {
    builder.AddBackboneLink(spec, spec.bufferSize);
  }
  std::vector<LinkController::State*> failureLinks = builder.GetFailureLinks();

  builder.SetupIpStack();
  builder.PrePopulateArp();
  builder.InstallOpenFlow(ctrls, {allSwitches});
  for (uint32_t i = 0; i < NUM_SWITCHES; ++i) {
    builder.ConfigureSwitch(i, topo.nodes[i], ctrl);
  }

  double measureStart = 1.0 + opts.warmupS;
  Simulator::Schedule(Seconds(opts.warmupS), [&]() {
    ctrl->PreInstallAllPaths(builder.GetHostInfos());
  });

  TrafficManager traffic(builder.GetHosts(), builder.GetHostIfaces(),
                         /*centralHostIdx=*/0);
  std::vector<TrafficClass> trafficClasses = {
      {"web",   0.50,  2.0,    3.0,   80,  true,  1448, false},
      {"video", 0.20,  8.0,   20.0, 8080,  true,  1448, false},
      {"voip",  0.15,  0.064, 15.0, 5060, false,   160,  true},
      {"bulk",  0.10, 10.0,   25.0,   21,  true,  1448, false},
      {"iot",   0.05,  0.064, 60.0, 1883, false,   512,  true},
  };
  if (opts.pingEnabled)
    traffic.InstallPings(measureStart, opts.simTime);
  if (opts.tcpEnabled)
    traffic.InstallMixedLoad(measureStart, opts.simTime, trafficClasses,
                             opts.trafficMode, opts.maxConcurrent,
                             opts.arrivalRateHz);

  StressEvents stress(builder.GetHosts(), builder.GetSwitches(),
                      builder.GetHostIfaces(), failureLinks);
  if (opts.failuresEnabled) {
    stress.Schedule(measureStart, opts.simTime - measureStart,
                    opts.flashCrowdDst, opts.blackHoleSwitchIdx);
  }

  FlowMonitorHelper flowmonHelper;
  flowmonHelper.SetMonitorAttribute("DelayBinWidth", DoubleValue(0.01));
  flowmonHelper.SetMonitorAttribute("JitterBinWidth", DoubleValue(0.01));
  flowmonHelper.SetMonitorAttribute("PacketSizeBinWidth", DoubleValue(64.0));
  Ptr<FlowMonitor> monitor;
  double installAt =
      std::min(measureStart + opts.evalWindowOffsetS, opts.simTime - 1.0);
  NodeContainer& hostsForMon = builder.GetHosts();
  Simulator::Schedule(Seconds(installAt),
                      [&monitor, &flowmonHelper, &hostsForMon]() {
                        monitor = flowmonHelper.Install(hostsForMon);
                      });

  if (opts.trace) builder.EnableTraces("fat-tree-k4");

  Simulator::Stop(Seconds(opts.simTime));
  Simulator::Run();

  StatsCollector::PrintPingReport();
  if (!monitor) monitor = flowmonHelper.Install(builder.GetHosts());

  ScenarioReportInputs ri;
  ri.monitor = monitor;
  ri.classifier =
      DynamicCast<Ipv4FlowClassifier>(flowmonHelper.GetClassifier());
  ri.ctrls = &ctrls;
  ri.nodes = &topo.nodes;
  ri.simTime = opts.simTime;
  ri.portToClass = &traffic.PortToClass();
  PrintScenarioReports(ri);

  Simulator::Destroy();
  return 0;
}
