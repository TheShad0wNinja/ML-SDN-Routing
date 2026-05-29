#include "topologies.h"

namespace ns3 {

TopoSpec BuildUsaSpec(const std::string& backboneQueue, bool crippleEnabled) {
  TopoSpec spec;
  spec.label = "usa";
  spec.nodes = {
      {"Vancouver",    "edge",     "100Mbps", 10, 1e-6, 2e3},
      {"Seattle",      "tier1",    "1Gbps",    2, 5e-7, 1e4},
      {"Portland",     "edge",     "100Mbps", 10, 1e-6, 2e3},
      {"Sunnyvale",    "tier2",    "500Mbps",  5, 8e-7, 4e3},
      {"LosAngeles",   "tier2",    "500Mbps",  5, 8e-7, 4e3},
      {"Missoula",     "crippled", "1Mbps",  100, 1.5e-6, 1e3},
      {"SaltLakeCity", "tier1",    "1Gbps",    2, 5e-7, 1e4},
      {"Phoenix",      "edge",     "100Mbps", 10, 1e-6, 2e3},
      {"Denver",       "tier2",    "500Mbps",  5, 8e-7, 4e3},
      {"Albuqerque",   "edge",     "100Mbps", 10, 1e-6, 2e3},
      {"ElPaso",       "tier2",    "500Mbps",  5, 8e-7, 4e3},
      {"Minneapolis",  "edge",     "100Mbps", 10, 1e-6, 2e3},
      {"KansasCity",   "tier2",    "500Mbps",  5, 8e-7, 4e3},
      {"Dallas",       "edge",     "100Mbps", 10, 1e-6, 2e3},
      {"Houston",      "tier1",    "1Gbps",    2, 5e-7, 1e4},
      {"Chicago",      "tier1",    "1Gbps",    2, 5e-7, 1e4},
      {"Indianapolis", "edge",     "100Mbps", 10, 1e-6, 2e3},
      {"Louisville",   "edge",     "100Mbps", 10, 1e-6, 2e3},
      {"Nashville",    "tier2",    "500Mbps",  5, 8e-7, 4e3},
      {"Memphis",      "edge",     "100Mbps", 10, 1e-6, 2e3},
      {"Jackson",      "edge",     "100Mbps", 10, 1e-6, 2e3},
      {"BatonRouge",   "edge",     "100Mbps", 10, 1e-6, 2e3},
      {"Cleveland",    "tier2",    "500Mbps",  5, 8e-7, 4e3},
      {"Pittsburgh",   "edge",     "100Mbps", 10, 1e-6, 2e3},
      {"Atlanta",      "tier2",    "500Mbps",  5, 8e-7, 4e3},
      {"Jacksonville", "tier2",    "500Mbps",  5, 8e-7, 4e3},
      {"Buffalo",      "edge",     "100Mbps", 10, 1e-6, 2e3},
      {"Ashburn",      "edge",     "100Mbps", 10, 1e-6, 2e3},
      {"Raleigh",      "edge",     "100Mbps", 10, 1e-6, 2e3},
      {"WashingtonDC", "tier2",    "500Mbps",  5, 8e-7, 4e3},
      {"Miami",        "edge",     "100Mbps", 10, 1e-6, 2e3},
      {"Philadelphia", "edge",     "100Mbps", 10, 1e-6, 2e3},
      {"NewYork",      "edge",     "100Mbps", 10, 1e-6, 2e3},
      {"Boston",       "edge",     "100Mbps", 10, 1e-6, 2e3},
  };
  if (!crippleEnabled) {
    spec.nodes[5] = {"Missoula", "tier2", "500Mbps", 5, 8e-7, 4e3};
  }
  // Powered-on idle draw the node-sleep action can eliminate, by tier. Modest
  // starting points — tune as needed. A slept switch contributes neither idle
  // nor forwarding power and is routed around.
  for (auto& n : spec.nodes) {
    if (n.nodeType == "tier1")
      n.idlePowerW = 2.0;
    else if (n.nodeType == "tier2")
      n.idlePowerW = 1.0;
    else
      n.idlePowerW = 0.5;  // edge
  }
  spec.links = {
      { 4,  7,  575.0, 0.000, backboneQueue, false, "2Gbps"}, // tier2-edge
      { 7, 10,  557.0, 0.000, backboneQueue, false, "500Mbps"}, // edge-tier2
      {10,  9,  369.0, 0.000, backboneQueue, false, "1Gbps"}, // tier2-edge
      {10, 14, 1087.0, 0.004, backboneQueue, false, "2Gbps"}, // tier2-tier1
      { 9,  8,  537.0, 0.000, backboneQueue, false, "1Gbps"}, // edge-tier2
      { 8, 12,  898.0, 0.002, backboneQueue, true,  "2Gbps" }, // tier2-tier2
      { 5, 11, 1617.0, 0.003, backboneQueue, false, "500Mbps"}, // tier2-edge
      {11, 15,  572.0, 0.000, backboneQueue, false, "2Gbps"}, // edge-tier1
      {14, 13,  362.0, 0.000, backboneQueue, false, "2Gbps"}, // tier1-edge
      {13, 12,  729.0, 0.000, backboneQueue, false, "1Gbps"}, // edge-tier2
      {12, 15,  662.0, 0.000, backboneQueue, false, "10Gbps"}, // tier2-tier1
      {15, 16,  263.0, 0.000, backboneQueue, true,  "2Gbps" }, // tier1-edge
      {14, 21,  413.0, 0.000, backboneQueue, false, "1Gbps"}, // tier1-edge
      {21, 25,  913.0, 0.002, backboneQueue, false, "1Gbps"}, // edge-tier2
      {25, 30,  525.0, 0.000, backboneQueue, false, "1Gbps"}, // tier2-edge
      {25, 24,  458.0, 0.000, backboneQueue, false, "2Gbps"}, // tier2-tier2
      {24, 18,  346.0, 0.000, backboneQueue, false, "2Gbps"}, // tier2-tier2
      {24, 28,  572.0, 0.000, backboneQueue, false, "1Gbps"}, // tier2-edge
      {14, 20,  569.0, 0.000, backboneQueue, false, "1Gbps"}, // tier1-edge
      {20, 19,  316.0, 0.000, backboneQueue, false, "500Mbps"}, // edge-edge
      {19, 18,  306.0, 0.000, backboneQueue, false, "1Gbps"}, // edge-tier2
      {18, 17,  249.0, 0.000, backboneQueue, false, "1Gbps"}, // tier2-edge
      {17, 16,  172.0, 0.000, backboneQueue, false, "500Mbps"}, // edge-edge
      {28, 29,  375.0, 0.000, backboneQueue, false, "2Gbps"}, // edge-tier2
      {29, 27,   55.0, 0.000, backboneQueue, false, "1Gbps"}, // tier2-edge
      {29, 31,  199.0, 0.000, backboneQueue, false, "1Gbps"}, // tier2-edge
      {31, 32,  130.0, 0.000, backboneQueue, false, "500Mbps"}, // edge-edge
      {15, 22,  497.0, 0.000, backboneQueue, true,  "10Gbps" }, // tier1-tier2
      {22, 26,  279.0, 0.000, backboneQueue, false, "1Gbps"}, // tier2-edge
      {26, 33,  644.0, 0.000, backboneQueue, false, "500Mbps"}, // edge-edge
      {22, 23,  185.0, 0.000, backboneQueue, false, "1Gbps"}, // tier2-edge
      {23, 27,  360.0, 0.000, backboneQueue, false, "500Mbps"}, // edge-edge
      {32, 33,  306.0, 0.000, backboneQueue, false, "500Mbps"}, // edge-edge
      { 2,  3,  907.0, 0.002, backboneQueue, false, "1Gbps"}, // edge-tier2
      { 3,  4,  503.0, 0.000, backboneQueue, false, "2Gbps"}, // tier2-tier2
      { 3,  6,  955.0, 0.003, backboneQueue, false, "10Gbps"}, // tier2-tier1
      { 8,  6,  598.0, 0.000, backboneQueue, false, "10Gbps"}, // tier2-tier1
      { 4,  6,  934.0, 0.003, backboneQueue, false, "5Gbps"}, // tier2-tier1
      { 0,  1,  194.0, 0.000, backboneQueue, false, "1Gbps"}, // edge-tier1
      { 1,  5,  635.0, 0.000, backboneQueue, false, "5Gbps"}, // tier1-tier2
      { 1,  2,  233.0, 0.000, backboneQueue, false, "1Gbps"}, // tier1-edge
      { 1,  6, 1128.0, 0.003, backboneQueue, false, "10Gbps"}, // tier1-tier1
  };
  // 1:1 host:switch — every switch gets its own host. Group ids match the
  // section partition below (west=0, central=1, east=2) so trafficMode=grouped
  // biases ~80% of flows across groups, and the bias survives section
  // filtering on federated workers.
  spec.hostToSwitch.resize(spec.nodes.size());
  spec.hostNames.resize(spec.nodes.size());
  spec.hostGroups.resize(spec.nodes.size());
  for (uint32_t i = 0; i < spec.nodes.size(); ++i) {
    spec.hostToSwitch[i] = i;
    spec.hostNames[i] = spec.nodes[i].name;
    spec.hostGroups[i] = (i <= 10) ? 0u : (i <= 21 ? 1u : 2u);
  }
  // Sections + inter-domain routes — formerly scenarios/usa/sections.json.
  spec.sections = {
      {0, "west",    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10},        {5, 8, 10}},
      {1, "central", {11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21},
                                                                {11, 12, 14, 15, 18, 21}},
      {2, "east",    {22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33},
                                                                {22, 24, 25}},
  };
  spec.interDomainRoutes = {
      {0, 1,  8, 12}, {0, 2,  8, 12},
      {1, 0, 12,  8}, {1, 2, 15, 22},
      {2, 0, 22, 15}, {2, 1, 22, 15},
  };
  spec.defaultCentralHost = 15;       // Chicago
  spec.defaultFlashCrowdDst = 15;     // Chicago
  spec.defaultBlackHoleSwitch = 22;   // Cleveland
  return spec;
}

}  // namespace ns3
