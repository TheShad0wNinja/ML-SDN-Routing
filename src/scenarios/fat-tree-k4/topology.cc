#include "topology.h"

namespace ns3 {

// Pod layout: 4 cores (idx 0..3), then per pod (2 agg + 2 edge) starting at
// idx 4. Agg-core split: agg[p][0] connects to cores 0,1; agg[p][1] to 2,3.
TopoSpec BuildFatTreeK4Spec(const std::string& backboneQueue) {
  TopoSpec spec;
  spec.label = "fat-tree-k4";
  auto pod = [](uint32_t p, uint32_t slot) { return 4 + p * 4 + slot; };

  for (uint32_t c = 0; c < 4; ++c) {
    spec.nodes.push_back(
        {"core" + std::to_string(c), "tier1", "1Gbps", 2, 0.05, 5e7});
  }
  for (uint32_t p = 0; p < 4; ++p) {
    spec.nodes.push_back({"agg" + std::to_string(p) + "_0", "tier2", "500Mbps",
                          5, 0.08, 2e7});
    spec.nodes.push_back({"agg" + std::to_string(p) + "_1", "tier2", "500Mbps",
                          5, 0.08, 2e7});
    spec.nodes.push_back(
        {"edge" + std::to_string(p) + "_0", "edge", "100Mbps", 10, 0.10, 1e7});
    spec.nodes.push_back(
        {"edge" + std::to_string(p) + "_1", "edge", "100Mbps", 10, 0.10, 1e7});
  }

  const double dKm = 200.0;
  for (uint32_t p = 0; p < 4; ++p) {
    uint32_t a0 = pod(p, 0), a1 = pod(p, 1);
    uint32_t e0 = pod(p, 2), e1 = pod(p, 3);
    spec.links.push_back({a0, 0, dKm, 0.0, backboneQueue, false});
    spec.links.push_back({a0, 1, dKm, 0.0, backboneQueue, false});
    spec.links.push_back({a1, 2, dKm, 0.0, backboneQueue, false});
    spec.links.push_back({a1, 3, dKm, 0.0, backboneQueue, false});
    spec.links.push_back({a0, e0, dKm, 0.0, backboneQueue, false});
    spec.links.push_back({a0, e1, dKm, 0.0, backboneQueue, false});
    spec.links.push_back({a1, e0, dKm, 0.0, backboneQueue, false});
    spec.links.push_back({a1, e1, dKm, 0.0, backboneQueue, false});
  }
  spec.links[0].failureTarget = true;
  spec.links[8].failureTarget = true;
  spec.links[16].failureTarget = true;

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
  return spec;
}

}  // namespace ns3
