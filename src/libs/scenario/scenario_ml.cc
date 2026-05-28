#include "scenario/scenario_ml.h"

namespace ns3 {

void MlOptions::Register(CommandLine& cmd) {
  cmd.AddValue("ml", "Enable FDRL agent", enabled);
  cmd.AddValue("mlIntervalS", "Agent period (s)", intervalS);
  cmd.AddValue("mlActionScale", "Final |dW| fraction (after taper)",
               actionScale);
  cmd.AddValue("mlActionScaleStart",
               "Initial |dW| fraction (during taper)", actionScaleStart);
  cmd.AddValue("mlTaperTicks", "Ticks over which action_scale tapers",
               taperTicks);
  cmd.AddValue("mlPriority",
               "Reward preset: balanced | throughput | energy | custom",
               priority);
  cmd.AddValue("mlAlpha", "Reward weight α (delay quality)", alpha);
  cmd.AddValue("mlBeta",  "Reward weight β (loss quality)",  beta);
  cmd.AddValue("mlGamma", "Reward weight γ (power-consumption penalty)",
               gamma);
  cmd.AddValue("mlDelta", "Reward weight δ (utilization penalty)", delta);
  cmd.AddValue("mlZeta",  "Reward weight ζ (active-switch footprint)", zeta);
  cmd.AddValue("mlEta",   "Reward weight η (route-through-low-reserve penalty)",
               eta);
  cmd.AddValue("mlTheta", "Reward weight θ (residual-energy stddev)", theta);
  cmd.AddValue("mlKappa", "Reward weight κ (route-churn penalty)", kappa);
  cmd.AddValue("mlDelayRef", "Delay reference for normalization (ms)",
               delayRefMs);
  cmd.AddValue("mlLossRef",  "Loss reference for normalization (bps)",
               lossRefBps);
  cmd.AddValue("mlPowerRef", "Power reference for normalization (W)",
               powerRefW);
  cmd.AddValue("mlExplore",
               "Enable Gaussian action noise", explore);
  cmd.AddValue("mlLearn",
               "Enable gradient updates / train_step", learn);
  cmd.AddValue("mlCheckpointEveryNTicks", "Checkpoint cadence",
               checkpointEveryNTicks);
  cmd.AddValue("mlResume", "Resume from checkpoint", resume);
  cmd.AddValue("mlEndpoint", "ZMQ endpoint (single-controller mode)",
               endpoint);
}

MlConfig MlOptions::BuildMlConfig(uint32_t seedIn) const {
  MlConfig cfg;
  cfg.enabled = enabled;
  cfg.interval_s = intervalS;
  cfg.action_scale = actionScale;
  cfg.action_scale_start = actionScaleStart;
  cfg.taper_ticks = taperTicks;
  cfg.reward_alpha = alpha;
  cfg.reward_beta = beta;
  cfg.reward_gamma = gamma;
  cfg.reward_delta = delta;
  cfg.reward_zeta = zeta;
  cfg.reward_eta = eta;
  cfg.reward_theta = theta;
  cfg.reward_kappa = kappa;
  cfg.delay_ref_ms = delayRefMs;
  cfg.loss_ref_bps = lossRefBps;
  cfg.power_ref_w = powerRefW;
  cfg.explore = explore;
  cfg.learn = learn;
  cfg.checkpoint_every_n_ticks = checkpointEveryNTicks;
  cfg.resume = resume;
  cfg.seed = seedIn;
  cfg.endpoint = endpoint;

  if (priority == "throughput")
    cfg.priority_preset = MlConfig::MlPriority::THROUGHPUT;
  else if (priority == "energy")
    cfg.priority_preset = MlConfig::MlPriority::ENERGY;
  else if (priority == "custom")
    cfg.priority_preset = MlConfig::MlPriority::CUSTOM;
  else
    cfg.priority_preset = MlConfig::MlPriority::BALANCED;
  return cfg;
}

}  // namespace ns3
