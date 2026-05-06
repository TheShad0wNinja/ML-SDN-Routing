from __future__ import annotations

from dataclasses import dataclass

from ryu.ofproto import ofproto_v1_3
from ryu.ofproto import ofproto_v1_3_parser

from . import constants as C
from .dummy_datapath import DummyDatapath


@dataclass(frozen=True)
class FlowDefaults:
    table_id: int = C.OF_FLOW_TABLE_ID
    priority: int = C.OF_FLOW_PRIORITY
    idle_timeout: int = C.OF_FLOW_IDLE_TIMEOUT


def get_in_port(msg) -> int:
    """
    Extract `in_port` from a Ryu message match.
    Preserves prior fallback behavior (returns 1 if not present).
    """
    for f in msg.match.fields:
        if f.header == ofproto_v1_3.OXM_OF_IN_PORT:
            return f.value
    return 1


def build_flow_mod(eth_dst: str, out_port: int, defaults: FlowDefaults = FlowDefaults()) -> bytes:
    parser = ofproto_v1_3_parser

    match = parser.OFPMatch(eth_dst=eth_dst)
    actions = [parser.OFPActionOutput(out_port)]
    inst = [
        parser.OFPInstructionActions(ofproto_v1_3.OFPIT_APPLY_ACTIONS, actions)
    ]

    fm = parser.OFPFlowMod(
        datapath=DummyDatapath(),
        match=match,
        instructions=inst,
        priority=defaults.priority,
        table_id=defaults.table_id,
        idle_timeout=defaults.idle_timeout,
    )
    fm.serialize()
    return fm.buf


def build_flow_delete() -> bytes:
    parser = ofproto_v1_3_parser
    ofp = ofproto_v1_3

    fm = parser.OFPFlowMod(
        datapath=DummyDatapath(),
        command=ofp.OFPFC_DELETE,
        table_id=C.OF_FLOW_TABLE_ID,
        out_port=ofp.OFPP_ANY,
        out_group=ofp.OFPG_ANY,
        match=parser.OFPMatch(),
    )
    fm.serialize()
    return fm.buf


def build_packet_out(msg, out_port: int) -> bytes:
    parser = ofproto_v1_3_parser
    ofp = ofproto_v1_3

    in_port = get_in_port(msg)
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


def build_flood_packet_out(msg) -> bytes:
    parser = ofproto_v1_3_parser
    ofp = ofproto_v1_3

    in_port = get_in_port(msg)
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


def build_lldp_packet_out(port_no: int, frame: bytes) -> bytes:
    """
    Used for controller-originated LLDP discovery packets.
    Preserves existing behavior: buffer_id=0xffffffff, in_port=OFPP_CONTROLLER.
    """
    parser = ofproto_v1_3_parser
    actions = [parser.OFPActionOutput(port_no)]

    po = parser.OFPPacketOut(
        datapath=DummyDatapath(),
        buffer_id=C.OF_BUFFER_ID_NO_BUFFER_U32,
        in_port=ofproto_v1_3.OFPP_CONTROLLER,
        actions=actions,
        data=frame,
    )
    po.serialize()
    return po.buf


def build_port_stats_request() -> bytes:
    req = ofproto_v1_3_parser.OFPPortStatsRequest(
        DummyDatapath(), 0, ofproto_v1_3.OFPP_ANY
    )
    req.serialize()
    return req.buf

