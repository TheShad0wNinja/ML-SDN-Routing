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
    void DoDispose() override;

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

    ofl_err HandleEchoReply(struct ofl_msg_echo* msg,
                            Ptr<const RemoteSwitch> swtch,
                            uint32_t xid) override;

private:
    void SendPacketOut(Ptr<const RemoteSwitch> swtch,
                       uint32_t inPort,
                       uint32_t bufferId,
                       const uint8_t* data,
                       size_t dataLen,
                       uint32_t outPort);
    void TriggerLldp();
    void TriggerStats();
    void WriteStateToJson();

    void HandleLldpPacket(uint64_t dpid, uint32_t inPort,
                          const uint8_t* data, size_t len);
    void HandleArpPacket(const uint8_t* data, size_t len);
    void ForwardPacket(Ptr<const RemoteSwitch> swtch, uint32_t inPort,
                       struct ofl_msg_packet_in* msg,
                       uint64_t srcMac, uint64_t dstMac);
    void InstallFlow(uint64_t dpid, uint64_t dstMac, uint32_t outPort);
    static std::string FormatIp(uint32_t ip);

    void SendSingleLldp(Ptr<const RemoteSwitch> swtch, uint64_t dpid, uint32_t port);
    void TriggerEcho();

    static constexpr uint32_t kMaxLldpProbe   = 8;
    static constexpr double   kEchoIntervalSec = 60;
    static constexpr uint32_t kEchoMaxMissed   = 3;

    std::unique_ptr<zmq::context_t> m_zmqContext;
    std::unique_ptr<zmq::socket_t>  m_socket;
    std::map<uint64_t, Ptr<const RemoteSwitch>> m_switchMap;
    Topology m_topology;
    // MAC (48-bit) -> (dpid, port)
    std::unordered_map<uint64_t, std::pair<uint64_t, uint32_t>> m_macToLoc;
    // switch dpid -> set of known ports
    std::unordered_map<uint64_t, std::unordered_set<uint32_t>> m_switchPorts;
    // per-switch per-port stats placeholder
    std::unordered_map<uint64_t, std::unordered_map<uint32_t, uint64_t>> m_portStats;
    // MAC -> IPv4 (host-byte-order)
    std::unordered_map<uint64_t, uint32_t> m_hostIpMap;
    // LLDP send timestamps: dpid -> port -> nanoseconds
    std::unordered_map<uint64_t, std::unordered_map<uint32_t, uint64_t>> m_lldpSendNs;
    // Echo timestamps: dpid -> nanoseconds of last outgoing echo request
    std::unordered_map<uint64_t, uint64_t> m_echoSendNs;
    // Echo RTTs: dpid -> last measured control-plane RTT in nanoseconds
    std::unordered_map<uint64_t, uint64_t> m_echoRttNs;
    // Liveness: dpid -> consecutive echo requests without a reply
    std::unordered_map<uint64_t, uint32_t> m_echoMissCount;
};

} // namespace ns3

#endif // ZMQ_OPENFLOW_CONTROLLER_H
