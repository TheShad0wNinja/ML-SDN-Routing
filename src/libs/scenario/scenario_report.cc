#include "scenario/scenario_report.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>

#include "ns3/histogram.h"

namespace ns3 {

namespace {

void PrintFlowMonitorSummary(const ScenarioReportInputs& in) {
  in.monitor->CheckForLostPackets();
  auto stats = in.monitor->GetFlowStats();

  uint64_t totalTx = 0, totalRx = 0, totalLost = 0;
  double delaySumS = 0.0, jitterSumS = 0.0;
  uint64_t rxForDelay = 0, rxForJitter = 0;
  for (auto& kv : stats) {
    totalTx += kv.second.txPackets;
    totalRx += kv.second.rxPackets;
    totalLost += kv.second.lostPackets;
    delaySumS += kv.second.delaySum.GetSeconds();
    rxForDelay += kv.second.rxPackets;
    jitterSumS += kv.second.jitterSum.GetSeconds();
    if (kv.second.rxPackets > 1) rxForJitter += kv.second.rxPackets - 1;
  }

  std::cout << "\n=== FlowMonitor Summary (post-warmup window) ===" << std::endl;
  std::cout << "  Flows       : " << stats.size() << std::endl;
  std::cout << "  Tx packets  : " << totalTx << std::endl;
  std::cout << "  Rx packets  : " << totalRx << std::endl;
  std::cout << "  Lost packets: " << totalLost << std::endl;
  if (totalTx > 0) {
    std::cout << "  Delivery    : " << (totalRx * 100.0 / totalTx) << "%"
              << std::endl;
  }
  if (rxForDelay > 0) {
    std::cout << "  Avg delay   : " << (delaySumS * 1000.0 / rxForDelay)
              << " ms" << std::endl;
  }
  std::cout << "  Avg jitter  : "
            << (rxForJitter > 0 ? jitterSumS * 1000.0 / rxForJitter : 0.0)
            << " ms" << std::endl;

  if (in.sections && !in.sections->empty() && in.ctrls->size() > 1) {
    double hopSum = 0.0;
    uint32_t hopCount = 0;
    std::cout << "\n=== Per-Controller Hop Counts ===" << std::endl;
    for (uint32_t s = 0; s < in.ctrls->size(); ++s) {
      double avg = (*in.ctrls)[s]->GetAverageHopCount();
      const std::string& name = s < in.sections->size()
                                    ? (*in.sections)[s].name
                                    : "ctrl" + std::to_string(s);
      std::cout << "  ctrl[" << s << "] (" << name << "): avg_hops=" << avg
                << std::endl;
      if (avg > 0) {
        hopSum += avg;
        ++hopCount;
      }
    }
    std::cout << "  Avg hops    : "
              << (hopCount > 0 ? hopSum / hopCount : 0.0) << std::endl;
  } else {
    std::cout << "  Avg hops    : " << (*in.ctrls)[0]->GetAverageHopCount()
              << std::endl;
  }
}

void PrintPerClass(const ScenarioReportInputs& in) {
  if (!in.portToClass || in.portToClass->empty()) return;

  struct ClassStats {
    uint64_t tx = 0, rx = 0, lost = 0;
    uint64_t rxBytes = 0;
    double delaySumS = 0.0, jitterSumS = 0.0;
    uint64_t rxForDelay = 0, rxForJitter = 0;
    std::vector<uint64_t> delayBins;
    double firstTxS = 1e30, lastRxS = 0.0;
    uint32_t flowCount = 0;
  };

  auto stats = in.monitor->GetFlowStats();
  std::map<std::string, ClassStats> per;
  for (auto& kv : stats) {
    auto t = in.classifier->FindFlow(kv.first);
    auto it = in.portToClass->find(t.destinationPort);
    if (it == in.portToClass->end()) continue;
    ClassStats& cs = per[it->second];
    ++cs.flowCount;
    cs.tx += kv.second.txPackets;
    cs.rx += kv.second.rxPackets;
    cs.lost += kv.second.lostPackets;
    cs.rxBytes += kv.second.rxBytes;
    cs.delaySumS += kv.second.delaySum.GetSeconds();
    cs.rxForDelay += kv.second.rxPackets;
    cs.jitterSumS += kv.second.jitterSum.GetSeconds();
    if (kv.second.rxPackets > 1) cs.rxForJitter += kv.second.rxPackets - 1;
    double firstTx = kv.second.timeFirstTxPacket.GetSeconds();
    double lastRx = kv.second.timeLastRxPacket.GetSeconds();
    if (firstTx < cs.firstTxS) cs.firstTxS = firstTx;
    if (lastRx > cs.lastRxS) cs.lastRxS = lastRx;
    const Histogram& h = kv.second.delayHistogram;
    for (uint32_t b = 0; b < h.GetNBins(); ++b) {
      uint32_t c = h.GetBinCount(b);
      if (c == 0) continue;
      if (cs.delayBins.size() <= b) cs.delayBins.resize(b + 1, 0);
      cs.delayBins[b] += c;
    }
  }

  std::cout << "\n=== Per-Class FlowMonitor ===" << std::endl;
  std::cout << std::left << std::setw(8) << "Class" << std::right
            << std::setw(7) << "Flows" << std::setw(10) << "Tx"
            << std::setw(10) << "Rx" << std::setw(8) << "Loss%"
            << std::setw(11) << "AvgD(ms)" << std::setw(11) << "p99D(ms)"
            << std::setw(12) << "Mbps" << std::endl;
  for (const auto& kv : per) {
    const ClassStats& cs = kv.second;
    double lossPct = cs.tx > 0 ? 100.0 * (double)cs.lost / cs.tx : 0.0;
    double avgDelayMs =
        cs.rxForDelay > 0 ? cs.delaySumS * 1000.0 / cs.rxForDelay : 0.0;
    uint64_t total = 0;
    for (uint64_t c : cs.delayBins) total += c;
    double p99Ms = 0.0;
    if (total > 0) {
      uint64_t target = static_cast<uint64_t>(std::ceil(0.99 * total));
      uint64_t running = 0;
      for (size_t b = 0; b < cs.delayBins.size(); ++b) {
        running += cs.delayBins[b];
        if (running >= target) {
          p99Ms = (b + 1) * 10.0;
          break;
        }
      }
    }
    double durS = std::max(1e-6, cs.lastRxS - cs.firstTxS);
    double goodputMbps = cs.rxBytes * 8.0 / durS / 1.0e6;
    std::cout << std::left << std::setw(8) << kv.first << std::right
              << std::setw(7) << cs.flowCount << std::setw(10) << cs.tx
              << std::setw(10) << cs.rx << std::setw(8) << std::fixed
              << std::setprecision(2) << lossPct << std::setw(11)
              << avgDelayMs << std::setw(11) << p99Ms << std::setw(12)
              << goodputMbps << std::endl;
  }
}

void PrintSwitchEnergy(const ScenarioReportInputs& in) {
  double totalInitialJ = 0.0;
  double totalResidualJ = 0.0;
  uint32_t tracked = 0;
  std::cout << "\n=== Switch Energy (consumed over " << in.simTime
            << "s) ===" << std::endl;
  std::cout << std::left << std::setw(14) << "Switch" << std::right
            << std::setw(14) << "Consumed (J)" << std::setw(14)
            << "Avg Power (W)" << std::endl;

  const uint32_t numSwitches = in.nodes->size();
  const bool isMulti = in.sections && !in.sections->empty()
                        && in.switchToSection.size() == numSwitches
                        && in.ctrls->size() > 1;

  // Lifetime tracking
  double minFrac = 2.0, maxFrac = -1.0;
  uint64_t minDpid = 0, maxDpid = 0;
  double firstDeathTime = -1.0;
  uint64_t firstDeathDpid = 0;

  for (uint32_t i = 0; i < numSwitches; ++i) {
    uint64_t dpid = i + 1;
    int owner = isMulti ? in.switchToSection[i] : 0;
    double init = (*in.ctrls)[owner]->GetSwitchInitialEnergyJ(dpid);
    double resid = (*in.ctrls)[owner]->GetSwitchResidualEnergyJ(dpid);
    if (init < 0 || resid < 0) continue;
    double consumed = init - resid;
    double avgW = (in.simTime > 0) ? consumed / in.simTime : 0.0;
    totalInitialJ += init;
    totalResidualJ += resid;
    ++tracked;
    std::cout << std::left << std::setw(14) << (*in.nodes)[i].name
              << std::right << std::setw(14) << std::fixed
              << std::setprecision(2) << consumed << std::setw(14) << avgW
              << std::endl;

    double frac = (init > 0) ? resid / init : 0.0;
    if (frac < minFrac) { minFrac = frac; minDpid = dpid; }
    if (frac > maxFrac) { maxFrac = frac; maxDpid = dpid; }

    double dt = (*in.ctrls)[owner]->GetDeathTimeS(dpid);
    if (dt >= 0.0 && (firstDeathTime < 0.0 || dt < firstDeathTime)) {
      firstDeathTime = dt;
      firstDeathDpid = dpid;
    }
  }
  if (tracked > 0 && in.simTime > 0) {
    double totalConsumed = totalInitialJ - totalResidualJ;
    double residualFrac = (totalInitialJ > 0)
                              ? (totalResidualJ / totalInitialJ) * 100.0
                              : 0.0;
    std::cout << "  Switches tracked  : " << tracked << std::endl;
    std::cout << "  Total consumed    : " << totalConsumed << " J"
              << std::endl;
    std::cout << "  Total residual    : " << totalResidualJ << " J"
              << std::endl;
    std::cout << "  Total avg power   : " << (totalConsumed / in.simTime)
              << " W" << std::endl;
    std::cout << "  Per-switch avg    : "
              << (totalConsumed / in.simTime / tracked) << " W" << std::endl;
    std::cout << "  Per-switch consumed : " << (totalConsumed / tracked)
              << " J" << std::endl;
    std::cout << "  Per-switch residual : " << (totalResidualJ / tracked)
              << " J" << std::endl;
    std::cout << "  Residual fraction : " << residualFrac << "%" << std::endl;
    // Network lifetime
    double lifetimeS = (firstDeathTime >= 0.0) ? firstDeathTime : in.simTime;
    std::cout << "  Network lifetime  : " << lifetimeS << " s";
    if (firstDeathTime < 0.0) std::cout << " (no deaths)";
    std::cout << std::endl;
    if (firstDeathTime >= 0.0) {
      std::cout << "  First death dpid  : " << firstDeathDpid << std::endl;
    }
    std::cout << "  Min residual pct  : " << (minFrac * 100.0) << " %"
              << std::endl;
    std::cout << "  Min residual dpid : " << minDpid << std::endl;
    std::cout << "  Max residual pct  : " << (maxFrac * 100.0) << " %"
              << std::endl;
    std::cout << "  Max residual dpid : " << maxDpid << std::endl;
  } else {
    std::cout << "  (no energy model configured)" << std::endl;
  }
}

}  // namespace

void PrintScenarioReports(const ScenarioReportInputs& in) {
  PrintFlowMonitorSummary(in);
  PrintPerClass(in);
  PrintSwitchEnergy(in);
}

}  // namespace ns3
