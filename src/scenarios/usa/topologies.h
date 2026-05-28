#ifndef SCRATCH_SCENARIOS_USA_TOPOLOGIES_H
#define SCRATCH_SCENARIOS_USA_TOPOLOGIES_H

#include <string>

#include "scenario/scenario_topo.h"

namespace ns3 {

// 34-node USA backbone with three sections (west/central/east) + inter-domain
// routes baked in (matches the former scenarios/usa/sections.json).
// crippleEnabled=true rewrites the Missoula profile to 1 Mbps / 100 us TCAM.
TopoSpec BuildUsaSpec(const std::string& backboneQueue, bool crippleEnabled);

}  // namespace ns3

#endif  // SCRATCH_SCENARIOS_USA_TOPOLOGIES_H
