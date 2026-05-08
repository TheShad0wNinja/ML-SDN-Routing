#include "topology.h"

#include <queue>

namespace ns3 {

void Topology::AddLink(uint64_t d1, uint32_t p1, uint64_t d2, uint32_t p2) {
    Link l1{d1, p1, d2, p2};
    Link l2{d2, p2, d1, p1};
    m_links.insert(l1);
    m_links.insert(l2);
    m_graph[d1].insert(d2);
    m_graph[d2].insert(d1);
}

void Topology::RemovePort(uint64_t dpid, uint32_t port) {
    // Collect links to remove
    std::vector<Link> to_remove;
    for (auto const& l : m_links) {
        if ((l.d1 == dpid && l.p1 == port) || (l.d2 == dpid && l.p2 == port)) {
            to_remove.push_back(l);
        }
    }
    for (auto const& l : to_remove) {
        m_links.erase(l);
        // update graph adjacency
        auto it = m_graph.find(l.d1);
        if (it != m_graph.end()) {
            it->second.erase(l.d2);
            if (it->second.empty()) m_graph.erase(it);
        }
    }
}

std::optional<std::vector<uint64_t>> Topology::ShortestPath(uint64_t src, uint64_t dst) const {
    if (src == dst) return std::vector<uint64_t>{src};
    std::unordered_set<uint64_t> visited;
    std::queue<std::vector<uint64_t>> q;
    q.push(std::vector<uint64_t>{src});
    visited.insert(src);

    while (!q.empty()) {
        auto path = q.front(); q.pop();
        uint64_t node = path.back();
        auto it = m_graph.find(node);
        if (it == m_graph.end()) continue;
        for (uint64_t nbr : it->second) {
            if (visited.count(nbr)) continue;
            auto new_path = path;
            new_path.push_back(nbr);
            if (nbr == dst) return new_path;
            visited.insert(nbr);
            q.push(std::move(new_path));
        }
    }
    return std::nullopt;
}

std::optional<uint32_t> Topology::GetOutPort(uint64_t src, uint64_t dst) const {
    for (auto const& l : m_links) {
        if (l.d1 == src && l.d2 == dst) return l.p1;
    }
    return std::nullopt;
}

bool Topology::IsSwitchLinkPort(uint64_t dpid, uint32_t in_port) const {
    for (auto const& l : m_links) {
        if (l.d1 == dpid && l.p1 == in_port) return true;
    }
    return false;
}

} // namespace ns3
