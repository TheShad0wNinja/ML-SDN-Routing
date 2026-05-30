#include "scenario/scenario_stress.h"

#include <algorithm>

#include "ns3/core-module.h"
#include "ns3/csma-module.h"
#include "ns3/error-model.h"
#include "ns3/log.h"

namespace ns3 {

NS_LOG_COMPONENT_DEFINE("ScenarioStress");

void StressOptions::Register(CommandLine& cmd) {
  cmd.AddValue("failures", "Enable scheduled link churn", enabled);
  cmd.AddValue("flashCrowdDst",
               "Host index targeted by the flash crowd "
               "(0 = use topology default)",
               flashCrowdDst);
  cmd.AddValue("blackHoleSwitch",
               "Switch index that goes dark at 0.85 W "
               "(0 = use topology default)",
               blackHoleSwitchIdx);
  cmd.AddValue("killSwitch",
               "Switch index drained to death (IoT battery-drain crisis) "
               "(0 = use topology default)",
               killSwitchIdx);
}

void LinkController::BringDown(State* ls) {
  NS_LOG_DEBUG("[TRACE] LinkController::BringDown ENTER t="
               << Simulator::Now().GetSeconds() << "s ls=" << ls);
  NS_LOG_INFO("Link DOWN at t=" << Simulator::Now().GetSeconds() << "s");
  SetErrorRate(ls->devA, 1.0);
  SetErrorRate(ls->devB, 1.0);
}

void LinkController::BringUp(State* ls) {
  NS_LOG_DEBUG("[TRACE] LinkController::BringUp ENTER t="
               << Simulator::Now().GetSeconds() << "s ls=" << ls);
  NS_LOG_INFO("Link UP at t=" << Simulator::Now().GetSeconds() << "s");
  SetErrorRate(ls->devA, ls->normalRate);
  SetErrorRate(ls->devB, ls->normalRate);
}

void LinkController::Degrade(State* ls, double rate) {
  NS_LOG_DEBUG("[TRACE] LinkController::Degrade ENTER t="
               << Simulator::Now().GetSeconds() << "s rate=" << rate
               << " ls=" << ls);
  NS_LOG_INFO("Link DEGRADED loss=" << rate << " at t="
                                    << Simulator::Now().GetSeconds() << "s");
  SetErrorRate(ls->devA, rate);
  SetErrorRate(ls->devB, rate);
}

void LinkController::SetErrorRate(Ptr<NetDevice> nd, double rate) {
  Ptr<CsmaNetDevice> csma = DynamicCast<CsmaNetDevice>(nd);
  if (!csma) return;
  Ptr<RateErrorModel> em = CreateObject<RateErrorModel>();
  em->SetAttribute("ErrorRate", DoubleValue(rate));
  em->SetAttribute("ErrorUnit", EnumValue(RateErrorModel::ERROR_UNIT_PACKET));
  csma->SetAttribute("ReceiveErrorModel", PointerValue(em));
}

StressEvents::StressEvents(NodeContainer& hosts, NodeContainer& switches,
                           Ipv4InterfaceContainer& ifaces,
                           const std::vector<LinkController::State*>& failureLinks)
    : m_hosts(hosts),
      m_switches(switches),
      m_ifaces(ifaces),
      m_failureLinks(failureLinks) {
  // Inherits the stream set by RngSeedManager::SetSeed(opts.seed), so crisis
  // timings/targets vary per seed but stay reproducible for a given seed.
  m_rng = CreateObject<UniformRandomVariable>();
}

double StressEvents::Frac(double base, double jitter, double lo, double hi) {
  double f = base + m_rng->GetValue(-jitter, jitter);
  return std::clamp(f, lo, hi);
}

void StressEvents::Schedule(double measureStart, double window,
                            uint32_t dstHostFlash,
                            uint32_t blackHoleSwitchIdx,
                            uint32_t killSwitchIdx,
                            std::function<void(uint32_t)> killFn) {
  auto at = [&](double frac) {
    return Seconds(measureStart + frac * window);
  };
  const size_t nLinks = m_failureLinks.size();

  // --- Gray-fail (excavator nick): one random failure link at 30% loss. ------
  if (nLinks >= 1) {
    size_t gi = m_rng->GetInteger(0, nLinks - 1);
    double down = Frac(0.10, 0.05, 0.03, 0.30);
    double up = std::min(down + 0.25, 0.45);
    Simulator::Schedule(at(down), &LinkController::Degrade, m_failureLinks[gi],
                        0.30);
    Simulator::Schedule(at(up), &LinkController::BringUp, m_failureLinks[gi]);
    NS_LOG_INFO("StressEvents: gray-fail link[" << gi << "] @frac " << down);
  }

  // --- Flash crowd: jittered magnitude/duration/timing to one host. ----------
  {
    double rateMbps = m_rng->GetValue(8.0, 14.0);
    double dur = m_rng->GetValue(4.0, 8.0);
    uint32_t flows = static_cast<uint32_t>(m_rng->GetInteger(3, 6));
    double startFrac = Frac(0.25, 0.06, 0.10, 0.55);
    PreInstallFlashCrowd(dstHostFlash, dur, rateMbps, flows,
                         measureStart + startFrac * window);
  }

  // --- Correlated outage: two distinct random failure links go fully down. ---
  if (nLinks >= 3) {
    size_t a = m_rng->GetInteger(0, nLinks - 1);
    size_t b = m_rng->GetInteger(0, nLinks - 1);
    while (b == a) b = m_rng->GetInteger(0, nLinks - 1);
    double down = Frac(0.55, 0.08, 0.45, 0.70);
    double up = std::min(down + 0.20, 0.85);
    Simulator::Schedule(at(down), &LinkController::BringDown, m_failureLinks[a]);
    Simulator::Schedule(at(down), &LinkController::BringDown, m_failureLinks[b]);
    Simulator::Schedule(at(up), &LinkController::BringUp, m_failureLinks[a]);
    Simulator::Schedule(at(up), &LinkController::BringUp, m_failureLinks[b]);
    NS_LOG_INFO("StressEvents: correlated outage links[" << a << "," << b
                                                         << "] @frac " << down);
  }

  // --- IoT battery-drain death: drain the kill target to zero. ---------------
  if (killFn && killSwitchIdx < m_switches.GetN()) {
    double when = Frac(0.65, 0.07, 0.50, 0.80);
    Simulator::Schedule(at(when), [killFn, killSwitchIdx]() {
      killFn(killSwitchIdx);
    });
    NS_LOG_INFO("StressEvents: battery-drain kill switch=" << killSwitchIdx
                                                           << " @frac " << when);
  }

  // --- Black-hole: chosen switch goes dark for ~5 s. -------------------------
  if (blackHoleSwitchIdx < m_switches.GetN()) {
    double when = Frac(0.85, 0.05, 0.75, 0.92);
    double dur = m_rng->GetValue(4.0, 7.0);
    Simulator::Schedule(at(when), &StressEvents::BlackHoleOn, this,
                        blackHoleSwitchIdx);
    Simulator::Schedule(at(when) + Seconds(dur), &StressEvents::BlackHoleOff,
                        this, blackHoleSwitchIdx);
  }

  // --- Mobility emulation: flap one random failure link in/out of range. -----
  if (nLinks >= 2) {
    size_t fi = m_rng->GetInteger(0, nLinks - 1);
    double downSecs = m_rng->GetValue(2.0, 4.0);
    double periodSecs = m_rng->GetValue(8.0, 14.0);
    ScheduleFlap(m_failureLinks[fi], measureStart, window,
                 Frac(0.30, 0.05, 0.20, 0.40), 0.70, downSecs, periodSecs);
    NS_LOG_INFO("StressEvents: mobility link-flap link[" << fi << "]");
  }
}

void StressEvents::ScheduleFlap(LinkController::State* link, double measureStart,
                                double window, double startFrac, double endFrac,
                                double downSecs, double periodSecs) {
  double t = measureStart + startFrac * window;
  double end = measureStart + endFrac * window;
  for (; t + downSecs < end; t += periodSecs) {
    Simulator::Schedule(Seconds(t), &LinkController::BringDown, link);
    Simulator::Schedule(Seconds(t + downSecs), &LinkController::BringUp, link);
  }
}

void StressEvents::PreInstallFlashCrowd(uint32_t dstHost, double dur,
                                        double rateMbps, uint32_t numFlows,
                                        double startT) {
  uint32_t n = m_hosts.GetN();
  if (dstHost >= n) return;
  NS_LOG_INFO("Flash crowd pre-installed: " << numFlows << " flows @ "
                                            << rateMbps << " Mbps to host "
                                            << dstHost << " firing at t="
                                            << startT << "s");
  Ptr<UniformRandomVariable> uv = CreateObject<UniformRandomVariable>();
  uint64_t bps = static_cast<uint64_t>(rateMbps * 1.0e6);
  for (uint32_t k = 0; k < numFlows; ++k) {
    uint32_t src;
    do {
      src = uv->GetInteger(0, n - 1);
    } while (src == dstHost);
    OnOffHelper onoff("ns3::TcpSocketFactory",
                      InetSocketAddress(m_ifaces.GetAddress(dstHost), 21));
    onoff.SetConstantRate(DataRate(bps), 1448);
    ApplicationContainer app = onoff.Install(m_hosts.Get(src));
    app.Start(Seconds(startT));
    app.Stop(Seconds(startT + dur));
  }
}

// Approximate a node black hole by setting receive error = 1.0 on every CSMA
// device of the chosen switch. Restore by installing a zero-rate model rather
// than re-applying a saved pointer (the saved pointer was the SIGSEGV path
// under --mixedLoad --failures).
void StressEvents::BlackHoleOn(uint32_t switchIdx) {
  NS_LOG_INFO("Black hole ON switch=" << switchIdx << " at t="
                                      << Simulator::Now().GetSeconds() << "s");
  Ptr<Node> sw = m_switches.Get(switchIdx);
  for (uint32_t i = 0; i < sw->GetNDevices(); ++i) {
    Ptr<NetDevice> nd = sw->GetDevice(i);
    if (!DynamicCast<CsmaNetDevice>(nd)) continue;
    LinkController::SetErrorRate(nd, 1.0);
  }
}

void StressEvents::BlackHoleOff(uint32_t switchIdx) {
  NS_LOG_INFO("Black hole OFF switch=" << switchIdx << " at t="
                                       << Simulator::Now().GetSeconds() << "s");
  Ptr<Node> sw = m_switches.Get(switchIdx);
  for (uint32_t i = 0; i < sw->GetNDevices(); ++i) {
    Ptr<NetDevice> nd = sw->GetDevice(i);
    if (!DynamicCast<CsmaNetDevice>(nd)) continue;
    LinkController::SetErrorRate(nd, 0.0);
  }
}

}  // namespace ns3
