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

  // Mutable cost API for ML link-weight adjustment.
  double GetBaseLinkCost(uint64_t dpid1, uint64_t dpid2) const;
  void   SetLinkCost(uint64_t dpid1, uint64_t dpid2, double newCost);
  void   ResetLinkCosts();

  // Layered cost composition: effective cost = base * (1 + congestion) * (1 + ml_delta).
  // Setters recompute and broadcast the effective cost; pathCache is invalidated.
  void   SetLinkBaseCost(uint64_t dpid1, uint64_t dpid2, double baseCost);
  double GetLinkCongestion(uint64_t dpid1, uint64_t dpid2) const;
  void   SetLinkCongestion(uint64_t dpid1, uint64_t dpid2, double congestion);
  double GetLinkMlDelta(uint64_t dpid1, uint64_t dpid2) const;
  void   SetLinkMlDelta(uint64_t dpid1, uint64_t dpid2, double delta);

  // Egress-port capacity (bits/s) stashed when the controller sees
  // the first PORT_STATS reply; 0 if not yet known.
  double GetLinkCapacityBps(uint64_t srcDpid, uint64_t dstDpid) const;
  void   SetLinkCapacityBps(uint64_t srcDpid, uint64_t dstDpid, double capBps);

  struct LinkInfo {
    uint64_t src_dpid;
    uint32_t src_port;
    uint64_t dst_dpid;
    uint32_t dst_port;
    double   delay_ms = 1.0;
    double   cost = 1.0;
    double   base_cost = 1.0;
    double   capacity_bps = 0.0;
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
  std::unordered_map<uint64_t, std::unordered_map<uint64_t, double>> m_baseLinkCost;
  std::unordered_map<uint64_t, std::unordered_map<uint64_t, double>> m_linkCongestion;
  std::unordered_map<uint64_t, std::unordered_map<uint64_t, double>> m_linkMlDelta;
  std::unordered_map<uint64_t, std::unordered_map<uint64_t, double>> m_linkCapacityBps;

  // Path cache: src -> dst -> path (mutable for const ShortestPath method)
  mutable std::unordered_map<uint64_t,
      std::unordered_map<uint64_t, std::vector<uint64_t>>> m_pathCache;

  void AddAdjacency(uint64_t src, uint64_t dst, uint32_t outPort);
  void RemoveAdjacency(uint64_t src, uint64_t dst);

  // Compose effective cost from base × (1 + congestion) × (1 + ml_delta).
  // No-op if the link doesn't exist. Clears path cache.
  void RecomputeCost(uint64_t dpid1, uint64_t dpid2);
};

} // namespace ns3

#endif // TOPOLOGY_H