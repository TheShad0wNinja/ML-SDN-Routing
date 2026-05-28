#include "topologies.h"

namespace ns3 {

TopoSpec BuildUsaSpec(const std::string& backboneQueue, bool crippleEnabled) {
  TopoSpec spec;
  spec.label = "usa";
  spec.nodes = {
      {"Vancouver",    "edge",     "100Mbps", 10, 0.10, 1e7},
      {"Seattle",      "tier1",    "1Gbps",    2, 0.05, 5e7},
      {"Portland",     "edge",     "100Mbps", 10, 0.10, 1e7},
      {"Sunnyvale",    "tier2",    "500Mbps",  5, 0.08, 2e7},
      {"LosAngeles",   "tier2",    "500Mbps",  5, 0.08, 2e7},
      {"Missoula",     "crippled", "1Mbps",  100, 0.15, 5e6},
      {"SaltLakeCity", "tier1",    "1Gbps",    2, 0.05, 5e7},
      {"Phoenix",      "edge",     "100Mbps", 10, 0.10, 1e7},
      {"Denver",       "tier2",    "500Mbps",  5, 0.08, 2e7},
      {"Albuqerque",   "edge",     "100Mbps", 10, 0.10, 1e7},
      {"ElPaso",       "tier2",    "500Mbps",  5, 0.08, 2e7},
      {"Minneapolis",  "edge",     "100Mbps", 10, 0.10, 1e7},
      {"KansasCity",   "tier2",    "500Mbps",  5, 0.08, 2e7},
      {"Dallas",       "edge",     "100Mbps", 10, 0.10, 1e7},
      {"Houston",      "tier1",    "1Gbps",    2, 0.05, 5e7},
      {"Chicago",      "tier1",    "1Gbps",    2, 0.05, 5e7},
      {"Indianapolis", "edge",     "100Mbps", 10, 0.10, 1e7},
      {"Louisville",   "edge",     "100Mbps", 10, 0.10, 1e7},
      {"Nashville",    "tier2",    "500Mbps",  5, 0.08, 2e7},
      {"Memphis",      "edge",     "100Mbps", 10, 0.10, 1e7},
      {"Jackson",      "edge",     "100Mbps", 10, 0.10, 1e7},
      {"BatonRouge",   "edge",     "100Mbps", 10, 0.10, 1e7},
      {"Cleveland",    "tier2",    "500Mbps",  5, 0.08, 2e7},
      {"Pittsburgh",   "edge",     "100Mbps", 10, 0.10, 1e7},
      {"Atlanta",      "tier2",    "500Mbps",  5, 0.08, 2e7},
      {"Jacksonville", "tier2",    "500Mbps",  5, 0.08, 2e7},
      {"Buffalo",      "edge",     "100Mbps", 10, 0.10, 1e7},
      {"Ashburn",      "edge",     "100Mbps", 10, 0.10, 1e7},
      {"Raleigh",      "edge",     "100Mbps", 10, 0.10, 1e7},
      {"WashingtonDC", "tier2",    "500Mbps",  5, 0.08, 2e7},
      {"Miami",        "edge",     "100Mbps", 10, 0.10, 1e7},
      {"Philadelphia", "edge",     "100Mbps", 10, 0.10, 1e7},
      {"NewYork",      "edge",     "100Mbps", 10, 0.10, 1e7},
      {"Boston",       "edge",     "100Mbps", 10, 0.10, 1e7},
  };
  if (!crippleEnabled) {
    spec.nodes[5] = {"Missoula", "tier2", "500Mbps", 5, 0.08, 2e7};
  }
  spec.links = {
      { 4,  7,  575.0, 0.000, backboneQueue, false},
      { 7, 10,  557.0, 0.000, backboneQueue, false},
      {10,  9,  369.0, 0.000, backboneQueue, false},
      {10, 14, 1087.0, 0.004, backboneQueue, false},
      { 9,  8,  537.0, 0.000, backboneQueue, false},
      { 8, 12,  898.0, 0.002, backboneQueue, true },
      { 5, 11, 1617.0, 0.003, backboneQueue, false},
      {11, 15,  572.0, 0.000, backboneQueue, false},
      {14, 13,  362.0, 0.000, backboneQueue, false},
      {13, 12,  729.0, 0.000, backboneQueue, false},
      {12, 15,  662.0, 0.000, backboneQueue, false},
      {15, 16,  263.0, 0.000, backboneQueue, true },
      {14, 21,  413.0, 0.000, backboneQueue, false},
      {21, 25,  913.0, 0.002, backboneQueue, false},
      {25, 30,  525.0, 0.000, backboneQueue, false},
      {25, 24,  458.0, 0.000, backboneQueue, false},
      {24, 18,  346.0, 0.000, backboneQueue, false},
      {24, 28,  572.0, 0.000, backboneQueue, false},
      {14, 20,  569.0, 0.000, backboneQueue, false},
      {20, 19,  316.0, 0.000, backboneQueue, false},
      {19, 18,  306.0, 0.000, backboneQueue, false},
      {18, 17,  249.0, 0.000, backboneQueue, false},
      {17, 16,  172.0, 0.000, backboneQueue, false},
      {28, 29,  375.0, 0.000, backboneQueue, false},
      {29, 27,   55.0, 0.000, backboneQueue, false},
      {29, 31,  199.0, 0.000, backboneQueue, false},
      {31, 32,  130.0, 0.000, backboneQueue, false},
      {15, 22,  497.0, 0.000, backboneQueue, true },
      {22, 26,  279.0, 0.000, backboneQueue, false},
      {26, 33,  644.0, 0.000, backboneQueue, false},
      {22, 23,  185.0, 0.000, backboneQueue, false},
      {23, 27,  360.0, 0.000, backboneQueue, false},
      {32, 33,  306.0, 0.000, backboneQueue, false},
      { 2,  3,  907.0, 0.002, backboneQueue, false},
      { 3,  4,  503.0, 0.000, backboneQueue, false},
      { 3,  6,  955.0, 0.003, backboneQueue, false},
      { 8,  6,  598.0, 0.000, backboneQueue, false},
      { 4,  6,  934.0, 0.003, backboneQueue, false},
      { 0,  1,  194.0, 0.000, backboneQueue, false},
      { 1,  5,  635.0, 0.000, backboneQueue, false},
      { 1,  2,  233.0, 0.000, backboneQueue, false},
      { 1,  6, 1128.0, 0.003, backboneQueue, false},
  };
  // 1:1 host:switch — every switch gets its own host.
  spec.hostToSwitch.resize(spec.nodes.size());
  spec.hostNames.resize(spec.nodes.size());
  for (uint32_t i = 0; i < spec.nodes.size(); ++i) {
    spec.hostToSwitch[i] = i;
    spec.hostNames[i] = spec.nodes[i].name;
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
  return spec;
}

}  // namespace ns3
