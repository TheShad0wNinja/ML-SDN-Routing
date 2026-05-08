#include "zmq-openflow-controller.h"
#include "ns3/log.h"
#include "openflow_builders.h"
#include <arpa/inet.h>
#include <cstring>
#include <vector>
#include <string>
#include <sstream>
#include <cstdio>

namespace ns3 {

NS_LOG_COMPONENT_DEFINE("ZmqOpenFlowController");

TypeId ZmqOpenFlowController::GetTypeId() {
    static TypeId tid = TypeId("ns3::ZmqOpenFlowController")
        .SetParent<OFSwitch13Controller>()
        .SetGroupName("OpenFlow13")
        .AddConstructor<ZmqOpenFlowController>();
    return tid;
}

ZmqOpenFlowController::ZmqOpenFlowController() {}
ZmqOpenFlowController::~ZmqOpenFlowController() {}

static uint64_t Htonll(uint64_t v) {
    uint32_t hi = htonl((uint32_t)(v >> 32));
    uint32_t lo = htonl((uint32_t)v);
    return ((uint64_t)hi << 32) | lo;
}
static uint64_t Ntohll(uint64_t v) {
    uint32_t hi = ntohl((uint32_t)(v >> 32));
    uint32_t lo = ntohl((uint32_t)v);
    return ((uint64_t)hi << 32) | lo;
}

void ZmqOpenFlowController::StartApplication() {
    NS_LOG_INFO("Starting ZMQ controller bridge...");
    // m_zmqContext = std::make_unique<zmq::context_t>(1);
    // m_socket = std::make_unique<zmq::socket_t>(*m_zmqContext, zmq::socket_type::req);
    // m_socket->set(zmq::sockopt::rcvtimeo, 5000); // 5 s timeout
    // m_socket->connect("tcp://127.0.0.1:5555");

    // Startup sync — works regardless of who starts first
    // zmq::message_t hello(5);
    // memcpy(hello.data(), "HELLO", 5);
    // m_socket->send(hello, zmq::send_flags::none);
    // zmq::message_t ack;
    // if (m_socket->recv(ack, zmq::recv_flags::none)) {
    //     NS_LOG_INFO("Python controller connected.");
    // }

    // Simulated-time triggers — C++ decides WHEN, Python decides WHAT
    Simulator::Schedule(Seconds(5), &ZmqOpenFlowController::TriggerLldp, this);
    Simulator::Schedule(Seconds(10), &ZmqOpenFlowController::TriggerStats, this);

    OFSwitch13Controller::StartApplication();
}

void ZmqOpenFlowController::StopApplication() {
    m_socket->close();
    m_zmqContext->close();
    OFSwitch13Controller::StopApplication();
}

void ZmqOpenFlowController::HandshakeSuccessful(Ptr<const RemoteSwitch> swtch) {
    NS_LOG_FUNCTION(this << swtch);

    // Get the switch datapath ID
    uint64_t swDpId = swtch->GetDpId();

    m_switchMap[swDpId] = swtch;
    // initialize port set for this switch
    m_switchPorts[swDpId] = std::unordered_set<uint32_t>();

    // After a successful handshake, let's install the table-miss entry, setting
    // to 128 bytes the maximum amount of data from a packet that should be sent
    // to the controller.
    DpctlExecute(swDpId, "flow-mod cmd=add,table=0,prio=0 apply:output=ctrl:128");

    // Configure te switch to buffer packets and send only the first 128 bytes
    // of each packet sent to the controller when not using an output action to
    // the OFPP_CONTROLLER logical port.
    DpctlExecute(swDpId, "set-config miss=128");
}

// ------------------------------------------------------------------
//  Core: send to Python, block, unpack reply, send to switches
// ------------------------------------------------------------------
void ZmqOpenFlowController::ExchangeWithPython(uint64_t dpid,
                                               const uint8_t* payload,
                                               size_t len) {
    // zmq::message_t req(len + 8);
    // uint64_t dpid_nbo = Htonll(dpid);
    // memcpy(req.data(), &dpid_nbo, 8);
    // if (len > 0) memcpy((uint8_t*)req.data() + 8, payload, len);

    // if (!m_socket->send(req, zmq::send_flags::none)) return;

    // zmq::message_t reply;
    // if (!m_socket->recv(reply, zmq::recv_flags::none)) return;
    // if (reply.size() < 2) return;

    // uint8_t* p = (uint8_t*)reply.data();
    // size_t rem = reply.size();
    // uint16_t count = ntohs(*(uint16_t*)p);
    // p += 2; rem -= 2;

    // for (uint16_t i = 0; i < count && rem >= 12; ++i) {
    //     uint64_t target_dpid = Ntohll(*(uint64_t*)p);
    //     p += 8; rem -= 8;
    //     uint32_t msg_len = ntohl(*(uint32_t*)p);
    //     p += 4; rem -= 4;
    //     if (rem < msg_len) break;

    //     auto it = m_switchMap.find(target_dpid);
    //     if (it != m_switchMap.end()) {
    //         struct ofl_msg_header* msg = nullptr;
    //         uint32_t xid = 0;
    //         ofl_err err = ofl_msg_unpack(p, msg_len, &msg, &xid, nullptr);
    //         if (!err && msg) {
    //             SendToSwitch(it->second, msg, xid);
    //             ofl_msg_free(msg, nullptr);
    //         }
    //     }
    //     p += msg_len; rem -= msg_len;
    // }
}

// ------------------------------------------------------------------
//  OpenFlow event handlers — HandlePacketIn implements routing logic
// ------------------------------------------------------------------
ofl_err ZmqOpenFlowController::HandlePacketIn(struct ofl_msg_packet_in* msg,
                                              Ptr<const RemoteSwitch> swtch,
                                              uint32_t xid) {
    uint64_t dpid = swtch->GetDpId();

    // Extract input port from the OpenFlow match
    uint32_t inPort = 0;
    size_t portLen = OXM_LENGTH(OXM_OF_IN_PORT);
    struct ofl_match_tlv* input = oxm_match_lookup(OXM_OF_IN_PORT, (struct ofl_match*)msg->match);
    if (input && input->value) {
        memcpy(&inPort, input->value, portLen);
    }

    // Record seen port
    m_switchPorts[dpid].insert(inPort);

    // LLDP discovery: check payload ethertype
    if (msg->data && msg->data_length >= 14) {
        const uint8_t* data = msg->data;
        uint16_t ethertype = (uint16_t)data[12] << 8 | (uint16_t)data[13];
        if (ethertype == 0x88CC) {
            // Parse TLVs to find chassis_id and port_id as integers
            size_t off = 14;
            uint64_t chassis_id = 0;
            uint32_t port_id = 0;
            while (off + 2 <= msg->data_length) {
                uint16_t hdr = (uint16_t)data[off] << 8 | (uint16_t)data[off+1];
                off += 2;
                uint16_t tlv_type = (hdr >> 9) & 0x7f;
                uint16_t ln = hdr & 0x1ff;
                if (tlv_type == 0 || ln == 0) break;
                if (off + ln > msg->data_length) break;
                const uint8_t* val = data + off;
                if (tlv_type == 1 && ln > 1) {
                    try {
                        std::string s((const char*)(val+1), ln-1);
                        chassis_id = std::stoull(s);
                    } catch (...) { }
                } else if (tlv_type == 2 && ln > 1) {
                    try {
                        std::string s((const char*)(val+1), ln-1);
                        port_id = static_cast<uint32_t>(std::stoul(s));
                    } catch (...) { }
                }
                off += ln;
            }
            if (chassis_id != 0 && port_id != 0) {
                m_topology.AddLink(chassis_id, port_id, dpid, inPort);
                NS_LOG_INFO("[TOPO] Link: " << chassis_id << ":" << port_id << " -> " << dpid << ":" << inPort);
            }
            ofl_msg_free((struct ofl_msg_header*)msg, nullptr);
            return 0;
        }
    }

    // Normal traffic: get ethernet src/dst from match
    struct ofl_match_tlv* ethSrc = oxm_match_lookup(OXM_OF_ETH_SRC, (struct ofl_match*)msg->match);
    struct ofl_match_tlv* ethDst = oxm_match_lookup(OXM_OF_ETH_DST, (struct ofl_match*)msg->match);
    if (!ethSrc || !ethDst || !ethSrc->value || !ethDst->value) {
        // Flood: can't parse MACs
        struct ofl_msg_packet_out* po = (struct ofl_msg_packet_out*)malloc(sizeof(*po));
        memset(po, 0, sizeof(*po));
        po->header.type = OFPT_PACKET_OUT;
        po->buffer_id = msg->buffer_id;
        po->in_port = inPort;
        if (msg->buffer_id == OFP_NO_BUFFER && msg->data && msg->data_length > 0) {
            po->data_length = msg->data_length;
            po->data = (uint8_t*)malloc(msg->data_length);
            memcpy(po->data, msg->data, msg->data_length);
        } else {
            po->data_length = 0;
            po->data = nullptr;
        }
        struct ofl_action_output* a = (struct ofl_action_output*)malloc(sizeof(*a));
        a->header.type = OFPAT_OUTPUT;
        a->header.len = sizeof(*a);
        a->port = OFPP_FLOOD;
        a->max_len = 0;
        po->actions_num = 1;
        po->actions = (struct ofl_action_header**)malloc(sizeof(struct ofl_action_header*));
        po->actions[0] = (struct ofl_action_header*)a;
        SendToSwitch(swtch, (struct ofl_msg_header*)po, 0);
        free(po->actions);
        free(a);
        if (po->data) free(po->data);
        free(po);
        ofl_msg_free((struct ofl_msg_header*)msg, nullptr);
        return 0;
    }

    // Convert MAC bytes to uint64 keys
    uint64_t src_mac = 0;
    uint64_t dst_mac = 0;
    for (int i = 0; i < 6; ++i) {
        src_mac = (src_mac << 8) | (uint8_t)ethSrc->value[i];
        dst_mac = (dst_mac << 8) | (uint8_t)ethDst->value[i];
    }

    // Learn host location if this port is not a switch-link port
    if (!m_topology.IsSwitchLinkPort(dpid, inPort)) {
        m_macToLoc[src_mac] = std::make_pair(dpid, inPort);
    }

    // Check if destination MAC is known
    auto dstIt = m_macToLoc.find(dst_mac);
    if (dstIt == m_macToLoc.end()) {
        // Unknown destination — flood
        struct ofl_msg_packet_out* po = (struct ofl_msg_packet_out*)malloc(sizeof(*po));
        memset(po, 0, sizeof(*po));
        po->header.type = OFPT_PACKET_OUT;
        po->buffer_id = msg->buffer_id;
        po->in_port = inPort;
        if (msg->buffer_id == OFP_NO_BUFFER && msg->data && msg->data_length > 0) {
            po->data_length = msg->data_length;
            po->data = (uint8_t*)malloc(msg->data_length);
            memcpy(po->data, msg->data, msg->data_length);
        } else {
            po->data_length = 0;
            po->data = nullptr;
        }
        struct ofl_action_output* a = (struct ofl_action_output*)malloc(sizeof(*a));
        a->header.type = OFPAT_OUTPUT;
        a->header.len = sizeof(*a);
        a->port = OFPP_FLOOD;
        a->max_len = 0;
        po->actions_num = 1;
        po->actions = (struct ofl_action_header**)malloc(sizeof(struct ofl_action_header*));
        po->actions[0] = (struct ofl_action_header*)a;
        SendToSwitch(swtch, (struct ofl_msg_header*)po, 0);
        free(po->actions);
        free(a);
        if (po->data) free(po->data);
        free(po);
        ofl_msg_free((struct ofl_msg_header*)msg, nullptr);
        return 0;
    }

    // Destination known
    uint64_t dst_dpid = dstIt->second.first;
    uint32_t dst_port = dstIt->second.second;

    if (dst_dpid == dpid) {
        // Local destination: install flow and send packet_out
        char macbuf[32];
        snprintf(macbuf, sizeof(macbuf), "%02x:%02x:%02x:%02x:%02x:%02x",
                 (int)ethDst->value[0], (int)ethDst->value[1], (int)ethDst->value[2],
                 (int)ethDst->value[3], (int)ethDst->value[4], (int)ethDst->value[5]);
        std::ostringstream cmd;
        cmd << "flow-mod cmd=add,table=0,prio=100 eth_dst=" << macbuf << ",apply:output=" << dst_port;
        DpctlExecute(dst_dpid, cmd.str());

        struct ofl_msg_packet_out* po = (struct ofl_msg_packet_out*)malloc(sizeof(*po));
        memset(po, 0, sizeof(*po));
        po->header.type = OFPT_PACKET_OUT;
        po->buffer_id = msg->buffer_id;
        po->in_port = inPort;
        if (msg->buffer_id == OFP_NO_BUFFER && msg->data && msg->data_length > 0) {
            po->data_length = msg->data_length;
            po->data = (uint8_t*)malloc(msg->data_length);
            memcpy(po->data, msg->data, msg->data_length);
        } else {
            po->data_length = 0;
            po->data = nullptr;
        }
        struct ofl_action_output* a = (struct ofl_action_output*)malloc(sizeof(*a));
        a->header.type = OFPAT_OUTPUT;
        a->header.len = sizeof(*a);
        a->port = dst_port;
        a->max_len = 0;
        po->actions_num = 1;
        po->actions = (struct ofl_action_header**)malloc(sizeof(struct ofl_action_header*));
        po->actions[0] = (struct ofl_action_header*)a;
        SendToSwitch(swtch, (struct ofl_msg_header*)po, 0);
        free(po->actions);
        free(a);
        if (po->data) free(po->data);
        free(po);
        ofl_msg_free((struct ofl_msg_header*)msg, nullptr);
        return 0;
    }

    // Multi-hop path computation
    auto opt_path = m_topology.ShortestPath(dpid, dst_dpid);
    if (!opt_path || opt_path->size() < 2) {
        // No path found: flood
        struct ofl_msg_packet_out* po = (struct ofl_msg_packet_out*)malloc(sizeof(*po));
        memset(po, 0, sizeof(*po));
        po->header.type = OFPT_PACKET_OUT;
        po->buffer_id = msg->buffer_id;
        po->in_port = inPort;
        if (msg->buffer_id == OFP_NO_BUFFER && msg->data && msg->data_length > 0) {
            po->data_length = msg->data_length;
            po->data = (uint8_t*)malloc(msg->data_length);
            memcpy(po->data, msg->data, msg->data_length);
        } else {
            po->data_length = 0;
            po->data = nullptr;
        }
        struct ofl_action_output* a = (struct ofl_action_output*)malloc(sizeof(*a));
        a->header.type = OFPAT_OUTPUT;
        a->header.len = sizeof(*a);
        a->port = OFPP_FLOOD;
        a->max_len = 0;
        po->actions_num = 1;
        po->actions = (struct ofl_action_header**)malloc(sizeof(struct ofl_action_header*));
        po->actions[0] = (struct ofl_action_header*)a;
        SendToSwitch(swtch, (struct ofl_msg_header*)po, 0);
        free(po->actions);
        free(a);
        if (po->data) free(po->data);
        free(po);
        ofl_msg_free((struct ofl_msg_header*)msg, nullptr);
        return 0;
    }

    std::vector<uint64_t> path = *opt_path;
    // Install flows along path
    for (size_t i = 0; i + 1 < path.size(); ++i) {
        uint64_t u = path[i];
        uint64_t v = path[i+1];
        auto outp_opt = m_topology.GetOutPort(u, v);
        if (!outp_opt) continue;
        uint32_t outp = *outp_opt;
        char macbuf[32];
        snprintf(macbuf, sizeof(macbuf), "%02x:%02x:%02x:%02x:%02x:%02x",
                 (int)ethDst->value[0], (int)ethDst->value[1], (int)ethDst->value[2],
                 (int)ethDst->value[3], (int)ethDst->value[4], (int)ethDst->value[5]);
        std::ostringstream cmd;
        cmd << "flow-mod cmd=add,table=0,prio=100 eth_dst=" << macbuf << ",apply:output=" << outp;
        DpctlExecute(u, cmd.str());
    }

    // Send packet_out from source to first next-hop
    auto first_out_opt = m_topology.GetOutPort(path[0], path[1]);
    if (first_out_opt) {
        uint32_t first_out = *first_out_opt;
        struct ofl_msg_packet_out* po = (struct ofl_msg_packet_out*)malloc(sizeof(*po));
        memset(po, 0, sizeof(*po));
        po->header.type = OFPT_PACKET_OUT;
        po->buffer_id = msg->buffer_id;
        po->in_port = inPort;
        if (msg->buffer_id == OFP_NO_BUFFER && msg->data && msg->data_length > 0) {
            po->data_length = msg->data_length;
            po->data = (uint8_t*)malloc(msg->data_length);
            memcpy(po->data, msg->data, msg->data_length);
        } else {
            po->data_length = 0;
            po->data = nullptr;
        }
        struct ofl_action_output* a = (struct ofl_action_output*)malloc(sizeof(*a));
        a->header.type = OFPAT_OUTPUT;
        a->header.len = sizeof(*a);
        a->port = first_out;
        a->max_len = 0;
        po->actions_num = 1;
        po->actions = (struct ofl_action_header**)malloc(sizeof(struct ofl_action_header*));
        po->actions[0] = (struct ofl_action_header*)a;
        SendToSwitch(swtch, (struct ofl_msg_header*)po, 0);
        free(po->actions);
        free(a);
        if (po->data) free(po->data);
        free(po);
    }

    ofl_msg_free((struct ofl_msg_header*)msg, nullptr);
    return 0;
}

ofl_err ZmqOpenFlowController::HandlePortStatus(struct ofl_msg_port_status* msg,
                                                Ptr<const RemoteSwitch> swtch,
                                                uint32_t xid) {
    uint64_t dpid = swtch->GetDpId();
    uint8_t* tx_buf = nullptr;
    size_t tx_len = 0;

    ofl_err pack_err = ofl_msg_pack((struct ofl_msg_header*)msg,
                                    xid,
                                    &tx_buf,
                                    &tx_len,
                                    nullptr);

    if (!pack_err) {
        // Before forwarding to Python, update local controller state
        if (msg && msg->desc) {
            uint32_t port_no = msg->desc->port_no;
            // reason: OFPPR_ADD=0, OFPPR_DELETE=1, OFPPR_MODIFY=2
            if (msg->reason == 1) { // delete
                // remove from switch ports and topology
                auto it = m_switchPorts.find(dpid);
                if (it != m_switchPorts.end()) {
                    it->second.erase(port_no);
                }
                m_topology.RemovePort(dpid, port_no);

                // remove any MAC mappings pointing to this (dpid,port)
                for (auto mit = m_macToLoc.begin(); mit != m_macToLoc.end();) {
                    if (mit->second.first == dpid && mit->second.second == port_no) {
                        mit = m_macToLoc.erase(mit);
                    } else {
                        ++mit;
                    }
                }
            } else {
                // add/modify -> record port as known link port (LLDP will discover peers)
                m_switchPorts[dpid].insert(port_no);
            }
        }

        ExchangeWithPython(dpid, tx_buf, tx_len);
        free(tx_buf);
    }

    return OFSwitch13Controller::HandlePortStatus(msg, swtch, xid);
}

ofl_err ZmqOpenFlowController::HandleMultipartReply(
    struct ofl_msg_multipart_reply_header* msg,
    Ptr<const RemoteSwitch> swtch,
    uint32_t xid) {
    uint64_t dpid = swtch->GetDpId();
    uint8_t* tx_buf = nullptr;
    size_t tx_len = 0;

    ofl_err pack_err = ofl_msg_pack(
        (struct ofl_msg_header*)msg, xid, &tx_buf, &tx_len, nullptr);

    if (!pack_err) {
        ExchangeWithPython(dpid, tx_buf, tx_len);
        free(tx_buf);
    }

    return OFSwitch13Controller::HandleMultipartReply(msg, swtch, xid);
}

// ------------------------------------------------------------------
//  Simulated-time triggers
// ------------------------------------------------------------------
void ZmqOpenFlowController::TriggerLldp() {
    std::vector<uint8_t> buf{'L', 'L', 'D', 'P'};
    uint16_t n = static_cast<uint16_t>(m_switchMap.size());
    uint16_t n_bo = htons(n);
    buf.insert(buf.end(), reinterpret_cast<uint8_t*>(&n_bo),
               reinterpret_cast<uint8_t*>(&n_bo) + sizeof n_bo);
    for (const auto& kv : m_switchMap)
    {
        uint64_t d_bo = Htonll(kv.first);
        buf.insert(buf.end(), reinterpret_cast<uint8_t*>(&d_bo),
                   reinterpret_cast<uint8_t*>(&d_bo) + sizeof d_bo);
    }
    ExchangeWithPython(0, buf.data(), buf.size());

    // Also generate LLDP Ethernet frames locally and send PacketOuts
    for (const auto& kv : m_switchMap) {
        uint64_t src_dpid = kv.first;
        Ptr<const RemoteSwitch> sw = kv.second;

        auto it = m_switchPorts.find(src_dpid);
        if (it == m_switchPorts.end() || it->second.empty()) {
            // fallback ports 1..4 when no inventory
            for (uint32_t p = 1; p <= 4; ++p) {
                std::vector<uint8_t> frame = BuildLldpFrame(src_dpid, p);
                struct ofl_msg_header* po = BuildLldpPacketOut(p, frame.data(), frame.size());
                if (po) {
                    SendToSwitch(sw, po, 0);
                    ofl_msg_free(po, nullptr);
                }
            }
        } else {
            for (uint32_t p : it->second) {
                std::vector<uint8_t> frame = BuildLldpFrame(src_dpid, p);
                struct ofl_msg_header* po = BuildLldpPacketOut(p, frame.data(), frame.size());
                if (po) {
                    SendToSwitch(sw, po, 0);
                    ofl_msg_free(po, nullptr);
                }
            }
        }
    }

    Simulator::Schedule(Seconds(5), &ZmqOpenFlowController::TriggerLldp, this);
}

void ZmqOpenFlowController::TriggerStats() {
    std::vector<uint8_t> buf{'S', 'T', 'A', 'T', 'S'};
    uint16_t n = static_cast<uint16_t>(m_switchMap.size());
    uint16_t n_bo = htons(n);
    buf.insert(buf.end(), reinterpret_cast<uint8_t*>(&n_bo),
               reinterpret_cast<uint8_t*>(&n_bo) + sizeof n_bo);
    for (const auto& kv : m_switchMap)
    {
        uint64_t d_bo = Htonll(kv.first);
        buf.insert(buf.end(), reinterpret_cast<uint8_t*>(&d_bo),
                   reinterpret_cast<uint8_t*>(&d_bo) + sizeof d_bo);
    }
    ExchangeWithPython(0, buf.data(), buf.size());

    // Additionally, send a native PORT_STATS request directly from C++ to
    // each registered switch. This reduces turnaround time for stats replies
    // and allows the controller to maintain port stats without a Python
    // round-trip. We still call ExchangeWithPython above for existing behavior.
    for (const auto& kv : m_switchMap) {
        Ptr<const RemoteSwitch> sw = kv.second;
        struct ofl_msg_header* req = BuildPortStatsRequest();
        if (req) {
            SendToSwitch(sw, req, 0);
            ofl_msg_free(req, nullptr);
        }
    }
    Simulator::Schedule(Seconds(10), &ZmqOpenFlowController::TriggerStats, this);
}

} // namespace ns3
