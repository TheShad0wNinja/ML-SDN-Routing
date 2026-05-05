#include "zmq-openflow-controller.h"
#include "ns3/log.h"
#include <arpa/inet.h>
#include <cstring>
#include <vector>

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
    m_zmqContext = std::make_unique<zmq::context_t>(1);
    m_socket = std::make_unique<zmq::socket_t>(*m_zmqContext, zmq::socket_type::req);
    m_socket->set(zmq::sockopt::rcvtimeo, 5000); // 5 s timeout
    m_socket->connect("tcp://127.0.0.1:5555");

    // Startup sync — works regardless of who starts first
    zmq::message_t hello(5);
    memcpy(hello.data(), "HELLO", 5);
    m_socket->send(hello, zmq::send_flags::none);
    zmq::message_t ack;
    if (m_socket->recv(ack, zmq::recv_flags::none)) {
        NS_LOG_INFO("Python controller connected.");
    }

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
    uint64_t dpid = swtch->GetDpId();
    m_switchMap[dpid] = swtch;
    DpctlExecute(dpid, "flow-mod cmd=add,table=0,prio=0 apply:output=ctrl:128");
    DpctlExecute(dpid, "set-config miss=128");
    OFSwitch13Controller::HandshakeSuccessful(swtch);
}

// ------------------------------------------------------------------
//  Core: send to Python, block, unpack reply, send to switches
// ------------------------------------------------------------------
void ZmqOpenFlowController::ExchangeWithPython(uint64_t dpid,
                                               const uint8_t* payload,
                                               size_t len) {
    zmq::message_t req(len + 8);
    uint64_t dpid_nbo = Htonll(dpid);
    memcpy(req.data(), &dpid_nbo, 8);
    if (len > 0) memcpy((uint8_t*)req.data() + 8, payload, len);

    if (!m_socket->send(req, zmq::send_flags::none)) return;

    zmq::message_t reply;
    if (!m_socket->recv(reply, zmq::recv_flags::none)) return;
    if (reply.size() < 2) return;

    uint8_t* p = (uint8_t*)reply.data();
    size_t rem = reply.size();
    uint16_t count = ntohs(*(uint16_t*)p);
    p += 2; rem -= 2;

    for (uint16_t i = 0; i < count && rem >= 12; ++i) {
        uint64_t target_dpid = Ntohll(*(uint64_t*)p);
        p += 8; rem -= 8;
        uint32_t msg_len = ntohl(*(uint32_t*)p);
        p += 4; rem -= 4;
        if (rem < msg_len) break;

        auto it = m_switchMap.find(target_dpid);
        if (it != m_switchMap.end()) {
            struct ofl_msg_header* msg = nullptr;
            uint32_t xid = 0;
            ofl_err err = ofl_msg_unpack(p, msg_len, &msg, &xid, nullptr);
            if (!err && msg) {
                SendToSwitch(it->second, msg, xid);
                ofl_msg_free(msg, nullptr);
            }
        }
        p += msg_len; rem -= msg_len;
    }
}

// ------------------------------------------------------------------
//  OpenFlow event handlers — all just forward to Python
// ------------------------------------------------------------------
ofl_err ZmqOpenFlowController::HandlePacketIn(struct ofl_msg_packet_in* msg,
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

    ofl_msg_free((struct ofl_msg_header*)msg, nullptr);
    return 0;
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
    Simulator::Schedule(Seconds(10), &ZmqOpenFlowController::TriggerStats, this);
}

} // namespace ns3
