from ryu.ofproto import ofproto_v1_3
from ryu.ofproto import ofproto_v1_3_parser


class DummyDatapath:
    """
    Minimal datapath object for Ryu message (de)serialization.
    """

    ofproto = ofproto_v1_3
    ofproto_parser = ofproto_v1_3_parser

