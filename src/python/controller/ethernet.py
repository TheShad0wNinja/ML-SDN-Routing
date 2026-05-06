import struct

from . import constants as C


def parse_eth_header(frame: bytes) -> tuple[bytes, bytes, int] | None:
    """
    Returns (dst_mac, src_mac, eth_type) or None if too short.
    """
    if not frame or len(frame) < C.ETH_HEADER_LEN:
        return None
    dst_mac = frame[0:6]
    src_mac = frame[6:12]
    (eth_type,) = struct.unpack_from("!H", frame, C.ETH_TYPE_OFFSET)
    return dst_mac, src_mac, eth_type


def mac_bytes_to_str(mac_bytes: bytes) -> str:
    return ":".join(f"{b:02x}" for b in mac_bytes)

