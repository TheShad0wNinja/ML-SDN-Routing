#include "scenario/scenario_cli.h"

namespace ns3 {

MlConfig ScenarioOptions::BuildMlConfig() const {
  MlConfig cfg;
  cfg.enabled = mlEnabled;
  cfg.interval_s = mlIntervalS;
  cfg.action_scale = mlActionScale;
  cfg.action_scale_start = mlActionScaleStart;
  cfg.taper_ticks = mlTaperTicks;
  cfg.reward_alpha = mlAlpha;
  cfg.reward_beta = mlBeta;
  cfg.reward_gamma = mlGamma;
  cfg.reward_delta = mlDelta;
  cfg.reward_zeta = mlZeta;
  cfg.reward_eta = mlEta;
  cfg.reward_theta = mlTheta;
  cfg.delay_ref_ms = mlDelayRef;
  cfg.loss_ref_bps = mlLossRef;
  cfg.power_ref_w = mlPowerRef;
  cfg.explore = mlExplore;
  cfg.checkpoint_every_n_ticks = mlCheckpointEveryNTicks;
  cfg.resume = mlResume;
  cfg.seed = seed;
  cfg.endpoint = mlEndpoint;

  if (mlPriority == "throughput")
    cfg.priority_preset = MlConfig::MlPriority::THROUGHPUT;
  else if (mlPriority == "energy")
    cfg.priority_preset = MlConfig::MlPriority::ENERGY;
  else if (mlPriority == "custom")
    cfg.priority_preset = MlConfig::MlPriority::CUSTOM;
  else
    cfg.priority_preset = MlConfig::MlPriority::BALANCED;
  return cfg;
}

void RegisterScenarioCli(CommandLine& cmd, ScenarioOptions& o) {
  cmd.AddValue("trace", "Enable pcap and datapath stats traces", o.trace);
  cmd.AddValue("simTime", "Simulation duration (s)", o.simTime);
  cmd.AddValue("warmupS", "Pre-warmup window for flow installs (s)", o.warmupS);
  cmd.AddValue("seed", "Random seed", o.seed);

  cmd.AddValue("trafficMode", "Traffic: random, central, grouped",
               o.trafficMode);
  cmd.AddValue("ping", "Enable measurement pings", o.pingEnabled);
  cmd.AddValue("tcp", "Enable OnOff TCP background load", o.tcpEnabled);
  cmd.AddValue("failures", "Enable scheduled link churn", o.failuresEnabled);
  cmd.AddValue("maxConcurrent", "Hard cap on concurrent mixed-load flows",
               o.maxConcurrent);
  cmd.AddValue("arrivalRateHz", "Mean Poisson arrival rate for new flows",
               o.arrivalRateHz);
  cmd.AddValue("flashCrowdDst", "Host index targeted by the flash crowd",
               o.flashCrowdDst);
  cmd.AddValue("blackHoleSwitch", "Switch index that goes dark at 0.85 W",
               o.blackHoleSwitchIdx);

  cmd.AddValue("backboneQueue", "Backbone CSMA queue size", o.backboneQueue);
  cmd.AddValue("edgeQueue", "Edge CSMA queue size", o.edgeQueue);

  cmd.AddValue("multiController",
               "Run one ZmqOpenFlowController per section (requires "
               "topology.sections non-empty)",
               o.multiController);
  cmd.AddValue("mlPortBase",
               "Lowest ZMQ port; controller i dials "
               "tcp://127.0.0.1:(mlPortBase+i) in --multiController mode",
               o.mlPortBase);
  cmd.AddValue("sectionId",
               "Section/Local-Controller id, used for naming and logs",
               o.sectionId);
  cmd.AddValue("sectionNodes",
               "CSV of original switch indices this section simulates "
               "(empty = whole topology)",
               o.sectionNodes);

  cmd.AddValue("ml", "Enable FDRL agent", o.mlEnabled);
  cmd.AddValue("mlIntervalS", "Agent period (s)", o.mlIntervalS);
  cmd.AddValue("mlActionScale", "Final |dW| fraction (after taper)",
               o.mlActionScale);
  cmd.AddValue("mlActionScaleStart", "Initial |dW| fraction (during taper)",
               o.mlActionScaleStart);
  cmd.AddValue("mlTaperTicks", "Ticks over which action_scale tapers",
               o.mlTaperTicks);
  cmd.AddValue("mlPriority",
               "Reward preset: balanced | throughput | energy | custom",
               o.mlPriority);
  cmd.AddValue("mlAlpha", "Reward weight α (delay quality)", o.mlAlpha);
  cmd.AddValue("mlBeta", "Reward weight β (loss quality)", o.mlBeta);
  cmd.AddValue("mlGamma", "Reward weight γ (power-consumption penalty)",
               o.mlGamma);
  cmd.AddValue("mlDelta", "Reward weight δ (utilization penalty)", o.mlDelta);
  cmd.AddValue("mlZeta", "Reward weight ζ (active-switch footprint)", o.mlZeta);
  cmd.AddValue("mlEta", "Reward weight η (route-through-low-reserve penalty)",
               o.mlEta);
  cmd.AddValue("mlTheta", "Reward weight θ (residual-energy stddev)",
               o.mlTheta);
  cmd.AddValue("mlDelayRef", "Delay reference for normalization (ms)",
               o.mlDelayRef);
  cmd.AddValue("mlLossRef", "Loss reference for normalization (bps)",
               o.mlLossRef);
  cmd.AddValue("mlPowerRef", "Power reference for normalization (W)",
               o.mlPowerRef);
  cmd.AddValue("mlExplore", "Enable OU exploration & training updates",
               o.mlExplore);
  cmd.AddValue("mlCheckpointEveryNTicks", "Checkpoint cadence",
               o.mlCheckpointEveryNTicks);
  cmd.AddValue("mlResume", "Resume from checkpoint", o.mlResume);
  cmd.AddValue("mlEndpoint", "ZMQ endpoint (single-controller mode)",
               o.mlEndpoint);
  cmd.AddValue("evalWindowOffsetS",
               "Delay FlowMonitor reset by this many seconds past warmup "
               "(0 = report from warmup end)",
               o.evalWindowOffsetS);
}

}  // namespace ns3
