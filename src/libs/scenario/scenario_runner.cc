#include "scenario/scenario_runner.h"

#include <algorithm>
#include <iostream>

#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/csma-module.h"
#include "ns3/flow-monitor-module.h"
#include "ns3/internet-apps-module.h"
#include "ns3/internet-module.h"
#include "ns3/network-module.h"
#include "ns3/ofswitch13-module.h"

#include "controller/zmq-openflow-controller.h"
#include "scenario/scenario_builder.h"
#include "scenario/scenario_multictrl.h"
#include "scenario/scenario_report.h"
#include "scenario/scenario_stress.h"
#include "scenario/scenario_topo.h"
#include "scenario/scenario_traffic.h"

namespace ns3 {

namespace {

uint32_t Resolve(uint32_t cliValue, uint32_t topoDefault) {
  return cliValue != 0 ? cliValue : topoDefault;
}

}  // namespace

int RunScenario(ScenarioOptions& opts, TopoSpec topo,
                const std::string& tracePrefix) {
  NormalizeTopoSpec(topo);

  // --multiController auto-enables ML + resume so the M in-process controllers
  // load their trained FedAvg weights without forcing the user to spell out
  // both flags.
  if (opts.multi.enabled) {
    if (!opts.ml.enabled) {
      std::cout << "[runner] --multiController auto-enabling --ml\n";
      opts.ml.enabled = true;
    }
    if (!opts.ml.resume) {
      std::cout << "[runner] --multiController auto-enabling --mlResume\n";
      opts.ml.resume = true;
    }
  }

  // --multiController without an explicit --sections defaults to the
  // topology's full section count.
  if (opts.multi.enabled && opts.multi.sections == 1) {
    if (topo.sections.size() < 2) {
      std::cerr << "FATAL: --multiController requires the topology to define "
                   ">= 2 sections (topology '" << topo.label << "' has "
                << topo.sections.size() << ")\n";
      return 1;
    }
    opts.multi.sections = topo.sections.size();
    std::cout << "[runner] --multiController inferred --sections="
              << opts.multi.sections << " from topology\n";
  }

  // sections=1 always works on any topology: ignore the topo's native
  // partition and run as a single-section, single-controller scenario.
  // sections>1 must match the topology's declared partition exactly.
  if (opts.multi.sections == 1) {
    SectionDef whole;
    whole.id = 0;
    whole.name = "all";
    whole.nodes.resize(topo.nodes.size());
    for (uint32_t i = 0; i < topo.nodes.size(); ++i) whole.nodes[i] = i;
    topo.sections = {whole};
    topo.interDomainRoutes.clear();
  } else if (opts.multi.sections != topo.sections.size()) {
    std::cerr << "FATAL: --sections=" << opts.multi.sections
              << " does not match topology '" << topo.label << "' which has "
              << topo.sections.size() << " section(s)\n";
    return 1;
  }

  RngSeedManager::SetSeed(opts.seed);
  GlobalValue::Bind("SchedulerType", StringValue("ns3::MapScheduler"));
  Config::SetDefault("ns3::TcpL4Protocol::SocketType",
                     TypeIdValue(TcpCubic::GetTypeId()));
  Config::SetDefault("ns3::TcpSocket::SegmentSize", UintegerValue(8940));

  if (opts.warmupS < 0.0) opts.warmupS = 0.0;
  if (opts.warmupS > opts.simTime - 5.0)
    opts.warmupS = std::max(0.0, opts.simTime - 5.0);

  // Phase-1 federated worker: filter the topology down to this worker's
  // section. Only applies when sections>1 and we're NOT in multi-controller
  // mode (multi-controller keeps the full topology and runs M ctrls in-proc).
  if (!opts.multi.enabled && opts.multi.sections > 1) {
    if (opts.multi.sectionId >= topo.sections.size()) {
      std::cerr << "FATAL: --sectionId=" << opts.multi.sectionId
                << " out of range (topology has " << topo.sections.size()
                << " sections)\n";
      return 1;
    }
    std::cout << "[SECTION] sectionId=" << opts.multi.sectionId
              << " — filtering to " << topo.sections[opts.multi.sectionId].name
              << "\n";
    topo = FilterTopoSpecBySection(
        topo, topo.sections[opts.multi.sectionId].nodes);
    if (topo.nodes.empty()) {
      std::cerr << "[SECTION] FATAL: filtered topology has no switches\n";
      return 1;
    }
  }

  const uint32_t NUM_SWITCHES = topo.nodes.size();
  const uint32_t NUM_HOSTS = topo.hostToSwitch.size();

  ControllerLayout layout =
      SetupControllers(opts.multi, opts.ml, topo, opts.seed);
  if (layout.ctrls.empty()) return 1;
  auto& ctrls = layout.ctrls;
  auto& switchToSection = layout.switchToSection;

  ScenarioBuilder builder;
  builder.CreateNodes(NUM_HOSTS, NUM_SWITCHES, ctrls.size());

  for (uint32_t h = 0; h < NUM_HOSTS; ++h) {
    uint32_t sw = topo.hostToSwitch[h];
    builder.InstallHost(h, sw, opts.edgeQueue);
    HostAnnotation ann;
    ann.name = (h < topo.hostNames.size() ? topo.hostNames[h]
                                          : topo.nodes[sw].name) + "-Host";
    ann.node_type = "host";
    ctrls[switchToSection[sw]]->SetHostAnnotation(
        builder.GetHostInfos()[h].mac, ann);
    // dpid convention is switchIndex + 1 (see ScenarioBuilder::ConfigureSwitch).
    // Protect this switch from the node-sleep action — sleeping a host's access
    // switch would strand the host.
    ctrls[switchToSection[sw]]->MarkHostSwitch(sw + 1);
  }
  for (const auto& spec : topo.links) {
    builder.AddBackboneLink(spec, spec.bufferSize);
  }
  std::vector<LinkController::State*> failureLinks = builder.GetFailureLinks();

  builder.SetupIpStack();
  builder.InstallOpenFlow(ctrls, layout.switchesPerCtrl);
  for (uint32_t i = 0; i < NUM_SWITCHES; ++i) {
    builder.ConfigureSwitch(i, topo.nodes[i], ctrls[switchToSection[i]]);
  }

  double measureStart = 1.0 + opts.warmupS;

  // Single-controller runs are fully reactive: hosts ARP normally, the
  // controller proxy-ARPs / floods to learn host locations, and installs
  // shortest-path flow-mods on the first packet of each flow using the
  // LLDP-discovered link costs current at that moment (see HandlePacketIn /
  // ForwardPacket). Nothing is pre-installed — the warmup window simply gives
  // LLDP time to discover the full topology before traffic starts at
  // measureStart, so the reactive paths are computed on the settled cost graph.
  //
  // multiController is different: each in-process controller only sees its own
  // section, so cross-domain reachability can't be learned reactively. There we
  // still proactively seed intra-section paths and install static inter-domain
  // border routes after warmup. (Capture by reference: all locals outlive Run.)
  if (opts.multi.enabled) {
    Simulator::Schedule(Seconds(opts.warmupS), [&]() {
      const uint32_t M = topo.sections.size();
      std::vector<std::vector<ZmqOpenFlowController::HostInfo>> intra(M);
      for (uint32_t h = 0; h < NUM_HOSTS; ++h) {
        intra[switchToSection[topo.hostToSwitch[h]]].push_back(
            builder.GetHostInfos()[h]);
      }
      for (uint32_t s = 0; s < M; ++s) {
        ctrls[s]->PreInstallAllPaths(intra[s]);
      }
      for (uint32_t s = 0; s < M; ++s) {
        std::vector<ZmqOpenFlowController::ExternalHostRoute> routes;
        for (const auto& r : topo.interDomainRoutes) {
          if (r.fromSection != s) continue;
          uint64_t srcDpid = r.viaSwitch + 1;
          uint64_t dstDpid = r.nextSwitch + 1;
          uint32_t borderOutPort = builder.PortBetween(srcDpid, dstDpid);
          if (borderOutPort == 0) {
            std::cerr << "WARN: no physical link " << r.viaSwitch << "→"
                      << r.nextSwitch << "; skipping route " << r.fromSection
                      << "→" << r.toSection << "\n";
            continue;
          }
          for (uint32_t h = 0; h < NUM_HOSTS; ++h) {
            if (switchToSection[topo.hostToSwitch[h]] !=
                static_cast<int>(r.toSection))
              continue;
            routes.push_back({builder.GetHostInfos()[h].mac, srcDpid,
                              borderOutPort});
          }
        }
        ctrls[s]->InstallExternalHostRoutes(routes);
      }
    });
  }

  uint32_t centralHostIdx =
      Resolve(opts.traffic.centralHostIdx, topo.defaultCentralHost);
  uint32_t flashCrowdDst =
      Resolve(opts.stress.flashCrowdDst, topo.defaultFlashCrowdDst);
  uint32_t blackHoleSwitchIdx =
      Resolve(opts.stress.blackHoleSwitchIdx, topo.defaultBlackHoleSwitch);
  uint32_t killSwitchIdx =
      Resolve(opts.stress.killSwitchIdx, topo.defaultKillSwitch);

  TrafficManager traffic(builder.GetHosts(), builder.GetHostIfaces(),
                         centralHostIdx, topo.hostGroups);
  std::vector<TrafficClass> trafficClasses =
      opts.traffic.classes.empty() ? TrafficOptions::DefaultClasses()
                                   : opts.traffic.classes;
  if (opts.traffic.ping) traffic.InstallPings(measureStart, opts.simTime);
  if (opts.traffic.mixedLoad)
    traffic.InstallMixedLoad(measureStart, opts.simTime, trafficClasses,
                             opts.traffic.mode, opts.traffic.maxConcurrent,
                             opts.traffic.arrivalRateHz);

  StressEvents stress(builder.GetHosts(), builder.GetSwitches(),
                      builder.GetHostIfaces(), failureLinks);
  if (opts.stress.enabled) {
    // Resolve the kill target's owning controller; dpid convention is
    // switchIndex + 1 (see ScenarioBuilder::ConfigureSwitch). The lambda is
    // fired by StressEvents at the (seed-jittered) crisis time.
    auto killFn = [&ctrls, &switchToSection,
                   NUM_SWITCHES](uint32_t switchIdx) {
      if (switchIdx >= NUM_SWITCHES) return;
      ctrls[switchToSection[switchIdx]]->ForceDeplete(switchIdx + 1);
    };
    stress.Schedule(measureStart, opts.simTime - measureStart, flashCrowdDst,
                    blackHoleSwitchIdx, killSwitchIdx, killFn);
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

  if (opts.trace) builder.EnableTraces(tracePrefix);

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
  if (opts.multi.enabled) {
    ri.sections = &topo.sections;
    ri.switchToSection = switchToSection;
  }
  PrintScenarioReports(ri);

  Simulator::Destroy();
  return 0;
}

}  // namespace ns3
