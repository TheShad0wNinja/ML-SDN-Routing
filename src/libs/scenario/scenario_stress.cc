#include "scenario/scenario_stress.h"

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
      m_failureLinks(failureLinks) {}

void StressEvents::Schedule(double measureStart, double window,
                            uint32_t dstHostFlash,
                            uint32_t blackHoleSwitchIdx) {
  auto at = [&](double frac) {
    return Seconds(measureStart + frac * window);
  };

  if (m_failureLinks.size() >= 1) {
    Simulator::Schedule(at(0.10), &LinkController::Degrade, m_failureLinks[0],
                        0.30);
    Simulator::Schedule(at(0.40), &LinkController::BringUp, m_failureLinks[0]);
  }

  // Flash crowd: 4 short bulk flows at 10 Mbps each = 40 Mbps to one host.
  // Pre-install up front (before Run) with future Start/Stop; dynamic install
  // from inside a scheduled callback raced with CSMA TX-queue and SIGSEGV'd
  // around t=30s under --tcp --failures.
  PreInstallFlashCrowd(dstHostFlash, 5.0, 10.0, 4,
                       measureStart + 0.25 * window);

  if (m_failureLinks.size() >= 3) {
    Simulator::Schedule(at(0.55), &LinkController::BringDown,
                        m_failureLinks[1]);
    Simulator::Schedule(at(0.55), &LinkController::BringDown,
                        m_failureLinks[2]);
    Simulator::Schedule(at(0.75), &LinkController::BringUp, m_failureLinks[1]);
    Simulator::Schedule(at(0.75), &LinkController::BringUp, m_failureLinks[2]);
    NS_LOG_INFO("StressEvents: correlated outage links[1,2] scheduled");
  }

  if (blackHoleSwitchIdx < m_switches.GetN()) {
    Simulator::Schedule(at(0.85), &StressEvents::BlackHoleOn, this,
                        blackHoleSwitchIdx);
    Simulator::Schedule(at(0.85) + Seconds(5.0), &StressEvents::BlackHoleOff,
                        this, blackHoleSwitchIdx);
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
// under --tcp --failures).
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
