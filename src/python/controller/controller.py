from __future__ import annotations

import struct
from collections import defaultdict

import zmq
from ryu.ofproto import ofproto_v1_3
from ryu.ofproto import ofproto_v1_3_parser

from . import constants as C
from . import wire
from .dummy_datapath import DummyDatapath
from .ethernet import mac_bytes_to_str, parse_eth_header
from .ipv4 import learn_src_ipv4
from .lldp import build_lldp_frame, parse_lldp_frame
from .openflow_builders import (
    build_flow_mod,
    build_flow_delete,
    build_flood_packet_out,
    build_lldp_packet_out,
    build_packet_out,
    build_port_stats_request,
    get_in_port,
)
from .state import StateDumper
from .topology import Topology


class SDNController:
    def __init__(self) -> None:
        self.ctx = zmq.Context()
        self.rep = self.ctx.socket(zmq.REP)
        self.rep.bind(C.ZMQ_BIND_ENDPOINT)

        self.topo = Topology()
        self.mac_to_loc: dict[bytes, tuple[int, int]] = {}
        self.switch_ports: dict[int, set[int]] = defaultdict(set)
        self.port_stats: dict[int, dict[int, dict]] = defaultdict(lambda: defaultdict(dict))

        # DPIDs learned from ns-3 after OpenFlow handshake (see TriggerLldp / TriggerStats).
        self.registered_dpids: set[int] = set()
        self.mac_to_ip: dict[bytes, str] = {}

        self.state = StateDumper()

        print("Controller ready. Waiting for ns-3...")
        self._main_loop()

    def _main_loop(self) -> None:
        while True:
            data = self.rep.recv()
            reply = self._handle_request(data)
            self.rep.send(reply)

    def _handle_request(self, data: bytes) -> bytes:
        try:
            dpid, payload = wire.parse_request(data)
        except ValueError:
            return wire.pack_reply([])

        trigger = wire.try_parse_trigger(payload)
        if trigger is not None:
            self.registered_dpids |= set(trigger.dpids)
            if trigger.kind == "LLDP":
                return wire.pack_reply(self._build_lldp_commands())
            if trigger.kind == "STATS":
                return wire.pack_reply(self._build_stats_commands())
            return wire.pack_reply([])

        if payload == C.HELLO_PAYLOAD:
            return C.HELLO_REPLY

        if len(payload) < 8:
            return wire.pack_reply([])

        ver, typ, ln, xid = struct.unpack_from("!BBHL", payload)
        msg = ofproto_v1_3_parser.msg_parser(DummyDatapath(), ver, typ, ln, xid, payload)
        if not msg:
            return wire.pack_reply([])

        name = msg.__class__.__name__

        # Log packet type to a simple text file
        with open("packet_types.log", "a") as f:
            f.write(f"{name}\n")

        if name == "OFPPacketIn":
            return self._handle_packet_in(dpid, msg)
        if name == "OFPPortStatus":
            return self._handle_port_status(dpid, msg)
        if name == "OFPPortStatsReply":
            return self._handle_port_stats(dpid, msg)
        return wire.pack_reply([])

    # ------------------------------------------------------------------
    #  PacketIn: learn topology / hosts, compute path, return commands
    # ------------------------------------------------------------------
    def _handle_packet_in(self, dpid: int, msg) -> bytes:
        in_port = get_in_port(msg)
        self.switch_ports[dpid].add(in_port)

        # LLDP discovery
        if msg.data:
            eth = parse_eth_header(msg.data)
            if eth is not None:
                _dst, _src, eth_type = eth
                if eth_type == C.ETH_TYPE_LLDP:
                    src_dpid, src_port = parse_lldp_frame(msg.data)
                    if src_dpid is not None and src_port is not None:
                        self.topo.add_link(src_dpid, src_port, dpid, in_port)
                        print(f"[TOPO] Link: {src_dpid}:{src_port} -> {dpid}:{in_port}")
                    return wire.pack_reply([])

        # Normal traffic
        return self._route_data_packet(dpid, in_port, msg)

    def _route_data_packet(self, dpid: int, in_port: int, msg) -> bytes:
        eth = parse_eth_header(msg.data) if msg.data else None
        if eth is None:
            self.state.maybe_dump(
                topo_links=self.topo.links,
                registered_dpids=self.registered_dpids,
                mac_to_loc=self.mac_to_loc,
                mac_to_ip=self.mac_to_ip,
                port_stats=self.port_stats,
            )
            return wire.pack_reply([(dpid, build_flood_packet_out(msg))])

        dst_mac, src_mac, _eth_type = eth

        # Learn host location only on host-facing ports (not inter-switch links).
        if not self.topo.is_switch_link_port(dpid, in_port):
            self.mac_to_loc[src_mac] = (dpid, in_port)

        # Opportunistically learn src IPv4 for UI.
        src_ip = learn_src_ipv4(msg.data)
        if src_ip is not None:
            self.mac_to_ip[src_mac] = src_ip

        if dst_mac not in self.mac_to_loc:
            self.state.maybe_dump(
                topo_links=self.topo.links,
                registered_dpids=self.registered_dpids,
                mac_to_loc=self.mac_to_loc,
                mac_to_ip=self.mac_to_ip,
                port_stats=self.port_stats,
            )
            return wire.pack_reply([(dpid, build_flood_packet_out(msg))])

        dst_dpid, dst_port = self.mac_to_loc[dst_mac]
        commands: list[tuple[int, bytes]] = []

        if dst_dpid == dpid:
            commands.append((dpid, build_flow_mod(mac_bytes_to_str(dst_mac), dst_port)))
            commands.append((dpid, build_packet_out(msg, dst_port)))
            self.state.maybe_dump(
                topo_links=self.topo.links,
                registered_dpids=self.registered_dpids,
                mac_to_loc=self.mac_to_loc,
                mac_to_ip=self.mac_to_ip,
                port_stats=self.port_stats,
            )
            return wire.pack_reply(commands)

        path = self.topo.shortest_path(dpid, dst_dpid)
        if not path or len(path) < 2:
            self.state.maybe_dump(
                topo_links=self.topo.links,
                registered_dpids=self.registered_dpids,
                mac_to_loc=self.mac_to_loc,
                mac_to_ip=self.mac_to_ip,
                port_stats=self.port_stats,
            )
            return wire.pack_reply([(dpid, build_flood_packet_out(msg))])

        for i in range(len(path) - 1):
            u, v = path[i], path[i + 1]
            out_p = self.topo.get_out_port(u, v)
            commands.append((u, build_flow_mod(mac_bytes_to_str(dst_mac), out_p)))

        first_out = self.topo.get_out_port(path[0], path[1])
        commands.append((dpid, build_packet_out(msg, first_out)))
        self.state.maybe_dump(
            topo_links=self.topo.links,
            registered_dpids=self.registered_dpids,
            mac_to_loc=self.mac_to_loc,
            mac_to_ip=self.mac_to_ip,
            port_stats=self.port_stats,
        )
        return wire.pack_reply(commands)

    # ------------------------------------------------------------------
    #  Statistics
    # ------------------------------------------------------------------
    def _handle_port_stats(self, dpid: int, msg) -> bytes:
        for stat in msg.body:
            self.port_stats[dpid][stat.port_no] = {
                "rx_packets": stat.rx_packets,
                "tx_packets": stat.tx_packets,
                "rx_bytes": stat.rx_bytes,
                "tx_bytes": stat.tx_bytes,
            }
        print(f"[STATS] DPID {dpid} — {len(msg.body)} ports")
        self.state.maybe_dump(
            topo_links=self.topo.links,
            registered_dpids=self.registered_dpids,
            mac_to_loc=self.mac_to_loc,
            mac_to_ip=self.mac_to_ip,
            port_stats=self.port_stats,
        )
        return wire.pack_reply([])

    def _handle_port_status(self, dpid: int, msg) -> bytes:
        desc = getattr(msg, "desc", None)
        port_no = getattr(desc, "port_no", None)
        if port_no is None:
            return wire.pack_reply([])

        reason = getattr(msg, "reason", None)
        port_is_down = bool(getattr(desc, "state", 0) & ofproto_v1_3.OFPPS_LINK_DOWN)
        port_was_removed = reason == ofproto_v1_3.OFPPR_DELETE or port_is_down

        if port_was_removed:
            self.topo.remove_port(dpid, port_no)

            stale_macs = [mac for mac, loc in self.mac_to_loc.items() if loc == (dpid, port_no)]
            for mac in stale_macs:
                self.mac_to_loc.pop(mac, None)
                self.mac_to_ip.pop(mac, None)

            if dpid in self.port_stats:
                self.port_stats[dpid].pop(port_no, None)

            self.switch_ports[dpid].discard(port_no)

            print(f"[PORT] Removed dpid={dpid} port={port_no} reason={reason}")
            self.state.maybe_dump(
                topo_links=self.topo.links,
                registered_dpids=self.registered_dpids,
                mac_to_loc=self.mac_to_loc,
                mac_to_ip=self.mac_to_ip,
                port_stats=self.port_stats,
            )
            return wire.pack_reply([(dpid, build_flow_delete())])

        self.switch_ports[dpid].add(port_no)
        print(f"[PORT] Added/updated dpid={dpid} port={port_no} reason={reason}")
        self.state.maybe_dump(
            topo_links=self.topo.links,
            registered_dpids=self.registered_dpids,
            mac_to_loc=self.mac_to_loc,
            mac_to_ip=self.mac_to_ip,
            port_stats=self.port_stats,
        )
        return wire.pack_reply([])

    # ------------------------------------------------------------------
    #  Controller-originated commands
    # ------------------------------------------------------------------
    def _all_known_switches(self) -> set[int]:
        switches = set(self.registered_dpids)
        for d1, p1, d2, p2 in self.topo.links:
            switches.add(d1)
            switches.add(d2)
        switches |= set(self.switch_ports.keys())
        return switches

    def _build_lldp_commands(self) -> list[tuple[int, bytes]]:
        commands: list[tuple[int, bytes]] = []
        for dpid in self._all_known_switches():
            ports = self.switch_ports.get(dpid, set()) or set(C.FALLBACK_SWITCH_PORTS)
            for p in ports:
                frame = build_lldp_frame(dpid, p)
                commands.append((dpid, build_lldp_packet_out(p, frame)))
        return commands

    def _build_stats_commands(self) -> list[tuple[int, bytes]]:
        commands: list[tuple[int, bytes]] = []
        for dpid in self._all_known_switches():
            commands.append((dpid, build_port_stats_request()))
        return commands

