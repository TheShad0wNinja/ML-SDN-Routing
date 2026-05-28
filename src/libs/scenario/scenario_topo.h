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
  std::string label;

  // Populated by topology factories that support multi-controller mode.
  // Empty = topology is unsupported in multi-controller mode.
  std::vector<SectionDef> sections;
  std::vector<InterDomainRoute> interDomainRoutes;
};

std::vector<uint32_t> ParseIndexCsv(const std::string& csv);

// Restrict a TopoSpec to a subset of switch indices. Used by the Phase-1
// federated path: each ns-3 process simulates one section as an isolated
// subgraph. Cross-section links are dropped. Indices are renumbered
// contiguously starting at 0. Sections + interDomainRoutes are cleared on
// the returned spec since they only describe the unfiltered topology.
TopoSpec FilterTopoSpecBySection(const TopoSpec& full,
                                 const std::vector<uint32_t>& keptIndices);

}  // namespace ns3

#endif  // SCRATCH_SCENARIO_TOPO_H
