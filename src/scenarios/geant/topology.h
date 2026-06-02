#pragma once
#include "scenario/scenario_topo.h"
#include <string>

namespace ns3 {
TopoSpec BuildGeantSpec(const std::string& backboneQueue);
}  // namespace ns3
