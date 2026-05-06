from __future__ import annotations

import json
import os
import time
from dataclasses import dataclass
from typing import Any

from . import constants as C
from .ethernet import mac_bytes_to_str


@dataclass
class StateDumper:
    """
    Dumps the controller state to a JSON file at most every 0.25s.
    """

    path: str = C.STATE_PATH
    min_interval_seconds: float = C.STATE_DUMP_MIN_INTERVAL_SECONDS
    _last_dump_ts: float = 0.0

    def maybe_dump(
        self,
        *,
        topo_links: set[tuple[int, int, int, int]],
        registered_dpids: set[int],
        mac_to_loc: dict[bytes, tuple[int, int]],
        mac_to_ip: dict[bytes, str],
        port_stats: dict[int, dict[int, dict[str, Any]]],
    ) -> None:
        now = time.time()
        if now - self._last_dump_ts < self.min_interval_seconds:
            return
        self._last_dump_ts = now

        state = build_state_snapshot(
            timestamp=now,
            topo_links=topo_links,
            registered_dpids=registered_dpids,
            mac_to_loc=mac_to_loc,
            mac_to_ip=mac_to_ip,
            port_stats=port_stats,
        )
        dump_state_json(self.path, state)


def build_state_snapshot(
    *,
    timestamp: float,
    topo_links: set[tuple[int, int, int, int]],
    registered_dpids: set[int],
    mac_to_loc: dict[bytes, tuple[int, int]],
    mac_to_ip: dict[bytes, str],
    port_stats: dict[int, dict[int, dict[str, Any]]],
) -> dict[str, Any]:
    """
    Builds a dictionary of the controller state.
    """
    links: list[dict[str, Any]] = []
    for d1, p1, d2, p2 in topo_links:
        # Keep only one direction (same canonicalization as old code).
        if (d2, p2, d1, p1) < (d1, p1, d2, p2):
            continue
        links.append(
            {
                "src_dpid": d1,
                "src_port": p1,
                "dst_dpid": d2,
                "dst_port": p2,
                "cost": 1,
            }
        )

    hosts: list[dict[str, Any]] = []
    for mac_b, (dpid, port) in mac_to_loc.items():
        h: dict[str, Any] = {
            "mac": mac_bytes_to_str(mac_b),
            "dpid": dpid,
            "port": port,
        }
        if mac_b in mac_to_ip:
            h["ip"] = mac_to_ip[mac_b]
        hosts.append(h)

    return {
        "switches": sorted(registered_dpids),
        "hosts": hosts,
        "links": links,
        "controller": {
            "id": C.CONTROLLER_NODE_ID,
            "label": "SDN Controller",
            "detail": "External Python controller (Ryu) via ZMQ; ns3 ZmqOpenFlowController bridge to switches.",
        },
        "control_links": [{"dpid": d} for d in sorted(registered_dpids)],
        "stats": {
            str(dpid): {str(pn): dict(vals) for pn, vals in ports.items()}
            for dpid, ports in port_stats.items()
        },
        "timestamp": timestamp,
    }


def dump_state_json(path: str, state: dict[str, Any]) -> None:
    try:
        os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
        with open(path, "w", encoding="utf-8") as f:
            json.dump(state, f)
    except OSError:
        # Preserve prior "best effort" behavior.
        pass

