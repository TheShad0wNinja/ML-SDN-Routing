import struct

from . import constants as C


def _tlv(tlv_type: int, value: bytes) -> bytes:
    hdr = (int(tlv_type) << 9) | len(value)
    return struct.pack("!H", hdr) + value


def build_lldp_frame(dpid: int, port_no: int) -> bytes:
    """
    Keep output identical to the previous controller.py implementation:
    - dst MAC: 01:80:c2:00:00:0e
    - src MAC: 00:00:00:00:00:01
    - ethertype: 0x88cc
    - chassis_id TLV: type=1, subtype=7, value=str(dpid)
    - port_id TLV: type=2, subtype=5, value=str(port_no)
    - ttl TLV: type=3, value=uint16(120)
    - end TLV
    """
    ethertype = struct.pack("!H", C.ETH_TYPE_LLDP)

    chassis = _tlv(
        C.LLDP_TLV_TYPE_CHASSIS_ID,
        bytes([C.LLDP_CHASSIS_ID_SUBTYPE_LOCALLY_ASSIGNED]) + str(int(dpid)).encode(),
    )
    port = _tlv(
        C.LLDP_TLV_TYPE_PORT_ID,
        bytes([C.LLDP_PORT_ID_SUBTYPE_IFNAME]) + str(int(port_no)).encode(),
    )
    ttl = _tlv(C.LLDP_TLV_TYPE_TTL, struct.pack("!H", C.LLDP_DEFAULT_TTL_SECONDS))
    end = _tlv(C.LLDP_TLV_TYPE_END, b"")

    return C.LLDP_DST_MAC + C.LLDP_SRC_MAC + ethertype + chassis + port + ttl + end


def parse_lldp_frame(frame: bytes) -> tuple[int | None, int | None]:
    """
    Parse an LLDP Ethernet frame and return (src_dpid, src_port).
    Matches prior behavior: looks for chassis_id and port_id TLVs and
    parses their string payloads into integers.
    """
    offset = C.ETH_HEADER_LEN
    chassis_id: int | None = None
    port_id: int | None = None

    while offset < len(frame) - 1:
        (hdr,) = struct.unpack_from("!H", frame, offset)
        tlv_type = (hdr >> 9) & 0x7F
        ln = hdr & 0x1FF
        offset += 2

        if tlv_type == 0 or ln == 0:
            break
        if offset + ln > len(frame):
            break

        val = frame[offset : offset + ln]
        if tlv_type == C.LLDP_TLV_TYPE_CHASSIS_ID and len(val) > 1:
            try:
                chassis_id = int(val[1:].decode("utf-8", errors="ignore"))
            except Exception:
                pass
        elif tlv_type == C.LLDP_TLV_TYPE_PORT_ID and len(val) > 1:
            try:
                port_id = int(val[1:].decode("utf-8", errors="ignore"))
            except Exception:
                pass

        offset += ln

    return chassis_id, port_id

