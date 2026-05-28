#include "scenario/scenario_traffic.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <set>

#include "ns3/core-module.h"
#include "ns3/internet-apps-module.h"

namespace ns3 {

uint64_t StatsCollector::g_pingTx = 0;
uint64_t StatsCollector::g_pingRx = 0;
double StatsCollector::g_rttSumMs = 0.0;

void StatsCollector::PingTxCallback(uint16_t /*seq*/, Ptr<Packet> /*p*/) {
  ++g_pingTx;
}

void StatsCollector::PingRttCallback(uint16_t /*seq*/, Time rtt) {
  ++g_pingRx;
  g_rttSumMs += rtt.GetMilliSeconds();
}

void StatsCollector::PrintPingReport() {
  std::cout << "\n=== Liveness Probe (ping) ===" << std::endl;
  std::cout << "  Sent        : " << g_pingTx << std::endl;
  std::cout << "  Received    : " << g_pingRx << std::endl;
  if (g_pingTx > 0) {
    std::cout << "  Success     : " << (g_pingRx * 100.0 / g_pingTx) << "%"
              << std::endl;
    std::cout << "  Loss        : " << ((g_pingTx - g_pingRx) * 100.0 / g_pingTx)
              << "%" << std::endl;
  }
  if (g_pingRx > 0) {
    std::cout << "  Avg RTT     : " << (g_rttSumMs / g_pingRx) << " ms"
              << std::endl;
  }
}

TrafficManager::TrafficManager(NodeContainer& hosts,
                               Ipv4InterfaceContainer& ifaces,
                               uint32_t centralHostIdx)
    : m_hosts(hosts), m_ifaces(ifaces), m_centralHostIdx(centralHostIdx) {
  m_uv = CreateObject<UniformRandomVariable>();
  m_durRv = CreateObject<LogNormalRandomVariable>();
  m_durRv->SetAttribute("Sigma", DoubleValue(0.6));
  m_rateRv = CreateObject<ParetoRandomVariable>();
  m_rateRv->SetAttribute("Scale", DoubleValue(1.0));
  m_rateRv->SetAttribute("Shape", DoubleValue(1.5));
}

void TrafficManager::WarmupFlows(double startTime, double durationS) {
  uint32_t n = m_hosts.GetN();
  uint32_t totalPairs = n * (n - 1);
  if (totalPairs == 0 || durationS <= 0.0) return;
  double slotS = durationS / static_cast<double>(totalPairs);
  uint32_t idx = 0;
  for (uint32_t src = 0; src < n; ++src) {
    for (uint32_t dst = 0; dst < n; ++dst) {
      if (dst == src) continue;
      PingHelper ping(Ipv4Address(m_ifaces.GetAddress(dst)));
      ping.SetAttribute("VerboseMode", EnumValue(Ping::SILENT));
      ping.SetAttribute("Count", UintegerValue(1));
      ApplicationContainer app = ping.Install(m_hosts.Get(src));
      double t = startTime + idx * slotS;
      app.Start(Seconds(t));
      app.Stop(Seconds(t + 1.0));
      m_warmupApps.Add(app);
      ++idx;
    }
  }
}

void TrafficManager::InstallPings(double startTime, double simTime) {
  uint32_t n = m_hosts.GetN();
  for (uint32_t src = 0; src < n; ++src) {
    uint32_t dst = (src + n / 2) % n;
    PingHelper ping(Ipv4Address(m_ifaces.GetAddress(dst)));
    ping.SetAttribute("VerboseMode", EnumValue(Ping::SILENT));
    ping.SetAttribute("Count", UintegerValue(0));
    ping.SetAttribute("Interval", TimeValue(Seconds(1.0)));
    m_pingApps.Add(ping.Install(m_hosts.Get(src)));
  }
  m_pingApps.Start(Seconds(startTime));
  m_pingApps.Stop(Seconds(simTime - 1.0));
  for (uint32_t i = 0; i < m_pingApps.GetN(); ++i) {
    m_pingApps.Get(i)->TraceConnectWithoutContext(
        "Tx", MakeCallback(&StatsCollector::PingTxCallback));
    m_pingApps.Get(i)->TraceConnectWithoutContext(
        "Rtt", MakeCallback(&StatsCollector::PingRttCallback));
  }
}

void TrafficManager::InstallMixedLoad(double startTime, double simTime,
                                      const std::vector<TrafficClass>& classes,
                                      const std::string& trafficMode,
                                      uint32_t maxConcurrent,
                                      double arrivalRateHz) {
  if (classes.empty() || arrivalRateHz <= 0.0) return;
  m_classes = classes;
  m_portToClass.clear();
  m_cumWeights.clear();
  double cum = 0.0;
  for (const auto& c : classes) {
    m_portToClass[c.port] = c.name;
    cum += c.weight;
    m_cumWeights.push_back(cum);
  }
  m_weightTotal = cum;

  uint32_t n = m_hosts.GetN();
  for (uint32_t i = 0; i < n; ++i) {
    for (const auto& c : classes) {
      std::string fac =
          c.isTcp ? "ns3::TcpSocketFactory" : "ns3::UdpSocketFactory";
      PacketSinkHelper sink(fac,
                            InetSocketAddress(Ipv4Address::GetAny(), c.port));
      ApplicationContainer app = sink.Install(m_hosts.Get(i));
      app.Start(Seconds(startTime - 0.5));
      app.Stop(Seconds(simTime));
      m_sinkApps.Add(app);
    }
  }

  Ptr<ExponentialRandomVariable> interArr =
      CreateObject<ExponentialRandomVariable>();
  interArr->SetAttribute("Mean", DoubleValue(1.0 / arrivalRateHz));

  std::multiset<double> endTimes;
  double t = startTime;
  while (t < simTime - 2.0) {
    t += interArr->GetValue();
    if (t >= simTime - 2.0) break;

    while (!endTimes.empty() && *endTimes.begin() <= t) {
      endTimes.erase(endTimes.begin());
    }
    if (endTimes.size() >= maxConcurrent) continue;

    uint32_t src = m_uv->GetInteger(0, n - 1);
    uint32_t dst = PickDestination(src, n, trafficMode);
    if (dst == src) continue;

    double roll = m_uv->GetValue(0.0, m_weightTotal);
    size_t idx = 0;
    while (idx + 1 < m_cumWeights.size() && roll > m_cumWeights[idx]) ++idx;
    const TrafficClass& cls = m_classes[idx];

    m_durRv->SetAttribute(
        "Mu", DoubleValue(std::log(std::max(1e-6, cls.meanDurS))));
    double dur = std::clamp(m_durRv->GetValue(), 1.0, 3.0 * cls.meanDurS);
    double endT = std::min(t + dur, simTime - 0.5);
    if (endT <= t + 0.1) continue;
    dur = endT - t;

    double rateMbps;
    if (cls.cbr) {
      rateMbps = cls.meanRateMbps;
    } else {
      double r = m_rateRv->GetValue() * cls.meanRateMbps;
      rateMbps = std::clamp(r, 0.5 * cls.meanRateMbps, 4.0 * cls.meanRateMbps);
    }
    uint64_t bps = static_cast<uint64_t>(rateMbps * 1.0e6);
    if (bps == 0) bps = 1000;

    std::string fac =
        cls.isTcp ? "ns3::TcpSocketFactory" : "ns3::UdpSocketFactory";
    OnOffHelper onoff(fac,
                      InetSocketAddress(m_ifaces.GetAddress(dst), cls.port));
    onoff.SetConstantRate(DataRate(bps), cls.pktSize);

    ApplicationContainer app = onoff.Install(m_hosts.Get(src));
    app.Start(Seconds(t));
    app.Stop(Seconds(endT));
    m_srcApps.Add(app);
    endTimes.insert(endT);
  }
}

uint32_t TrafficManager::PickDestination(uint32_t src, uint32_t n,
                                         const std::string& mode) {
  uint32_t dst = src;
  if (mode == "central") {
    // Clamp central host to last index if topology has fewer nodes, then pick
    // a deterministic non-self alternative if the clamp collides with src.
    dst = (m_centralHostIdx < n) ? m_centralHostIdx : (n - 1);
    if (dst == src && n > 1) dst = (src == 0) ? 1 : 0;
  } else if (mode == "random") {
    do {
      dst = m_uv->GetInteger(0, n - 1);
    } while (dst == src);
  } else if (mode == "grouped") {
    bool isWest = (src <= 10);
    bool isEast = (src >= 22);
    double roll = m_uv->GetValue();
    if (roll < 0.8) {
      if (isWest)
        dst = m_uv->GetInteger(22, n - 1);
      else if (isEast)
        dst = m_uv->GetInteger(0, 10);
      else
        do {
          dst = m_uv->GetInteger(0, n - 1);
        } while (dst == src);
    } else {
      do {
        dst = m_uv->GetInteger(0, n - 1);
      } while (dst == src);
    }
  }
  return dst;
}

}  // namespace ns3
