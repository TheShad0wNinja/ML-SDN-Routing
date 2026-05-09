#include "topology.h"

#include <algorithm>
#include <limits>
#include <queue>
#include <set>
#include <utility>

namespace ns3
{

void Topology::AddLink(uint64_t dpid1, uint32_t port1, uint64_t dpid2, uint32_t port2, double costMs)
{
    m_links[dpid1][port1] = {dpid2, port2};
    m_links[dpid2][port2] = {dpid1, port1};

    m_linkPorts[dpid1].insert(port1);
    m_linkPorts[dpid2].insert(port2);

    m_linkCost[dpid1][dpid2] = costMs;
    m_linkCost[dpid2][dpid1] = costMs;

    AddAdjacency(dpid1, dpid2, port1);
    AddAdjacency(dpid2, dpid1, port2);
}

void Topology::AddAdjacency(uint64_t src, uint64_t dst, uint32_t outPort)
{
    m_graph[src].insert(dst);
    m_routing[src][dst] = outPort;
}

void Topology::RemovePort(uint64_t dpid, uint32_t port)
{
    auto dpidIt = m_links.find(dpid);
    if (dpidIt == m_links.end())
    {
        return;
    }

    auto portIt = dpidIt->second.find(port);
    if (portIt == dpidIt->second.end())
    {
        return;
    }

    const LinkEndpoint &localEnd = portIt->second;
    uint64_t peerDpid = localEnd.peerDpid;
    uint32_t peerPort = localEnd.peerPort;

    // Erase reverse mapping
    auto peerDpidIt = m_links.find(peerDpid);
    if (peerDpidIt != m_links.end())
    {
        peerDpidIt->second.erase(peerPort);
        if (peerDpidIt->second.empty())
        {
            m_links.erase(peerDpidIt);
        }
    }

    // Erase local mapping
    dpidIt->second.erase(portIt);
    if (dpidIt->second.empty())
    {
        m_links.erase(dpidIt);
    }

    // Clean up fast-lookup sets
    auto erasePort = [](std::unordered_map<uint64_t, std::unordered_set<uint32_t>> &map, uint64_t key, uint32_t p) {
        auto it = map.find(key);
        if (it == map.end())
        {
            return;
        }
        it->second.erase(p);
        if (it->second.empty())
        {
            map.erase(it);
        }
    };
    erasePort(m_linkPorts, dpid, port);
    erasePort(m_linkPorts, peerDpid, peerPort);

    // Only remove graph edges if no other parallel link exists between the pair
    RemoveAdjacency(dpid, peerDpid);
    RemoveAdjacency(peerDpid, dpid);
}

void Topology::RemoveAdjacency(uint64_t src, uint64_t dst)
{
    bool otherLinkExists = false;
    auto dpidIt = m_links.find(src);
    if (dpidIt != m_links.end())
    {
        for (const auto &[port, end] : dpidIt->second)
        {
            if (end.peerDpid == dst)
            {
                otherLinkExists = true;
                break;
            }
        }
    }

    if (otherLinkExists)
    {
        return;
    }

    auto graphIt = m_graph.find(src);
    if (graphIt != m_graph.end())
    {
        graphIt->second.erase(dst);
        if (graphIt->second.empty())
        {
            m_graph.erase(graphIt);
        }
    }

    auto routeIt = m_routing.find(src);
    if (routeIt != m_routing.end())
    {
        routeIt->second.erase(dst);
        if (routeIt->second.empty())
        {
            m_routing.erase(routeIt);
        }
    }
}

double Topology::GetLinkCost(uint64_t dpid1, uint64_t dpid2) const
{
    auto it = m_linkCost.find(dpid1);
    if (it == m_linkCost.end())
        return 1.0;
    auto jt = it->second.find(dpid2);
    if (jt == it->second.end())
        return 1.0;
    return jt->second;
}

std::optional<std::vector<uint64_t>> Topology::ShortestPath(uint64_t src, uint64_t dst) const
{
    if (src == dst)
    {
        return std::vector<uint64_t>{src};
    }

    if (m_graph.find(src) == m_graph.end())
    {
        return std::nullopt;
    }

    // Dijkstra: priority queue of (cost, node)
    using CostNode = std::pair<double, uint64_t>;
    std::priority_queue<CostNode, std::vector<CostNode>, std::greater<CostNode>> pq;
    std::unordered_map<uint64_t, double> dist;
    std::unordered_map<uint64_t, uint64_t> parent;

    dist[src] = 0.0;
    pq.push({0.0, src});

    while (!pq.empty())
    {
        auto [cost, u] = pq.top();
        pq.pop();

        if (cost > dist[u])
            continue;

        if (u == dst)
        {
            std::vector<uint64_t> path;
            uint64_t node = dst;
            while (node != src)
            {
                path.push_back(node);
                node = parent[node];
            }
            path.push_back(src);
            std::reverse(path.begin(), path.end());
            return path;
        }

        auto it = m_graph.find(u);
        if (it == m_graph.end())
            continue;

        for (uint64_t v : it->second)
        {
            double edgeCost = GetLinkCost(u, v);
            double newDist = cost + edgeCost;
            if (dist.find(v) == dist.end() || newDist < dist[v])
            {
                dist[v] = newDist;
                parent[v] = u;
                pq.push({newDist, v});
            }
        }
    }

    return std::nullopt;
}

std::optional<uint32_t> Topology::GetOutPort(uint64_t src, uint64_t dst) const
{
    auto srcIt = m_routing.find(src);
    if (srcIt == m_routing.end())
    {
        return std::nullopt;
    }

    auto dstIt = srcIt->second.find(dst);
    if (dstIt == srcIt->second.end())
    {
        return std::nullopt;
    }

    return dstIt->second;
}

bool Topology::IsSwitchLinkPort(uint64_t dpid, uint32_t port) const
{
    auto it = m_linkPorts.find(dpid);
    if (it == m_linkPorts.end())
    {
        return false;
    }
    return it->second.count(port) != 0;
}

std::vector<Topology::LinkInfo> Topology::GetAllLinks() const
{
    using LinkKey = std::pair<std::pair<uint64_t, uint32_t>,
                              std::pair<uint64_t, uint32_t>>;
    std::vector<LinkInfo> result;
    std::set<LinkKey> seen;

    for (const auto& dpid_kv : m_links)
    {
        uint64_t dpid1 = dpid_kv.first;
        for (const auto& port_kv : dpid_kv.second)
        {
            uint32_t port1 = port_kv.first;
            const LinkEndpoint& endpoint = port_kv.second;

            uint64_t dpid2 = endpoint.peerDpid;
            uint32_t port2 = endpoint.peerPort;

            // Canonicalise so each undirected link is stored once.
            LinkKey key = (dpid1 < dpid2) || (dpid1 == dpid2 && port1 <= port2)
                ? LinkKey{{dpid1, port1}, {dpid2, port2}}
                : LinkKey{{dpid2, port2}, {dpid1, port1}};

            if (seen.insert(key).second)
            {
                double cost = GetLinkCost(dpid1, dpid2);
                result.push_back({dpid1, port1, dpid2, port2, cost});
            }
        }
    }

    return result;
}

std::unordered_map<uint64_t, std::unordered_set<uint32_t>>
Topology::ComputeSpanningTree() const
{
    std::unordered_map<uint64_t, std::unordered_set<uint32_t>> result;
    if (m_graph.empty()) return result;

    uint64_t root = m_graph.begin()->first;
    for (const auto& kv : m_graph) root = std::min(root, kv.first);

    std::unordered_set<uint64_t> visited;
    std::queue<uint64_t> q;
    visited.insert(root);
    q.push(root);

    while (!q.empty()) {
        uint64_t u = q.front(); q.pop();
        auto it = m_graph.find(u);
        if (it == m_graph.end()) continue;
        for (uint64_t v : it->second) {
            if (visited.count(v)) continue;
            visited.insert(v);
            q.push(v);
            auto uIt = m_routing.find(u);
            if (uIt != m_routing.end()) {
                auto pIt = uIt->second.find(v);
                if (pIt != uIt->second.end()) result[u].insert(pIt->second);
            }
            auto vIt = m_routing.find(v);
            if (vIt != m_routing.end()) {
                auto pIt = vIt->second.find(u);
                if (pIt != vIt->second.end()) result[v].insert(pIt->second);
            }
        }
    }
    return result;
}

std::optional<uint64_t> Topology::GetPeerDpid(uint64_t dpid, uint32_t port) const
{
    auto it = m_links.find(dpid);
    if (it == m_links.end()) return std::nullopt;
    auto pit = it->second.find(port);
    if (pit == it->second.end()) return std::nullopt;
    return pit->second.peerDpid;
}

std::optional<uint32_t> Topology::GetPeerPort(uint64_t dpid, uint32_t port) const
{
    auto it = m_links.find(dpid);
    if (it == m_links.end()) return std::nullopt;
    auto pit = it->second.find(port);
    if (pit == it->second.end()) return std::nullopt;
    return pit->second.peerPort;
}

} // namespace ns3