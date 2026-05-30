#ifndef SCRATCH_SCENARIOS_SENSOR_CLUSTER_TOPOLOGY_H
#define SCRATCH_SCENARIOS_SENSOR_CLUSTER_TOPOLOGY_H

#include <string>

#include "scenario/scenario_topo.h"

namespace ns3 {

// Clustered WSN-style topology with genuine inter-host path diversity. C
// cluster-heads carry the hosts (traffic sinks/sources) and are non-sleepable;
// each cluster has S host-free "sensor" relays in a 4-cycle mesh, with two
// parallel sensor bridges to each neighbouring cluster. A long-haul, modest-
// bandwidth CH "express ring" is the high-cost backup, so baseline routing
// prefers the cheap multi-hop sensor fabric — the hosts are NOT near-directly
// connected. The agent learns to load-balance/sleep sensors to conserve
// residual energy while the express ring keeps everything reroutable when a
// sensor sleeps, dies (battery-drain crisis), or a link is cut. See topology.cc
// for the cost reasoning (cost is bandwidth-driven, so sensors=1Gbps≈1.0,
// express=200Mbps long-haul≈17.5).
TopoSpec BuildSensorClusterSpec(const std::string& backboneQueue);

}  // namespace ns3

#endif  // SCRATCH_SCENARIOS_SENSOR_CLUSTER_TOPOLOGY_H
