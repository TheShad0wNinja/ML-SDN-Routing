#include "topology.h"

#include <string>

namespace ns3 {

// Clustered WSN topology with real inter-host path diversity.
//
//   index 0 .. C-1            cluster-heads CH_0 .. CH_{C-1}  (host-bearing)
//   index C + c*S + j         sensor j of cluster c           (host-free)
//
// Cost recap: base_cost = clamp(delayMs,1,1000) / capacity_gbps, and every
// short link floors to delayMs=1, so cost is driven by *bandwidth*. We exploit
// that to make the sensor fabric the cheap, preferred transport and the
// cluster-head ring an expensive backup:
//   * Sensor links: 1Gbps, short  -> base cost ~1.0
//   * CH express ring: 200Mbps, 700 km long-haul -> base cost ~17.5
// So baseline (Dijkstra) routes inter-cluster traffic *through the sensors*
// (cheap, multi-hop) instead of hopping the near-direct CH ring. The hosts are
// therefore NOT near-directly connected — traffic threads the sensor mesh,
// giving the ML cost-lever many comparable paths to steer and making sensors
// load-bearing (they drain, can die, and sleeping one forces a reroute).
//
// Connectivity guarantees:
//   * The CH express ring always reconnects everything, so no sensor is ever a
//     cut vertex -> all sensors stay sleepable, deaths/cuts stay reroutable.
//   * Each CH attaches to all S of its sensors and there are TWO parallel
//     inter-cluster sensor bridges per adjacent pair, so a slept/dead/cut
//     sensor always has a sibling path before falling back to the ring.
TopoSpec BuildSensorClusterSpec(const std::string& backboneQueue) {
  TopoSpec spec;
  spec.label = "sensor-cluster";

  constexpr uint32_t C = 4;  // cluster-heads
  constexpr uint32_t S = 4;  // sensors per cluster
  auto ch = [](uint32_t c) { return c; };
  auto sn = [&](uint32_t c, uint32_t j) { return C + c * S + j; };

  // NodeProfile: name, nodeType, cpuCapacity, tcamDelayUs, energyPerByteJ,
  // initialEnergyJ, idlePowerW.
  for (uint32_t c = 0; c < C; ++c) {
    // Cluster-heads: host-bearing gateways + inter-cluster transit. Large energy
    // budget, never sleep.
    spec.nodes.push_back(
        {"ch" + std::to_string(c), "cluster-head", "1Gbps", 5, 5e-7, 2e4, 1.5});
  }
  for (uint32_t c = 0; c < C; ++c) {
    for (uint32_t j = 0; j < S; ++j) {
      // Sensor relays: host-free, sleepable. They carry the cheap inter-cluster
      // traffic by default, so a loaded sensor depletes mid-sim (~300 s at a few
      // Mbps); an idle one never dies. energyPerByteJ/initialEnergyJ tuned so
      // death is reachable under load within a 600 s sim but not instant.
      spec.nodes.push_back({"s" + std::to_string(c) + "_" + std::to_string(j),
                            "sensor", "1Gbps", 15, 1.5e-6, 1200.0, 0.1});
    }
  }

  auto addLink = [&](uint32_t a, uint32_t b, const std::string& rate,
                     double distKm, bool fail) {
    spec.links.push_back({a, b, distKm, 0.0, backboneQueue, fail, rate});
  };

  // CH express ring — long-haul, modest bandwidth => HIGH cost backup. Every hop
  // is a failure target (cutting one is reroutable the other way or via sensors).
  for (uint32_t c = 0; c < C; ++c) {
    addLink(ch(c), ch((c + 1) % C), "200Mbps", 700.0, /*fail=*/true);
  }

  // Per cluster: CH attaches to all S sensors (no sensor is a cut vertex), and
  // the sensors form a 4-cycle mesh. All sensor links are short 1Gbps => cost ~1.
  for (uint32_t c = 0; c < C; ++c) {
    for (uint32_t j = 0; j < S; ++j) addLink(ch(c), sn(c, j), "1Gbps", 40.0, false);
    addLink(sn(c, 0), sn(c, 1), "1Gbps", 40.0, false);
    addLink(sn(c, 2), sn(c, 3), "1Gbps", 40.0, false);
    addLink(sn(c, 0), sn(c, 2), "1Gbps", 40.0, false);
    addLink(sn(c, 1), sn(c, 3), "1Gbps", 40.0, false);
  }

  // Two parallel inter-cluster sensor bridges per adjacent pair (east sensors of
  // c -> west sensors of c+1): redundant, load-balanceable cheap paths. Mark a
  // couple as failure material for the seed-varied outage/flap pool.
  for (uint32_t c = 0; c < C; ++c) {
    addLink(sn(c, 2), sn((c + 1) % C, 0), "1Gbps", 60.0, /*fail=*/(c % 2 == 0));
    addLink(sn(c, 3), sn((c + 1) % C, 1), "1Gbps", 60.0, /*fail=*/false);
  }

  // One host per cluster-head (the data sink/source). Sensors stay host-free so
  // the node-sleep mask leaves them sleepable.
  for (uint32_t c = 0; c < C; ++c) {
    spec.hostToSwitch.push_back(ch(c));
    spec.hostNames.push_back("h" + std::to_string(c));
    spec.hostGroups.push_back(c);  // one group per cluster
  }

  spec.defaultCentralHost = 0;            // CH0 host
  spec.defaultFlashCrowdDst = 0;          // CH0 host
  spec.defaultBlackHoleSwitch = sn(0, 2); // a bridge sensor (host-free, reroutable)
  spec.defaultKillSwitch = sn(0, 3);      // a loaded bridge sensor (battery-drain)
  spec.defaultPowerRefW = 5000.0;         // ~16kW full sat; realistic peak ~5kW at 30% util
  return spec;
}

}  // namespace ns3
