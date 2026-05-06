import heapq
from collections import defaultdict


class Topology:
    def __init__(self):
        self.links: set[tuple[int, int, int, int]] = set()

    def add_link(self, d1: int, p1: int, d2: int, p2: int) -> None:
        self.links.add((d1, p1, d2, p2))
        self.links.add((d2, p2, d1, p1))

    def shortest_path(self, src: int, dst: int) -> list[int] | None:
        graph: dict[int, set[int]] = defaultdict(set)
        for d1, p1, d2, p2 in self.links:
            graph[d1].add(d2)

        queue: list[tuple[int, int, list[int]]] = [(0, src, [src])]
        visited: set[int] = set()
        while queue:
            cost, node, path = heapq.heappop(queue)
            if node == dst:
                return path
            if node in visited:
                continue
            visited.add(node)
            for nbr in graph[node]:
                if nbr not in visited:
                    heapq.heappush(queue, (cost + 1, nbr, path + [nbr]))
        return None

    def get_out_port(self, src: int, dst: int) -> int | None:
        for d1, p1, d2, p2 in self.links:
            if d1 == src and d2 == dst:
                return p1
        return None

    def is_switch_link_port(self, dpid: int, in_port: int) -> bool:
        for d1, p1, d2, p2 in self.links:
            if d1 == dpid and p1 == in_port:
                return True
        return False

