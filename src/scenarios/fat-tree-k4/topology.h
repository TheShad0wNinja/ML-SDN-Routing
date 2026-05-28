#ifndef SCRATCH_SCENARIOS_FATTREE_TOPOLOGY_H
#define SCRATCH_SCENARIOS_FATTREE_TOPOLOGY_H

#include <string>

#include "scenario/scenario_topo.h"

namespace ns3 {

// Standard 3-tier fat-tree k=4: 4 cores, 4 pods of (2 agg + 2 edge), 16 hosts
// (2 per edge switch). Sections left empty — fat-tree does not partition
// into independent administrative domains the way the USA backbone does.
TopoSpec BuildFatTreeK4Spec(const std::string& backboneQueue);

}  // namespace ns3

#endif  // SCRATCH_SCENARIOS_FATTREE_TOPOLOGY_H
