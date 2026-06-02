#include "topology.h"

namespace ns3 {

// GEANT-inspired pan-European research backbone — zero-shot generalization test.
//
// 24 nodes: 6 tier1 core (host-free), 10 tier2 regional (host-free),
//           8 edge endpoints (host-bearing).
//
// Routing story: hosts live only at geographic periphery (Dublin, Oslo,
// Helsinki, Lisbon, Athens, Sofia, Bucharest, Tallinn). All inter-host
// traffic must traverse at least 3 transit hops through the tier1/tier2
// mesh, giving the GAT actor many comparable paths to evaluate.
//
// Node index map:
//   0 Amsterdam  1 Frankfurt  2 London      3 Paris    4 Milan    5 Vienna
//   6 Brussels   7 Zurich     8 Prague       9 Warsaw  10 Budapest 11 Stockholm
//  12 Madrid    13 Rome      14 Ljubljana   15 Bratislava
//  16 Dublin    17 Oslo      18 Helsinki    19 Lisbon
//  20 Athens    21 Sofia     22 Bucharest   23 Tallinn
//
// 6 failure targets (all have proven alternate paths):
//   {0,11} Amsterdam-Stockholm  → Stockholm uses Warsaw path
//   {1,8}  Frankfurt-Prague     → Prague uses Vienna-Frankfurt
//   {3,4}  Paris-Milan          → Milan uses Zurich/Vienna-Frankfurt-Paris
//   {8,9}  Prague-Warsaw        → Warsaw uses Budapest-Bratislava-Prague
//   {10,22} Budapest-Bucharest  → Bucharest uses Sofia-Budapest
//   {11,18} Stockholm-Helsinki  → Helsinki uses Tallinn-Stockholm
TopoSpec BuildGeantSpec(const std::string& backboneQueue) {
  TopoSpec spec;
  spec.label = "geant";

  // NodeProfile: {name, nodeType, cpuCapacity, tcamDelayUs, energyPerByteJ, initialEnergyJ}
  // idlePowerW is set via the tier loop below.
  spec.nodes = {
      // --- tier1 core: high-capacity transit hubs, never host-bearing ---
      {"Amsterdam",   "tier1", "1Gbps",    2, 5e-7, 1.5e4},  // 0
      {"Frankfurt",   "tier1", "1Gbps",    2, 5e-7, 1.5e4},  // 1
      {"London",      "tier1", "1Gbps",    2, 5e-7, 1.5e4},  // 2
      {"Paris",       "tier1", "1Gbps",    2, 5e-7, 1.5e4},  // 3
      {"Milan",       "tier1", "1Gbps",    2, 5e-7, 1.5e4},  // 4
      {"Vienna",      "tier1", "1Gbps",    2, 5e-7, 1.5e4},  // 5
      // --- tier2 regional: mid-capacity transit, host-free ---
      {"Brussels",    "tier2", "500Mbps",  5, 8e-7, 6e3},    // 6
      {"Zurich",      "tier2", "500Mbps",  5, 8e-7, 6e3},    // 7
      {"Prague",      "tier2", "500Mbps",  5, 8e-7, 6e3},    // 8
      {"Warsaw",      "tier2", "500Mbps",  5, 8e-7, 6e3},    // 9
      {"Budapest",    "tier2", "500Mbps",  5, 8e-7, 6e3},    // 10
      {"Stockholm",   "tier2", "500Mbps",  5, 8e-7, 6e3},    // 11
      {"Madrid",      "tier2", "500Mbps",  5, 8e-7, 6e3},    // 12
      {"Rome",        "tier2", "500Mbps",  5, 8e-7, 6e3},    // 13
      {"Ljubljana",   "tier2", "500Mbps",  5, 8e-7, 6e3},    // 14
      {"Bratislava",  "tier2", "500Mbps",  5, 8e-7, 6e3},    // 15
      // --- edge endpoints: host-bearing, geographic periphery ---
      {"Dublin",      "edge",  "100Mbps", 10, 1e-6, 3e3},    // 16
      {"Oslo",        "edge",  "100Mbps", 10, 1e-6, 3e3},    // 17
      {"Helsinki",    "edge",  "100Mbps", 10, 1e-6, 3e3},    // 18
      {"Lisbon",      "edge",  "100Mbps", 10, 1e-6, 3e3},    // 19
      {"Athens",      "edge",  "100Mbps", 10, 1e-6, 3e3},    // 20
      {"Sofia",       "edge",  "100Mbps", 10, 1e-6, 3e3},    // 21
      {"Bucharest",   "edge",  "100Mbps", 10, 1e-6, 3e3},    // 22
      {"Tallinn",     "edge",  "100Mbps", 10, 1e-6, 3e3},    // 23
  };

  for (auto& n : spec.nodes) {
    if (n.nodeType == "tier1")      n.idlePowerW = 2.0;
    else if (n.nodeType == "tier2") n.idlePowerW = 1.0;
    else                            n.idlePowerW = 0.5;
  }

  // LinkSpec: {src, dst, distKm, lossRate, bufferSize, failureTarget, dataRate}
  //
  // Cost model: base_cost = clamp(delayMs, 1, 1000) / capacity_gbps
  // With lossRate=0.0 and delays 0-5ms, cost is bandwidth-driven:
  //   10Gbps tier1-tier1: cost 0.1-0.5    (strongly preferred)
  //    5Gbps tier1-tier2: cost 0.2-1.0
  //    2Gbps tier2-tier2: cost 0.5-2.0
  //    1Gbps tier2-edge:  cost 1.0-3.0    (shortest paths use tier1 spine)
  spec.links = {
      // === Tier1-Tier1 backbone ring (10 Gbps) ===
      { 0,  1,  400.0, 0.001, backboneQueue, false, "10Gbps"},  // Amsterdam-Frankfurt
      { 0,  2,  500.0, 0.001, backboneQueue, false, "10Gbps"},  // Amsterdam-London
      { 1,  3,  500.0, 0.001, backboneQueue, false, "10Gbps"},  // Frankfurt-Paris
      { 2,  3,  400.0, 0.001, backboneQueue, false, "10Gbps"},  // London-Paris

      // === Tier1-Tier2 spokes (5 Gbps) ===
      { 0,  6,  200.0, 0.000, backboneQueue, false, "5Gbps"},   // Amsterdam-Brussels
      { 0, 11, 1800.0, 0.005, backboneQueue, true,  "5Gbps"},   // Amsterdam-Stockholm [FAIL]
      { 1,  6,  300.0, 0.000, backboneQueue, false, "5Gbps"},   // Frankfurt-Brussels
      { 1,  7,  400.0, 0.001, backboneQueue, false, "5Gbps"},   // Frankfurt-Zurich
      { 1,  8,  500.0, 0.001, backboneQueue, true,  "5Gbps"},   // Frankfurt-Prague [FAIL]
      { 1,  5,  600.0, 0.001, backboneQueue, false, "5Gbps"},   // Frankfurt-Vienna
      { 2,  6,  300.0, 0.000, backboneQueue, false, "5Gbps"},   // London-Brussels
      { 3,  6,  300.0, 0.000, backboneQueue, false, "5Gbps"},   // Paris-Brussels
      { 3,  4,  700.0, 0.002, backboneQueue, true,  "5Gbps"},   // Paris-Milan [FAIL]
      { 3, 12, 1200.0, 0.003, backboneQueue, false, "5Gbps"},   // Paris-Madrid
      { 4,  7,  300.0, 0.000, backboneQueue, false, "5Gbps"},   // Milan-Zurich
      { 4,  5,  700.0, 0.002, backboneQueue, false, "5Gbps"},   // Milan-Vienna
      { 4, 13,  500.0, 0.001, backboneQueue, false, "5Gbps"},   // Milan-Rome
      { 4, 14,  400.0, 0.001, backboneQueue, false, "5Gbps"},   // Milan-Ljubljana
      { 5,  8,  300.0, 0.000, backboneQueue, false, "5Gbps"},   // Vienna-Prague
      { 5, 10,  200.0, 0.000, backboneQueue, false, "5Gbps"},   // Vienna-Budapest
      { 5, 14,  300.0, 0.000, backboneQueue, false, "5Gbps"},   // Vienna-Ljubljana
      { 5, 15,   60.0, 0.000, backboneQueue, false, "5Gbps"},   // Vienna-Bratislava
      { 7,  5,  700.0, 0.002, backboneQueue, false, "5Gbps"},   // Zurich-Vienna (cross-link)

      // === Tier2-Tier2 regional mesh (2 Gbps) ===
      { 8,  9,  500.0, 0.001, backboneQueue, true,  "2Gbps"},   // Prague-Warsaw [FAIL]
      { 8, 15,  200.0, 0.000, backboneQueue, false, "2Gbps"},   // Prague-Bratislava
      { 9, 10,  700.0, 0.002, backboneQueue, false, "2Gbps"},   // Warsaw-Budapest
      { 9, 11, 1500.0, 0.004, backboneQueue, false, "2Gbps"},   // Warsaw-Stockholm
      { 9, 23,  800.0, 0.002, backboneQueue, false, "2Gbps"},   // Warsaw-Tallinn
      {10, 15,  150.0, 0.000, backboneQueue, false, "2Gbps"},   // Budapest-Bratislava
      {10, 22,  700.0, 0.002, backboneQueue, true,  "2Gbps"},   // Budapest-Bucharest [FAIL]
      {10, 21,  800.0, 0.002, backboneQueue, false, "2Gbps"},   // Budapest-Sofia
      {14, 10,  400.0, 0.001, backboneQueue, false, "2Gbps"},   // Ljubljana-Budapest
      {21, 22,  300.0, 0.000, backboneQueue, false, "2Gbps"},   // Sofia-Bucharest

      // === Tier2/Tier1-to-Edge (1 Gbps) ===
      {11, 17,  500.0, 0.001, backboneQueue, false, "1Gbps"},   // Stockholm-Oslo
      {11, 18,  400.0, 0.001, backboneQueue, true,  "1Gbps"},   // Stockholm-Helsinki [FAIL]
      {11, 23,  300.0, 0.000, backboneQueue, false, "1Gbps"},   // Stockholm-Tallinn
      {12, 19,  600.0, 0.001, backboneQueue, false, "1Gbps"},   // Madrid-Lisbon
      {13, 20, 1000.0, 0.003, backboneQueue, false, "1Gbps"},   // Rome-Athens
      {21, 20,  500.0, 0.001, backboneQueue, false, "1Gbps"},   // Sofia-Athens
      {18, 23,  300.0, 0.000, backboneQueue, false, "1Gbps"},   // Helsinki-Tallinn
      { 2, 16,  500.0, 0.001, backboneQueue, false, "1Gbps"},   // London-Dublin
  };

  // 8 hosts, one per edge node. Transit nodes (0-15) carry no hosts.
  // Host groups for trafficMode=grouped:
  //   group 0 (Western): Dublin, Oslo, Lisbon
  //   group 1 (Nordic):  Helsinki, Tallinn
  //   group 2 (Eastern): Athens, Sofia, Bucharest
  const uint32_t edgeSwitches[] = {16, 17, 18, 19, 20, 21, 22, 23};
  const char*    hostNames[]    = {"Dublin", "Oslo", "Helsinki", "Lisbon",
                                   "Athens", "Sofia", "Bucharest", "Tallinn"};
  const uint32_t hostGroups[]   = {0, 0, 1, 0, 2, 2, 2, 1};
  for (uint32_t i = 0; i < 8; ++i) {
    spec.hostToSwitch.push_back(edgeSwitches[i]);
    spec.hostNames.push_back(hostNames[i]);
    spec.hostGroups.push_back(hostGroups[i]);
  }

  // No spec.sections — NormalizeTopoSpec() backfills a single all-switch section.
  spec.defaultCentralHost     = 4;       // Athens (host 4) — southeastern periphery
  spec.defaultFlashCrowdDst   = 4;       // Athens
  spec.defaultBlackHoleSwitch = 6;       // Brussels — tier2, non-articulation
  spec.defaultKillSwitch      = 14;      // Ljubljana — tier2, non-articulation, reroutable
  spec.defaultPowerRefW       = 8000.0;  // ~8kW peak for 24-node research backbone

  return spec;
}

}  // namespace ns3
