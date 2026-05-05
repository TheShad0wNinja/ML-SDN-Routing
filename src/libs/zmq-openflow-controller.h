#ifndef ZMQ_OPENFLOW_CONTROLLER_H
#define ZMQ_OPENFLOW_CONTROLLER_H

#include "ns3/ofswitch13-controller.h"
#include "ns3/simulator.h"
#include "ns3/nstime.h"
#include <zmq.hpp>
#include <memory>
#include <map>
#include <cstdint>

namespace ns3 {

class ZmqOpenFlowController : public OFSwitch13Controller {
public:
    static TypeId GetTypeId();
    ZmqOpenFlowController();
    ~ZmqOpenFlowController() override;

protected:
    void StartApplication() override;
    void StopApplication() override;
    void HandshakeSuccessful(Ptr<const RemoteSwitch> swtch) override;

    ofl_err HandlePacketIn(struct ofl_msg_packet_in* msg,
                           Ptr<const RemoteSwitch> swtch,
                           uint32_t xid) override;

    ofl_err HandleMultipartReply(struct ofl_msg_multipart_reply_header* msg,
                                 Ptr<const RemoteSwitch> swtch,
                                 uint32_t xid) override;

private:
    void ExchangeWithPython(uint64_t dpid,
                            const uint8_t* payload,
                            size_t len);
    void TriggerLldp();
    void TriggerStats();

    std::unique_ptr<zmq::context_t> m_zmqContext;
    std::unique_ptr<zmq::socket_t>  m_socket; // REQ
    std::map<uint64_t, Ptr<const RemoteSwitch>> m_switchMap;
};

} // namespace ns3

#endif // ZMQ_OPENFLOW_CONTROLLER_H
