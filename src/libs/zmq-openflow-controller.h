#ifndef ZMQ_OPENFLOW_CONTROLLER_H
#define ZMQ_OPENFLOW_CONTROLLER_H

#include "ns3/ofswitch13-controller.h"
#include "ns3/simulator.h"
#include "ns3/nstime.h"
#include "topology.h"
#include <zmq.hpp>
#include <memory>
#include <map>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <utility>

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

    ofl_err HandlePortStatus(struct ofl_msg_port_status* msg,
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
    // Controller-side topology and learned state
    Topology m_topology;
    // MAC (48-bit) -> (dpid, port)
    std::unordered_map<uint64_t, std::pair<uint64_t, uint32_t>> m_macToLoc;
    // switch dpid -> set of known link ports
    std::unordered_map<uint64_t, std::unordered_set<uint32_t>> m_switchPorts;
    // per-switch per-port stats (placeholder store): dpid -> (port -> value)
    std::unordered_map<uint64_t, std::unordered_map<uint32_t, uint64_t>> m_portStats;
};

} // namespace ns3

#endif // ZMQ_OPENFLOW_CONTROLLER_H
