#include "zmq-openflow-controller.h"
#include "ns3/log.h"
#include <cstring>

namespace ns3 {

NS_LOG_COMPONENT_DEFINE("ZmqOpenFlowController");
NS_OBJECT_ENSURE_REGISTERED(ZmqOpenFlowController);

TypeId ZmqOpenFlowController::GetTypeId() {
    static TypeId tid = TypeId("ns3::ZmqOpenFlowController")
        .SetParent<OFSwitch13Controller>()
        .SetGroupName("OpenFlow13")
        .AddConstructor<ZmqOpenFlowController>();
    return tid;
}

ZmqOpenFlowController::ZmqOpenFlowController() {
}

ZmqOpenFlowController::~ZmqOpenFlowController() {
}

void ZmqOpenFlowController::StartApplication() {
    NS_LOG_INFO("Starting ZMQ OpenFlow Controller...");
    m_zmqContext = std::make_unique<zmq::context_t>(1);
    m_zmqSocket = std::make_unique<zmq::socket_t>(*m_zmqContext, zmq::socket_type::req);
    m_zmqSocket->set(zmq::sockopt::rcvtimeo, 5000); // 5 sec timeout
    m_zmqSocket->connect("tcp://127.0.0.1:5555");
    
    OFSwitch13Controller::StartApplication();
}

void ZmqOpenFlowController::StopApplication() {
    if (m_zmqSocket) {
        m_zmqSocket->close();
        m_zmqSocket.reset();
    }
    if (m_zmqContext) {
        m_zmqContext->close();
        m_zmqContext.reset();
    }
    OFSwitch13Controller::StopApplication();
}

void ZmqOpenFlowController::HandshakeSuccessful(Ptr<const RemoteSwitch> swtch) {
    NS_LOG_INFO("Handshake successful with switch DPID " << swtch->GetDpId());
    
    // Add default Table-Miss rule to direct to controller
    uint64_t swDpId = swtch->GetDpId();
    DpctlExecute(swDpId, "flow-mod cmd=add,table=0,prio=0 apply:output=ctrl:128");
    DpctlExecute(swDpId, "set-config miss=128");

    // Do NOT fabricate OpenFlow messages here.
    // The switch handshake is handled internally by OFSwitch13.
    // Python learns about traffic via PacketIn messages.
    OFSwitch13Controller::HandshakeSuccessful(swtch);
}

ofl_err ZmqOpenFlowController::HandlePacketIn(
    struct ofl_msg_packet_in* msg, 
    Ptr<const RemoteSwitch> swtch, 
    uint32_t xid) 
{
    NS_LOG_LOGIC("PacketIn from DPID " << swtch->GetDpId() 
                 << ", xid=" << xid << ", sending to Python");
    
    // Serialize to OpenFlow 1.3 wire format.
    // NOTE: If compilation fails here, check your BOFUSS headers.
    // Some BOFUSS versions use: ofl_msg_pack(msg, &tx_buf, &tx_len, nullptr)
    uint8_t* tx_buf = nullptr;
    size_t tx_len = 0;
    ofl_err pack_err = ofl_msg_pack(
        (struct ofl_msg_header*)msg, xid, &tx_buf, &tx_len, nullptr);
    
    if (pack_err) {
        NS_LOG_ERROR("ofl_msg_pack failed, error=" << pack_err);
        ofl_msg_free((struct ofl_msg_header*)msg, nullptr);
        return 0;
    }
    
    zmq::message_t req(tx_buf, tx_len);
    auto sendRes = m_zmqSocket->send(req, zmq::send_flags::none);
    free(tx_buf);
    
    if (!sendRes) {
        NS_LOG_ERROR("ZMQ send failed");
        ofl_msg_free((struct ofl_msg_header*)msg, nullptr);
        return 0;
    }
    
    zmq::message_t rep;
    auto recvResult = m_zmqSocket->recv(rep, zmq::recv_flags::none);
    if (!recvResult) {
        NS_LOG_WARN("ZMQ timeout waiting for Python (DPID " << swtch->GetDpId() << ")");
        ofl_msg_free((struct ofl_msg_header*)msg, nullptr);
        return 0;
    }
    
    if (rep.size() > 0) {
        struct ofl_msg_header* rep_msg = nullptr;
        uint32_t rep_xid = 0;
        ofl_err unpack_err = ofl_msg_unpack(
            (uint8_t*)rep.data(), rep.size(), &rep_msg, &rep_xid, nullptr);
            
        if (!unpack_err && rep_msg) {
            NS_LOG_LOGIC("Forwarding " << rep.size() 
                         << " bytes from Python to DPID " << swtch->GetDpId());
            SendToSwitch(swtch, rep_msg, rep_xid);
            ofl_msg_free(rep_msg, nullptr);
        } else {
            NS_LOG_ERROR("ofl_msg_unpack failed, error=" << unpack_err);
        }
    }
    
    // BOFUSS requires the handler to free the inbound message.
    ofl_msg_free((struct ofl_msg_header*)msg, nullptr);
    return 0;
}

} // namespace ns3