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
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace ns3 {

// Full counters + derived rates for one OpenFlow port
struct PortStatsEntry {
    uint64_t rx_packets = 0, tx_packets = 0;
    uint64_t rx_bytes   = 0, tx_bytes   = 0;
    uint64_t rx_dropped = 0, tx_dropped = 0;
    uint64_t rx_errors  = 0, tx_errors  = 0;
    uint32_t duration_sec = 0;
    // For rate calculation (not serialised to JSON)
    uint64_t prev_rx_bytes = 0, prev_tx_bytes = 0;
    double   prev_time_s   = 0.0;
    // Derived rates in bits/s — updated on each PORT_STATS reply
    double rx_rate_bps = 0.0, tx_rate_bps = 0.0;
    // Link speed captured from PORT_DESC (kbps)
    uint32_t speed_kbps = 0;
};

// Per-switch DDPG observation vector (M/M/1 queue model approximations)
struct SwitchObservation {
    double lambda_bps        = 0;  // aggregate arrival rate on host-facing ports (bits/s)
    double mu_max_bps        = 0;  // sum of host-port link capacities (bits/s)
    double rho               = 0;  // traffic intensity = lambda / mu_max
    double K                 = 64; // system capacity (queue slots, 64-packet FIFO default)
    double N                 = 0;  // expected queue occupancy = rho/(1-rho) [M/M/1]
    double d_ms              = 0;  // expected delay via Little's law (ms)
    double L_bps             = 0;  // loss rate = sum of dropped-bytes rate (bits/s)
    double rbw_bps           = 0;  // residual bandwidth = mu_max - lambda
    double residual_energy_j = -1; // joules remaining; -1 = not tracked
};

// Host display annotation (name + node_type for topology viewer grouping)
struct HostAnnotation {
    std::string name      = "";
    std::string node_type = "host";
};

// Per-switch forwarding energy model set by the scenario
struct SwitchEnergyModel {
    double initial_energy_j  = -1;   // -1 = not tracked
    double energy_per_byte_j = 1e-9; // joules consumed per byte forwarded
};

class ZmqOpenFlowController : public OFSwitch13Controller {
public:
    static TypeId GetTypeId();
    ZmqOpenFlowController();
    ~ZmqOpenFlowController() override;
    void DoDispose() override;

    // Called from scenario before Simulator::Run() to annotate hosts (name + type only)
    void SetHostAnnotation(uint64_t mac, const HostAnnotation& ann);
    // Configure forwarding-energy model for a switch (by DPID)
    void SetSwitchEnergyModel(uint64_t dpid, double initial_j, double per_byte_j);
    // Override the stats polling interval (seconds); default is 60 s
    void SetStatsInterval(double seconds);

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
    void RebuildSpanningTree();
    void HandlePortDescReply(struct ofl_msg_multipart_reply_port_desc* reply, uint64_t dpid);
    void HandlePortStatsReply(struct ofl_msg_multipart_reply_port* reply, uint64_t dpid);
    void ComputeSwitchObservations(uint64_t dpid);
    void FloodViaST(Ptr<const RemoteSwitch> inSwtch, uint32_t inPort,
                    uint32_t bufferId, const uint8_t* data, size_t dataLen);

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
    // per-switch per-port full stats (replaces old uint64 placeholder)
    std::unordered_map<uint64_t, std::unordered_map<uint32_t, PortStatsEntry>> m_portStats;
    // per-switch per-port link speed from PORT_DESC (kbps)
    std::unordered_map<uint64_t, std::unordered_map<uint32_t, uint32_t>> m_portSpeeds;
    // per-switch DDPG observation vectors
    std::unordered_map<uint64_t, SwitchObservation> m_switchObs;
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
    // IP (host byte order) -> MAC reverse map; updated alongside m_hostIpMap
    std::unordered_map<uint32_t, uint64_t> m_ipToMac;
    // Spanning tree: dpid -> set of inter-switch ports on the spanning tree
    std::unordered_map<uint64_t, std::unordered_set<uint32_t>> m_spanningTree;
    // Host display annotations set by scenario
    std::unordered_map<uint64_t, HostAnnotation> m_hostAnnotations;
    // Per-switch energy models and residual energy
    std::unordered_map<uint64_t, SwitchEnergyModel> m_switchEnergyModel;
    std::unordered_map<uint64_t, double>             m_switchResidualEnergy;

    double m_statsIntervalS = 60.0;
};

} // namespace ns3

#endif // ZMQ_OPENFLOW_CONTROLLER_H
