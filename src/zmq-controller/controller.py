import zmq
from ryu.ofproto import ofproto_v1_3
from ryu.ofproto import ofproto_v1_3_parser


class DummyDatapath:
    """Minimal datapath stand-in so Ryu's parser/serializer works."""
    ofproto = ofproto_v1_3
    ofproto_parser = ofproto_v1_3_parser


def get_in_port(msg):
    """Extract in_port from the OFPMatch fields list."""
    for field in msg.match.fields:
        if field.header == ofproto_v1_3.OXM_OF_IN_PORT:
            return field.value
    return 1  # Safe fallback


def make_flood_response(msg):
    """
    Build an OFPPacketOut that floods the packet.
    This is enough for ping to work across switches.
    """
    parser = ofproto_v1_3_parser
    ofp = ofproto_v1_3

    in_port = get_in_port(msg)

    actions = [parser.OFPActionOutput(ofp.OFPP_FLOOD)]

    # Only include raw packet data if the switch didn't buffer it
    data = None
    if msg.buffer_id == ofp.OFP_NO_BUFFER:
        data = msg.data

    pkt_out = parser.OFPPacketOut(
        datapath=DummyDatapath(),
        buffer_id=msg.buffer_id,
        in_port=in_port,
        actions=actions,
        data=data
    )
    pkt_out.set_xid(msg.xid)
    pkt_out.serialize()
    return pkt_out.buf


def main():
    print("Starting Synchronous ZMQ OpenFlow Controller on port 5555...")
    context = zmq.Context()
    socket = context.socket(zmq.REP)
    socket.bind("tcp://*:5555")

    while True:
        try:
            data = socket.recv()
        except zmq.ZMQError as e:
            print(f"ZMQ receive error: {e}")
            continue

        if not data:
            socket.send(b"")
            continue

        dummy_dp = DummyDatapath()

        try:
            # msg_parser returns a LIST of messages
            # in newer ryu versions msg_parser takes (datapath, version, msg_type, msg_len, xid, buf)
            # For ofproto_v1_3, let's unpack header first:
            import struct
            version, msg_type, msg_len, xid = struct.unpack_from("!BBHL", data)
            msg = ofproto_v1_3_parser.msg_parser(dummy_dp, version, msg_type, msg_len, xid, data)
            if msg:
                msgs = [msg]
            else:
                msgs = []
        except Exception as e:
            print(f"Parse error: {e}")
            socket.send(b"")
            continue

        if not msgs:
            socket.send(b"")
            continue

        msg = msgs[0]
        name = msg.__class__.__name__
        print(f"Recv: {name} xid={msg.xid}")

        reply_bytes = b""

        if name == "OFPPacketIn":
            print(f"  -> PacketIn buffer={msg.buffer_id} len={msg.total_len}")
            try:
                reply_bytes = make_flood_response(msg)
                print(f"  -> Sending PacketOut ({len(reply_bytes)} bytes)")
            except Exception as e:
                print(f"  -> Build error: {e}")
        else:
            print(f"  -> Ignoring {name}")

        socket.send(reply_bytes)


if __name__ == "__main__":
    main()