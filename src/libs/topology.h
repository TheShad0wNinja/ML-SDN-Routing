#ifndef TOPOLOGY_H
#define TOPOLOGY_H

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ns3 {

class Topology {
public:
  Topology() = default;

  bool AddLink(uint64_t dpid1, uint32_t port1, uint64_t dpid2, uint32_t port2, double delayMs = 1.0, double cost = 1.0);
  void RemovePort(uint64_t dpid, uint32_t port);

  std::optional<std::vector<uint64_t>> ShortestPath(uint64_t src,
                                                    uint64_t dst) const;
  std::optional<uint32_t> GetOutPort(uint64_t src, uint64_t dst) const;
  bool IsSwitchLinkPort(uint64_t dpid, uint32_t port) const;
  double GetLinkDelay(uint64_t dpid1, uint64_t dpid2) const;
  double GetLinkCost(uint64_t dpid1, uint64_t dpid2) const;

  struct LinkInfo {
    uint64_t src_dpid;
    uint32_t src_port;
    uint64_t dst_dpid;
    uint32_t dst_port;
    double   delay_ms = 1.0;
    double   cost = 1.0;
  };
  std::vector<LinkInfo> GetAllLinks() const;

  // Returns: dpid -> set of ports that are on the BFS spanning tree
  std::unordered_map<uint64_t, std::unordered_set<uint32_t>> ComputeSpanningTree() const;

  std::optional<uint64_t> GetPeerDpid(uint64_t dpid, uint32_t port) const;
  std::optional<uint32_t> GetPeerPort(uint64_t dpid, uint32_t port) const;

private:
  struct LinkEndpoint {
    uint64_t peerDpid;
    uint32_t peerPort;
  };

  // Primary storage: dpid -> localPort -> {peer, localPort, peerPort}
  std::unordered_map<uint64_t, std::unordered_map<uint32_t, LinkEndpoint>>
      m_links;

  // O(1) "is this port an inter-switch link?" lookup
  std::unordered_map<uint64_t, std::unordered_set<uint32_t>> m_linkPorts;

  // O(1) routing lookup: src -> dst -> outPort
  std::unordered_map<uint64_t, std::unordered_map<uint64_t, uint32_t>>
      m_routing;

  // Weighted adjacency list for Dijkstra
  std::unordered_map<uint64_t, std::unordered_set<uint64_t>> m_graph;

  // Link costs and delays
  std::unordered_map<uint64_t, std::unordered_map<uint64_t, double>> m_linkDelay;
  std::unordered_map<uint64_t, std::unordered_map<uint64_t, double>> m_linkCost;

  // Path cache: src -> dst -> path (mutable for const ShortestPath method)
  mutable std::unordered_map<uint64_t,
      std::unordered_map<uint64_t, std::vector<uint64_t>>> m_pathCache;

  void AddAdjacency(uint64_t src, uint64_t dst, uint32_t outPort);
  void RemoveAdjacency(uint64_t src, uint64_t dst);
};

} // namespace ns3

#endif // TOPOLOGY_H