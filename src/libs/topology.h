#ifndef ZMQ_TOPOLOGY_H
#define ZMQ_TOPOLOGY_H

#include <cstdint>
#include <unordered_set>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <optional>

namespace ns3 {

struct Link {
    uint64_t d1;
    uint32_t p1;
    uint64_t d2;
    uint32_t p2;
    bool operator==(Link const& o) const noexcept {
        return d1 == o.d1 && p1 == o.p1 && d2 == o.d2 && p2 == o.p2;
    }
};

struct LinkHash {
    size_t operator()(Link const& l) const noexcept {
        // combine a few hashes — good-enough for small topologies
        size_t h = std::hash<uint64_t>()(l.d1);
        h ^= std::hash<uint32_t>()(l.p1) + 0x9e3779b97f4a7c15ULL + (h<<6) + (h>>2);
        h ^= std::hash<uint64_t>()(l.d2) + 0x9e3779b97f4a7c15ULL + (h<<6) + (h>>2);
        h ^= std::hash<uint32_t>()(l.p2) + 0x9e3779b97f4a7c15ULL + (h<<6) + (h>>2);
        return h;
    }
};

class Topology {
public:
    Topology() = default;

    // Add a bidirectional link (stores both directions)
    void AddLink(uint64_t d1, uint32_t p1, uint64_t d2, uint32_t p2);

    // Remove any links touching (dpid, port)
    void RemovePort(uint64_t dpid, uint32_t port);

    // Return shortest path as vector of dpids, or empty optional if none
    std::optional<std::vector<uint64_t>> ShortestPath(uint64_t src, uint64_t dst) const;

    // Return output port on `src` that leads directly to `dst` (one-hop), or nullopt
    std::optional<uint32_t> GetOutPort(uint64_t src, uint64_t dst) const;

    // True if (dpid,in_port) is a link port
    bool IsSwitchLinkPort(uint64_t dpid, uint32_t in_port) const;

private:
    std::unordered_set<Link, LinkHash> m_links;
    std::unordered_map<uint64_t, std::unordered_set<uint64_t>> m_graph;
};

} // namespace ns3

#endif // ZMQ_TOPOLOGY_H
