#ifndef SCRATCH_SCENARIO_TOPO_H
#define SCRATCH_SCENARIO_TOPO_H

#include <cstdint>
#include <string>
#include <vector>

namespace ns3 {

struct NodeProfile {
  std::string name;
  std::string nodeType;
  std::string cpuCapacity;
  uint32_t tcamDelayUs = 5;
  double energyPerByteJ = 0.0;
  double initialEnergyJ = 0.0;
};

struct LinkSpec {
  uint32_t src = 0;
  uint32_t dst = 0;
  double distanceKm = 0.0;
  double lossRate = 0.0;
  std::string bufferSize;
  bool failureTarget = false;
  std::string dataRate = "1Gbps";
};

struct SectionDef {
  uint32_t id = 0;
  std::string name;
  std::vector<uint32_t> nodes;
  std::vector<uint32_t> borderSwitches;
};

struct InterDomainRoute {
  uint32_t fromSection = 0;
  uint32_t toSection = 0;
  uint32_t viaSwitch = 0;
  uint32_t nextSwitch = 0;
};

struct TopoSpec {
  std::vector<NodeProfile> nodes;
  std::vector<LinkSpec> links;
  std::vector<uint32_t> hostToSwitch;
  std::vector<std::string> hostNames;
  // Optional per-host group id used by trafficMode="grouped" to bias flows
  // across groups (e.g. west/central/east). Parallel to hostToSwitch. Leave
  // empty for topologies without a natural grouping — grouped mode then
  // degrades to random. Survives FilterTopoSpecBySection so federated
  // workers see the correct group of each surviving host.
  std::vector<uint32_t> hostGroups;
  std::string label;

  // Populated by topology factories that natively partition; left empty for
  // flat topologies. NormalizeTopoSpec() backfills a single all-switch
  // section so every TopoSpec has at least one.
  std::vector<SectionDef> sections;
  std::vector<InterDomainRoute> interDomainRoutes;

  // Topology-specific defaults that scenarios used to hard-code in their
  // main.cc. Set by the topology factory; the runner reads them when the
  // corresponding CLI flag wasn't explicitly set.
  uint32_t defaultCentralHost = 0;
  uint32_t defaultFlashCrowdDst = 0;
  uint32_t defaultBlackHoleSwitch = 0;
};

std::vector<uint32_t> ParseIndexCsv(const std::string& csv);

// Ensure topo.sections is non-empty: if a factory didn't define one, add a
// single section covering all switches. Idempotent.
void NormalizeTopoSpec(TopoSpec& topo);

// Restrict a TopoSpec to a subset of switch indices. Used by the Phase-1
// federated path: each ns-3 process simulates one section as an isolated
// subgraph. Cross-section links are dropped. Indices are renumbered
// contiguously starting at 0. Sections + interDomainRoutes are cleared on
// the returned spec since they only describe the unfiltered topology.
TopoSpec FilterTopoSpecBySection(const TopoSpec& full,
                                 const std::vector<uint32_t>& keptIndices);

}  // namespace ns3

#endif  // SCRATCH_SCENARIO_TOPO_H
