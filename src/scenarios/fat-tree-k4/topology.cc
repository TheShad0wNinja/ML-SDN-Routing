#include "topology.h"

namespace ns3 {

// Pod layout: 4 cores (idx 0..3), then per pod (2 agg + 2 edge) starting at
// idx 4. Agg-core split: agg[p][0] connects to cores 0,1; agg[p][1] to 2,3.
TopoSpec BuildFatTreeK4Spec(const std::string& backboneQueue) {
  TopoSpec spec;
  spec.label = "fat-tree-k4";
  auto pod = [](uint32_t p, uint32_t slot) { return 4 + p * 4 + slot; };

  // NodeProfile fields: name, tier, cpuCapacity, tcamDelayUs, energyPerByteJ,
  // initialEnergyJ, idlePowerW. idlePowerW is the powered-on idle draw the
  // node-sleep action can eliminate; modest per-tier starting points — tune as
  // needed (a slept switch contributes neither idle nor forwarding power).
  // initialEnergyJ bumped ~1.5× vs. the original (core 1e4→1.5e4, agg 4e3→6e3,
  // edge 2e3→3e3) for headroom: fat-tree's energy story is sleep/consolidation
  // (idlePowerW is the lever), not node death, so DC bursts shouldn't drain a
  // switch to the lifetime hinge prematurely. energyPerByteJ unchanged.
  for (uint32_t c = 0; c < 4; ++c) {
    spec.nodes.push_back(
        {"core" + std::to_string(c), "tier1", "1Gbps", 2, 5e-7, 1.5e4, 2.0});
  }
  for (uint32_t p = 0; p < 4; ++p) {
    spec.nodes.push_back({"agg" + std::to_string(p) + "_0", "tier2", "500Mbps",
                          5, 8e-7, 6e3, 1.0});
    spec.nodes.push_back({"agg" + std::to_string(p) + "_1", "tier2", "500Mbps",
                          5, 8e-7, 6e3, 1.0});
    spec.nodes.push_back(
        {"edge" + std::to_string(p) + "_0", "edge", "100Mbps", 10, 1e-6, 3e3, 0.5});
    spec.nodes.push_back(
        {"edge" + std::to_string(p) + "_1", "edge", "100Mbps", 10, 1e-6, 3e3, 0.5});
  }

  const double dKm = 200.0;
  for (uint32_t p = 0; p < 4; ++p) {
    uint32_t a0 = pod(p, 0), a1 = pod(p, 1);
    uint32_t e0 = pod(p, 2), e1 = pod(p, 3);
    
    // Agg to Core links - fairly high bandwidth
    spec.links.push_back({a0, 0, dKm, 0.0, backboneQueue, false, "10Gbps"});
    spec.links.push_back({a0, 1, dKm, 0.0, backboneQueue, false, "10Gbps"});
    spec.links.push_back({a1, 2, dKm, 0.0, backboneQueue, false, "10Gbps"});
    spec.links.push_back({a1, 3, dKm, 0.0, backboneQueue, false, "10Gbps"});
    
    // Edge to Agg links - medium bandwidth
    spec.links.push_back({a0, e0, dKm, 0.0, backboneQueue, false, "1Gbps"});
    spec.links.push_back({a0, e1, dKm, 0.0, backboneQueue, false, "1Gbps"});
    spec.links.push_back({a1, e0, dKm, 0.0, backboneQueue, false, "1Gbps"});
    spec.links.push_back({a1, e1, dKm, 0.0, backboneQueue, false, "1Gbps"});
  }
  // Agg↔core uplinks are redundant (each agg has two cores), so they are safe
  // crisis material: cutting/flapping one always has a backup. Mark several so
  // the seed-varied suite has links to pick from for outage + mobility flaps.
  spec.links[0].failureTarget = true;
  spec.links[1].failureTarget = true;
  spec.links[8].failureTarget = true;
  spec.links[16].failureTarget = true;
  spec.links[17].failureTarget = true;

  for (uint32_t p = 0; p < 4; ++p) {
    uint32_t e0 = pod(p, 2), e1 = pod(p, 3);
    spec.hostToSwitch.push_back(e0);
    spec.hostToSwitch.push_back(e0);
    spec.hostToSwitch.push_back(e1);
    spec.hostToSwitch.push_back(e1);
  }
  for (uint32_t h = 0; h < 16; ++h) {
    spec.hostNames.push_back("h" + std::to_string(h));
  }
  spec.defaultCentralHost = 0;
  spec.defaultFlashCrowdDst = 0;
  spec.defaultBlackHoleSwitch = 4;  // pod-0 aggregation switch (host-free)
  spec.defaultKillSwitch = 8;       // pod-1 agg (host-free, reroutable via its twin)
  spec.defaultPowerRefW = 10000.0;  // ~30kW full sat; realistic peak ~10kW at 30% util
  return spec;
}

}  // namespace ns3
