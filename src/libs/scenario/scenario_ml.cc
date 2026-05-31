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
  cmd.AddValue("mlPowerRef", "Power reference for normalization (W)",
               powerRefW);
  cmd.AddValue("mlFootprintFloorBps",
               "Min bps for a switch to count as 'active' (footprint penalty)",
               footprintFloorBps);
  cmd.AddValue("mlFootprintFrac",
               "Load-proportional active threshold: totalTxBps * frac",
               footprintFrac);
  cmd.AddValue("mlEnPower", "ENERGY reward sub-weight: power cost", enPower);
  cmd.AddValue("mlEnUtil", "ENERGY reward sub-weight: utilization", enUtil);
  cmd.AddValue("mlEnFootprint", "ENERGY reward sub-weight: footprint",
               enFootprint);
  cmd.AddValue("mlEnReserve", "ENERGY reward sub-weight: reserve-aware",
               enReserve);
  cmd.AddValue("mlEnBalance", "ENERGY reward sub-weight: energy balance",
               enBalance);
  cmd.AddValue("mlEnChurn", "ENERGY reward sub-weight: route churn", enChurn);
  cmd.AddValue("mlSlaPdr",
               "ENERGY QoS hinge: true-PDR floor below which delivery is penalised",
               slaPdr);
  cmd.AddValue("mlSlaDelay",
               "ENERGY QoS hinge: delayQuality floor below which delay is penalised",
               slaDelay);
  cmd.AddValue("mlPdrHingeW", "ENERGY QoS hinge slope for loss", pdrHingeW);
  cmd.AddValue("mlDelayHingeW", "ENERGY QoS hinge slope for delay", delayHingeW);
  cmd.AddValue("mlSleepThreshold",
               "Node-sleep action threshold: node value above this powers a "
               "switch off (routed around, zero idle+forwarding power)",
               sleepThreshold);
  cmd.AddValue("mlExplore",
               "Enable Gaussian action noise", explore);
  cmd.AddValue("mlLearn",
               "Enable gradient updates / train_step", learn);
  cmd.AddValue("mlCheckpointEveryNTicks", "Checkpoint cadence",
               checkpointEveryNTicks);
  cmd.AddValue("mlResume", "Resume from checkpoint", resume);
  cmd.AddValue("mlEndpoint", "ZMQ endpoint (single-controller mode)",
               endpoint);
  cmd.AddValue("mlNoiseSigma",
               "Pin Python agent's exploration noise sigma to this value "
               "(overrides default 0.3 init and checkpoint's saved sigma). "
               "Negative = use the default decay schedule.",
               noiseSigmaInit);
  cmd.AddValue("mlNoiseSigmaMin",
               "Floor on the decaying Gaussian action noise (default 0.10).",
               noiseSigmaMin);
  cmd.AddValue("mlActionVarWeight",
               "Actor-loss weight on the per-link action variance "
               "(higher = stronger anti-collapse pressure).",
               actionVarWeight);
  cmd.AddValue("mlSaturationWeight",
               "Actor-loss weight on squared pre-tanh logits "
               "(higher = harder pull back into tanh's linear region).",
               saturationWeight);
  cmd.AddValue("mlResetActor",
               "One-shot: on next checkpoint resume, fresh-init the actor "
               "(keep critic + replay). Use to recover a collapsed policy.",
               resetActor);
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
  cfg.power_ref_w = powerRefW;
  cfg.footprint_floor_bps = footprintFloorBps;
  cfg.footprint_frac = footprintFrac;
  cfg.en_w_power = enPower;
  cfg.en_w_util = enUtil;
  cfg.en_w_footprint = enFootprint;
  cfg.en_w_reserve = enReserve;
  cfg.en_w_balance = enBalance;
  cfg.en_w_churn = enChurn;
  cfg.sla_pdr = slaPdr;
  cfg.sla_delay = slaDelay;
  cfg.pdr_hinge_w = pdrHingeW;
  cfg.delay_hinge_w = delayHingeW;
  cfg.sleep_threshold = sleepThreshold;
  cfg.explore = explore;
  cfg.learn = learn;
  cfg.checkpoint_every_n_ticks = checkpointEveryNTicks;
  cfg.resume = resume;
  cfg.seed = seedIn;
  cfg.endpoint = endpoint;
  cfg.noise_sigma_init = noiseSigmaInit;
  cfg.noise_sigma_min = noiseSigmaMin;
  cfg.action_var_weight = actionVarWeight;
  cfg.saturation_weight = saturationWeight;
  cfg.reset_actor = resetActor;

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
