import heapq
import json
import os
import struct
import time
import zmq
from collections import defaultdict

from ryu.ofproto import ofproto_v1_3
from ryu.ofproto import ofproto_v1_3_parser

_SCRATCH_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


class DummyDatapath:
    ofproto = ofproto_v1_3
    ofproto_parser = ofproto_v1_3_parser


class Topology:
    def __init__(self):
        self.links = set()

    def add_link(self, d1, p1, d2, p2):
        self.links.add((d1, p1, d2, p2))
        self.links.add((d2, p2, d1, p1))

    def shortest_path(self, src, dst):
        graph = defaultdict(set)
        for d1, p1, d2, p2 in self.links:
            graph[d1].add(d2)
        queue = [(0, src, [src])]
        visited = set()
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

    def get_out_port(self, src, dst):
        for d1, p1, d2, p2 in self.links:
            if d1 == src and d2 == dst:
                return p1
        return None


class SDNController:
    LLDP_DST = bytes.fromhex("0180c200000e")
    LLDP_ETH = 0x88cc
    ETH_IPV4 = 0x0800
    _STATE_PATH = os.path.join(_SCRATCH_ROOT, "data", "state", "sdn_state.json")
    # Stable vis-network node id for the logical control plane (Python + ZMQ bridge).
    _CONTROLLER_NODE_ID = "sdn-controller"

    def __init__(self):
        self.ctx = zmq.Context()
        self.rep = self.ctx.socket(zmq.REP)
        self.rep.bind("tcp://*:5555")

        self.topo = Topology()
        self.mac_to_loc = {}
        self.switch_ports = defaultdict(set)
        self.port_stats = defaultdict(lambda: defaultdict(dict))
        # DPIDs learned from ns-3 after OpenFlow handshake (see TriggerLldp / TriggerStats).
        self.registered_dpids = set()
        self.mac_to_ip = {}
        self._last_state_dump_ts = 0.0

        print("Controller ready. Waiting for ns-3...")
        self._main_loop()

    def _main_loop(self):
        while True:
            data = self.rep.recv()
            reply = self._handle_request(data)
            self.rep.send(reply)

    def _handle_request(self, data):
        if len(data) < 8:
            return self._pack_reply([])

        dpid = struct.unpack("!Q", data[:8])[0]
        payload = data[8:]

        # --- C++ simulated-time triggers (optional binary DPID list after header) ---
        if payload.startswith(b"LLDP"):
            self._parse_registered_dpids(payload, header_len=4)
            return self._pack_reply(self._build_lldp_commands())
        if payload.startswith(b"STATS"):
            self._parse_registered_dpids(payload, header_len=5)
            return self._pack_reply(self._build_stats_commands())
        if payload == b"HELLO":
            return b"READY"

        # --- OpenFlow messages from switches ---
        if len(payload) < 8:
            return self._pack_reply([])

        ver, typ, ln, xid = struct.unpack_from("!BBHL", payload)
        msg = ofproto_v1_3_parser.msg_parser(DummyDatapath(), ver, typ, ln, xid, payload)
        if not msg:
            return self._pack_reply([])

        name = msg.__class__.__name__

        if name == "OFPPacketIn":
            return self._handle_packet_in(dpid, msg)
        elif name == "OFPPortStatsReply":
            return self._handle_port_stats(dpid, msg)
        else:
            return self._pack_reply([])

    # ------------------------------------------------------------------
    #  PacketIn: learn topology / hosts, compute path, return commands
    # ------------------------------------------------------------------
    def _handle_packet_in(self, dpid, msg):
        in_port = self._get_in_port(msg)
        self.switch_ports[dpid].add(in_port)

        # LLDP discovery
        if msg.data and len(msg.data) > 14:
            eth_type = struct.unpack_from("!H", msg.data, 12)[0]
            if eth_type == self.LLDP_ETH:
                src_dpid, src_port = self._parse_lldp(msg.data)
                if src_dpid is not None and src_port is not None:
                    self.topo.add_link(src_dpid, src_port, dpid, in_port)
                    print(f"[TOPO] Link: {src_dpid}:{src_port} -> {dpid}:{in_port}")
                return self._pack_reply([])

        # Normal traffic
        return self._route_data_packet(dpid, in_port, msg)

    def _route_data_packet(self, dpid, in_port, msg):
        if not msg.data or len(msg.data) < 12:
            self._maybe_dump_sdn_state()
            return self._pack_reply([(dpid, self._flood_msg(msg))])

        dst_mac = msg.data[0:6]
        src_mac = msg.data[6:12]
        # self.mac_to_loc[src_mac] = (dpid, in_port)

        is_switch_link = False
        for d1, p1, d2, p2 in self.topo.links:
            if d1 == dpid and p1 == in_port:
                is_switch_link = True
                break

        if not is_switch_link:
            self.mac_to_loc[src_mac] = (dpid, in_port)

        self._learn_ipv4_from_frame(msg.data, src_mac)

        if dst_mac not in self.mac_to_loc:
            self._maybe_dump_sdn_state()
            return self._pack_reply([(dpid, self._flood_msg(msg))])

        dst_dpid, dst_port = self.mac_to_loc[dst_mac]
        commands = []

        if dst_dpid == dpid:
            commands.append((dpid, self._flow_mod_msg(dst_mac, dst_port)))
            commands.append((dpid, self._packet_out_msg(msg, dst_port)))
            self._maybe_dump_sdn_state()
            return self._pack_reply(commands)

        path = self.topo.shortest_path(dpid, dst_dpid)
        if not path or len(path) < 2:
            self._maybe_dump_sdn_state()
            return self._pack_reply([(dpid, self._flood_msg(msg))])

        for i in range(len(path) - 1):
            u, v = path[i], path[i + 1]
            out_p = self.topo.get_out_port(u, v)
            commands.append((u, self._flow_mod_msg(dst_mac, out_p)))

        first_out = self.topo.get_out_port(path[0], path[1])
        commands.append((dpid, self._packet_out_msg(msg, first_out)))
        self._maybe_dump_sdn_state()
        return self._pack_reply(commands)

    # ------------------------------------------------------------------
    #  Statistics
    # ------------------------------------------------------------------
    def _handle_port_stats(self, dpid, msg):
        for stat in msg.body:
            self.port_stats[dpid][stat.port_no] = {
                "rx_packets": stat.rx_packets,
                "tx_packets": stat.tx_packets,
                "rx_bytes": stat.rx_bytes,
                "tx_bytes": stat.tx_bytes,
            }
        print(f"[STATS] DPID {dpid} — {len(msg.body)} ports")
        self._maybe_dump_sdn_state()
        return self._pack_reply([])

    def _learn_ipv4_from_frame(self, data, src_mac):
        if not data or len(data) < 34:
            return
        eth_type = struct.unpack_from("!H", data, 12)[0]
        if eth_type != self.ETH_IPV4:
            return
        ver_ihl = data[14]
        if (ver_ihl >> 4) != 4:
            return
        ihl = (ver_ihl & 0x0F) * 4
        if len(data) < 14 + ihl:
            return
        src_ip = ".".join(str(b) for b in data[14 + 12 : 14 + 16])
        self.mac_to_ip[src_mac] = src_ip

    def _maybe_dump_sdn_state(self):
        now = time.time()
        if now - self._last_state_dump_ts < 0.25:
            return
        self._last_state_dump_ts = now
        links = []
        for d1, p1, d2, p2 in self.topo.links:
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
        hosts = []
        for mac_b, (dpid, port) in self.mac_to_loc.items():
            h = {
                "mac": self._mac_str(mac_b),
                "dpid": dpid,
                "port": port,
            }
            if mac_b in self.mac_to_ip:
                h["ip"] = self.mac_to_ip[mac_b]
            hosts.append(h)
        state = {
            "switches": sorted(self.registered_dpids),
            "hosts": hosts,
            "links": links,
            "controller": {
                "id": self._CONTROLLER_NODE_ID,
                "label": "SDN Controller",
                "detail": "External Python controller (Ryu) via ZMQ; ns3 ZmqOpenFlowController bridge to switches.",
            },
            "control_links": [{"dpid": d} for d in sorted(self.registered_dpids)],
            "stats": {
                str(dpid): {
                    str(pn): dict(vals) for pn, vals in ports.items()
                }
                for dpid, ports in self.port_stats.items()
            },
            "timestamp": now,
        }
        try:
            os.makedirs(os.path.dirname(self._STATE_PATH) or ".", exist_ok=True)
            with open(self._STATE_PATH, "w", encoding="utf-8") as f:
                json.dump(state, f)
        except OSError:
            pass

    def _build_lldp_commands(self):
        commands = []
        switches = set(self.registered_dpids)
        for l in self.topo.links:
            switches.add(l[0])
            switches.add(l[2])
        switches |= set(self.switch_ports.keys())
        for dpid in switches:
            ports = self.switch_ports.get(dpid, set()) or {1, 2, 3, 4}
            for p in ports:
                frame = self._build_lldp_frame(dpid, p)
                parser = ofproto_v1_3_parser
                actions = [parser.OFPActionOutput(p)]
                po = parser.OFPPacketOut(
                    datapath=DummyDatapath(),
                    buffer_id=0xffffffff,
                    in_port=ofproto_v1_3.OFPP_CONTROLLER,
                    actions=actions,
                    data=frame,
                )
                po.serialize()
                commands.append((dpid, po.buf))
        return commands

    def _build_stats_commands(self):
        commands = []
        switches = set(self.registered_dpids)
        for l in self.topo.links:
            switches.add(l[0])
            switches.add(l[2])
        switches |= set(self.switch_ports.keys())
        for dpid in switches:
            req = ofproto_v1_3_parser.OFPPortStatsRequest(
                DummyDatapath(), 0, ofproto_v1_3.OFPP_ANY
            )
            req.serialize()
            commands.append((dpid, req.buf))
        return commands

    # ------------------------------------------------------------------
    #  Helpers
    # ------------------------------------------------------------------
    def _pack_reply(self, commands):
        payload = struct.pack("!H", len(commands))
        for dpid, of_bytes in commands:
            payload += struct.pack("!Q", dpid)
            payload += struct.pack("!I", len(of_bytes))
            payload += of_bytes
        return payload

    def _get_in_port(self, msg):
        for f in msg.match.fields:
            if f.header == ofproto_v1_3.OXM_OF_IN_PORT:
                return f.value
        return 1

    def _parse_registered_dpids(self, payload, header_len):
        off = header_len
        if len(payload) < off + 2:
            return
        (n,) = struct.unpack_from("!H", payload, off)
        off += 2
        for _ in range(n):
            if off + 8 > len(payload):
                break
            self.registered_dpids.add(struct.unpack_from("!Q", payload, off)[0])
            off += 8

    def _mac_str(self, mac_bytes):
        return ":".join(f"{b:02x}" for b in mac_bytes)

    def _flow_mod_msg(self, dst_mac, out_port):
        parser = ofproto_v1_3_parser
        match = parser.OFPMatch(eth_dst=self._mac_str(dst_mac))
        actions = [parser.OFPActionOutput(out_port)]
        inst = [parser.OFPInstructionActions(ofproto_v1_3.OFPIT_APPLY_ACTIONS, actions)]
        fm = parser.OFPFlowMod(
            datapath=DummyDatapath(),
            match=match,
            instructions=inst,
            priority=100,
            table_id=0,
            idle_timeout=60,
        )
        fm.serialize()
        return fm.buf

    def _packet_out_msg(self, msg, out_port):
        parser = ofproto_v1_3_parser
        ofp = ofproto_v1_3
        in_port = self._get_in_port(msg)
        actions = [parser.OFPActionOutput(out_port)]
        data = msg.data if msg.buffer_id == ofp.OFP_NO_BUFFER else None
        po = parser.OFPPacketOut(
            datapath=DummyDatapath(),
            buffer_id=msg.buffer_id,
            in_port=in_port,
            actions=actions,
            data=data,
        )
        po.set_xid(msg.xid)
        po.serialize()
        return po.buf

    def _flood_msg(self, msg):
        parser = ofproto_v1_3_parser
        ofp = ofproto_v1_3
        in_port = self._get_in_port(msg)
        actions = [parser.OFPActionOutput(ofp.OFPP_FLOOD)]
        data = msg.data if msg.buffer_id == ofp.OFP_NO_BUFFER else None
        po = parser.OFPPacketOut(
            datapath=DummyDatapath(),
            buffer_id=msg.buffer_id,
            in_port=in_port,
            actions=actions,
            data=data,
        )
        po.set_xid(msg.xid)
        po.serialize()
        return po.buf

    def _build_lldp_frame(self, dpid, port_no):
        src_mac = bytes.fromhex("000000000001")
        ethertype = struct.pack("!H", self.LLDP_ETH)
        def tlv(t, v):
            hdr = (t << 9) | len(v)
            return struct.pack("!H", hdr) + v
        chassis = tlv(1, bytes([7]) + str(dpid).encode())
        port = tlv(2, bytes([5]) + str(port_no).encode())
        ttl = tlv(3, struct.pack("!H", 120))
        end = tlv(0, b"")
        return self.LLDP_DST + src_mac + ethertype + chassis + port + ttl + end

    def _parse_lldp(self, data):
        offset = 14
        chassis_id = port_id = None
        while offset < len(data) - 1:
            hdr = struct.unpack_from("!H", data, offset)[0]
            t = (hdr >> 9) & 0x7F
            ln = hdr & 0x1FF
            offset += 2
            if t == 0 or ln == 0:
                break
            if offset + ln > len(data):
                break
            val = data[offset:offset + ln]
            if t == 1 and len(val) > 1:
                try: chassis_id = int(val[1:].decode("utf-8", errors="ignore"))
                except: pass
            elif t == 2 and len(val) > 1:
                try: port_id = int(val[1:].decode("utf-8", errors="ignore"))
                except: pass
            offset += ln
        return chassis_id, port_id


if __name__ == "__main__":
    SDNController()
