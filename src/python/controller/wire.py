import struct
from dataclasses import dataclass
from typing import Iterable, Sequence

from . import constants as C


@dataclass(frozen=True)
class Trigger:
    kind: str  # "LLDP" | "STATS"
    dpids: tuple[int, ...]


def parse_request(data: bytes) -> tuple[int, bytes]:
    """
    ns-3 -> Python request format (see `scratch/libs/zmq-openflow-controller.cc`):
      dpid:uint64(be) + payload:bytes
    """
    if len(data) < 8:
        raise ValueError("request too short for dpid header")
    (dpid,) = struct.unpack("!Q", data[:8])
    return dpid, data[8:]


def try_parse_trigger(payload: bytes) -> Trigger | None:
    if payload.startswith(C.TRIGGER_LLDP_PREFIX):
        dpids = _parse_dpid_list(payload, header_len=C.TRIGGER_LLDP_HEADER_LEN)
        return Trigger(kind="LLDP", dpids=dpids)
    if payload.startswith(C.TRIGGER_STATS_PREFIX):
        dpids = _parse_dpid_list(payload, header_len=C.TRIGGER_STATS_HEADER_LEN)
        return Trigger(kind="STATS", dpids=dpids)
    return None


def _parse_dpid_list(payload: bytes, header_len: int) -> tuple[int, ...]:
    off = header_len
    if len(payload) < off + 2:
        return ()
    (n,) = struct.unpack_from("!H", payload, off)
    off += 2
    dpids: list[int] = []
    for _ in range(n):
        if off + 8 > len(payload):
            break
        (dpid,) = struct.unpack_from("!Q", payload, off)
        dpids.append(dpid)
        off += 8
    return tuple(dpids)


def pack_reply(commands: Sequence[tuple[int, bytes]] | Iterable[tuple[int, bytes]]) -> bytes:
    """
    Python -> ns-3 reply format:
      count:uint16(be) + repeated:
        dpid:uint64(be) + len:uint32(be) + of_msg_bytes
    """
    if not isinstance(commands, Sequence):
        commands = list(commands)

    payload = struct.pack("!H", len(commands))
    for dpid, of_bytes in commands:
        payload += struct.pack("!Q", int(dpid))
        payload += struct.pack("!I", len(of_bytes))
        payload += of_bytes
    return payload

