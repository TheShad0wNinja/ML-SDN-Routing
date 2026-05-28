#include "scenario/scenario_topo.h"

#include <iostream>

namespace ns3 {

std::vector<uint32_t> ParseIndexCsv(const std::string& csv) {
  std::vector<uint32_t> out;
  std::string tok;
  for (char c : csv) {
    if (c == ',' || c == ' ' || c == '\t') {
      if (!tok.empty()) {
        out.push_back(static_cast<uint32_t>(std::stoul(tok)));
        tok.clear();
      }
    } else {
      tok.push_back(c);
    }
  }
  if (!tok.empty()) out.push_back(static_cast<uint32_t>(std::stoul(tok)));
  return out;
}

TopoSpec FilterTopoSpecBySection(const TopoSpec& full,
                                 const std::vector<uint32_t>& keptIndices) {
  TopoSpec out;
  out.label = full.label + "-section";

  std::vector<int> oldToNew(full.nodes.size(), -1);
  for (uint32_t newIdx = 0; newIdx < keptIndices.size(); ++newIdx) {
    uint32_t oldIdx = keptIndices[newIdx];
    if (oldIdx >= full.nodes.size()) continue;
    if (oldToNew[oldIdx] != -1) continue;
    oldToNew[oldIdx] = static_cast<int>(out.nodes.size());
    out.nodes.push_back(full.nodes[oldIdx]);
  }

  uint32_t droppedXSection = 0;
  for (const auto& l : full.links) {
    if (l.src >= full.nodes.size() || l.dst >= full.nodes.size()) continue;
    int s = oldToNew[l.src], d = oldToNew[l.dst];
    if (s < 0 || d < 0) {
      ++droppedXSection;
      continue;
    }
    LinkSpec rewritten = l;
    rewritten.src = static_cast<uint32_t>(s);
    rewritten.dst = static_cast<uint32_t>(d);
    out.links.push_back(rewritten);
  }

  for (uint32_t h = 0; h < full.hostToSwitch.size(); ++h) {
    uint32_t sw = full.hostToSwitch[h];
    if (sw >= full.nodes.size()) continue;
    int newSw = oldToNew[sw];
    if (newSw < 0) continue;
    out.hostToSwitch.push_back(static_cast<uint32_t>(newSw));
    out.hostNames.push_back(h < full.hostNames.size()
                                ? full.hostNames[h]
                                : full.nodes[sw].name);
  }

  std::cout << "[SECTION] kept " << out.nodes.size() << "/"
            << full.nodes.size() << " switches, " << out.links.size() << "/"
            << full.links.size() << " links (" << droppedXSection
            << " cross-section dropped), " << out.hostToSwitch.size() << "/"
            << full.hostToSwitch.size() << " hosts" << std::endl;
  return out;
}

}  // namespace ns3
