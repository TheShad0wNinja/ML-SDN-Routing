#ifndef ZMQ_OPENFLOW_CONTROLLER_H
#define ZMQ_OPENFLOW_CONTROLLER_H

#include <ns3/application.h>
#include <ns3/ofswitch13-module.h>
#include <zmq.hpp>
#include <memory>

namespace ns3 {

class ZmqOpenFlowController : public OFSwitch13Controller {
public:
    static TypeId GetTypeId();
    ZmqOpenFlowController();
    virtual ~ZmqOpenFlowController();

protected:
    virtual void StartApplication() override;
    virtual void StopApplication() override;
    virtual void HandshakeSuccessful(Ptr<const RemoteSwitch> swtch) override;
    virtual ofl_err HandlePacketIn(
        struct ofl_msg_packet_in* msg,
        Ptr<const RemoteSwitch> swtch,
        uint32_t xid) override;

private:
    std::unique_ptr<zmq::context_t> m_zmqContext;
    std::unique_ptr<zmq::socket_t> m_zmqSocket;
};

} // namespace ns3

#endif