from . import constants as C


def learn_src_ipv4(frame: bytes) -> str | None:
    """
    If `frame` is an Ethernet+IPv4 packet, return the source IPv4 string, else None.
    """
    if not frame or len(frame) < (C.ETH_HEADER_LEN + 20):
        return None

    eth = frame[0:C.ETH_HEADER_LEN]
    eth_type = (eth[C.ETH_TYPE_OFFSET] << 8) | eth[C.ETH_TYPE_OFFSET + 1]
    if eth_type != C.ETH_TYPE_IPV4:
        return None

    ip0 = C.ETH_HEADER_LEN
    ver_ihl = frame[ip0]
    if (ver_ihl >> 4) != 4:
        return None
    ihl = (ver_ihl & 0x0F) * 4
    if len(frame) < ip0 + ihl:
        return None

    src_ip_bytes = frame[ip0 + 12 : ip0 + 16]
    return ".".join(str(b) for b in src_ip_bytes)

