import os


_SCRATCH_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# -----------------------------
# ZMQ wire / trigger messages
# -----------------------------
ZMQ_BIND_ENDPOINT = "tcp://*:5555"

TRIGGER_LLDP_PREFIX = b"LLDP"
TRIGGER_STATS_PREFIX = b"STATS"
HELLO_PAYLOAD = b"HELLO"
HELLO_REPLY = b"READY"

TRIGGER_LLDP_HEADER_LEN = 4
TRIGGER_STATS_HEADER_LEN = 5

# -----------------------------
# Ethernet / LLDP constants
# -----------------------------
ETH_HEADER_LEN = 14
ETH_TYPE_OFFSET = 12

ETH_TYPE_LLDP = 0x88CC
ETH_TYPE_IPV4 = 0x0800

LLDP_DST_MAC = bytes.fromhex("0180c200000e")
LLDP_SRC_MAC = bytes.fromhex("000000000001")  # keep existing behavior

LLDP_TLV_TYPE_END = 0
LLDP_TLV_TYPE_CHASSIS_ID = 1
LLDP_TLV_TYPE_PORT_ID = 2
LLDP_TLV_TYPE_TTL = 3

# Subtypes used by current implementation (keep identical behavior)
LLDP_CHASSIS_ID_SUBTYPE_LOCALLY_ASSIGNED = 7
LLDP_PORT_ID_SUBTYPE_IFNAME = 5

LLDP_DEFAULT_TTL_SECONDS = 120

# -----------------------------
# OpenFlow defaults (Ryu builders)
# -----------------------------
OF_FLOW_TABLE_ID = 0
OF_FLOW_PRIORITY = 100
OF_FLOW_IDLE_TIMEOUT = 60

# 0xffffffff literal is used in current code for PacketOut buffer_id in LLDP sends
OF_BUFFER_ID_NO_BUFFER_U32 = 0xFFFFFFFF

# -----------------------------
# Controller behavior defaults
# -----------------------------
STATE_DUMP_MIN_INTERVAL_SECONDS = 1
STATE_PATH = os.path.join(_SCRATCH_ROOT, "data", "state", "sdn_state.json")

# Stable vis-network node id for the logical control plane (Python + ZMQ bridge).
CONTROLLER_NODE_ID = "sdn-controller"

# When we have no port inventory for a switch, we currently fall back to ports 1..4.
FALLBACK_SWITCH_PORTS = (1, 2, 3, 4)

