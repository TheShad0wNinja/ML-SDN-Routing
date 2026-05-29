#ifndef ZMQ_OPENFLOW_CONTROLLER_H
#define ZMQ_OPENFLOW_CONTROLLER_H

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <zmq.hpp>

#include "ns3/nstime.h"
#include "ns3/ofswitch13-controller.h"
#include "ns3/simulator.h"
#include "topology.h"

namespace ns3 {

struct PortStatsEntry {
  uint64_t rx_packets = 0, tx_packets = 0;
  uint64_t rx_bytes = 0, tx_bytes = 0;
  uint64_t rx_dropped = 0, tx_dropped = 0;
  uint64_t rx_errors = 0, tx_errors = 0;
  uint32_t duration_sec = 0;

  // For rate calculation (not serialised to JSON)
  uint64_t prev_rx_bytes = 0, prev_tx_bytes = 0;
  uint64_t prev_rx_packets = 0, prev_tx_packets = 0;
  uint64_t prev_rx_dropped = 0, prev_tx_dropped = 0;
  double prev_time_s = 0.0;

  // Derived rates in bits/s — updated on each PORT_STATS reply
  double rx_rate_bps = 0.0, tx_rate_bps = 0.0;

  // Empirical per-port drop byte-rates (delta dropped packets × avg pkt size ×
  // 8 / dt). Computed from OF1.3-standard tx_dropped / rx_dropped counters
  double rx_drop_rate_bps = 0.0, tx_drop_rate_bps = 0.0;

  // OFPMP_QUEUE aggregated counters (port-level sum across queue_ids).
  // q_tx_errors = packets dropped by the queue due to overrun.
  uint64_t q_tx_errors = 0;
  uint64_t prev_q_tx_errors = 0;
  double q_tx_error_rate_pps = 0.0;

  // Link speed captured from PORT_DESC (kbps)
  uint32_t speed_kbps = 0;
};

struct SwitchObservation {
  double d_ms = 0;   // Data Plan Queueing Delay max over ports of M/M/1
                     // wait-time proxy: base_delay·u/(1-u)
  double L_bps = 0;  // sum of packet loss bps for all ports (rx_drbp_rate_bps +
                     // tx_drop_rate_bps)
  double residual_energy_j = -1;  // joules remaining; -1 = not tracked
};

struct HostAnnotation {
  std::string name = "";
  std::string node_type = "host";
};

struct SwitchEnergyModel {
  double initial_energy_j = -1;     // -1 = not tracked
  double energy_per_byte_j = 1e-9;  // joules consumed per byte forwarded
};

struct MlConfig {
  bool enabled = false;  // If ML model is enabled or not
  double interval_s = 1.0;  // ML ticks for updating route weights

  // Action-scale taper: |ΔW| starts at action_scale_start, tapers linearly
  // over taper_ticks to action_scale. Big swings early, fine-tune late.
  double action_scale_start = 0.30;  // initial |ΔW| during taper
  double action_scale = 0.10;        // final |ΔW| as fraction of base cost
  uint32_t taper_ticks = 400;        // ticks over which to taper

  // Reward weights //
  // Positive Goals
  double reward_alpha = 1.0;  // delay quality (higher = lower latency reward)
  double reward_beta = 2.0;   // loss quality (higher = lower loss reward)
  // Penalties
  double reward_gamma = 1.5;  // power-consumption penalty
  double reward_delta = 1.0;  // link utilization penalty (mean+peak)
  double reward_zeta =
      0.5;                  // active-switch footprint penalty (lower hop count)
  double reward_eta = 1.5;  // Reserve-aware penalty (ignore nearly dead nodes)
  double reward_theta =
      1.0;  // residual-energy variance penalty (encourage round-robin routing)
  double reward_kappa =
      1.0;  // route-churn penalty (L1 of action-vector delta between ticks)

  // Normalization references
  double delay_ref_ms = 200.0;   // ms; baseline target
  double loss_ref_bps = 1.0e6;   // bits/s; tolerable drop budget
  double power_ref_w = 100.0;    // watts; baseline aggregate power

  // ML Priority preset for reward weights
  enum MlPriority { BALANCED, THROUGHPUT, ENERGY, CUSTOM };
  MlPriority priority_preset = BALANCED;

  // Exploration control
  bool explore = true;                     // Gaussian action noise enabled
  bool learn = true;                       // Gradient updates / train_step enabled
  uint32_t checkpoint_every_n_ticks = 60;  // Python-side checkpoint cadence
  bool resume = true;                      // Python loads checkpoint if present
  uint32_t seed = 12345;                   // shared seed for Python RNG
  std::string endpoint = "tcp://127.0.0.1:5555";
  // Pin the Python agent's exploration noise sigma to a fixed value on
  // construction (skips both the default 0.3 init and the checkpoint's
  // saved sigma). Negative = use the agent's default decay schedule.
  // Lets a cooldown / fine-tune phase run with low noise on top of an
  // already-trained model without touching agent code.
  double noise_sigma_init = -1.0;

  // Controller id for federated learning controller artifacts
  uint32_t controller_id = 0;
};

class ZmqOpenFlowController : public OFSwitch13Controller {
 public:
  static TypeId GetTypeId();
  ZmqOpenFlowController();
  ~ZmqOpenFlowController() override;
  void DoDispose() override;

  // Called from scenario before Simulator::Run() to annotate hosts (name +
  // type)
  void SetHostAnnotation(uint64_t mac, const HostAnnotation& ann);

  // Stores intra-domain host info
  struct HostInfo {
    uint64_t mac;
    uint64_t dpid;
    uint32_t port;
  };
  // Install all flow mods required to route all intra-domain hosts
  void PreInstallAllPaths(const std::vector<HostInfo>& hosts);

  // Stores inter-domain host info
  struct ExternalHostRoute {
    uint64_t dst_mac;
    uint64_t border_dpid;
    uint32_t border_out_port;
  };
  // Install all flows modes required for external hosts border switchs
  // to route all inter-domain hosts
  void InstallExternalHostRoutes(const std::vector<ExternalHostRoute>& routes);

  // Configure forwarding-energy model for a switch (by DPID)
  void SetSwitchEnergyModel(uint64_t dpid, double initial_j, double per_byte_j);

  // Read back energy state for reporting. Returns -1 if dpid not configured.
  double GetSwitchInitialEnergyJ(uint64_t dpid) const;
  double GetSwitchResidualEnergyJ(uint64_t dpid) const;

  // Retrieve the mean hop-count for all routes in the topology for routing
  // summary
  double GetAverageHopCount() const { return m_topology.AverageHopCount(); }

  // Override the stats polling interval (seconds); default is 60 s
  void SetStatsInterval(double seconds);

  // Enable online FDRL local agent. Default-constructed MlConfig keeps it off.
  void SetMlConfig(const MlConfig& cfg);

 protected:
  void StartApplication() override;
  void StopApplication() override;
  void HandshakeSuccessful(Ptr<const RemoteSwitch> swtch) override;

  ofl_err HandlePacketIn(struct ofl_msg_packet_in* msg,
                         Ptr<const RemoteSwitch> swtch, uint32_t xid) override;

  ofl_err HandlePortStatus(struct ofl_msg_port_status* msg,
                           Ptr<const RemoteSwitch> swtch,
                           uint32_t xid) override;

  ofl_err HandleMultipartReply(struct ofl_msg_multipart_reply_header* msg,
                               Ptr<const RemoteSwitch> swtch,
                               uint32_t xid) override;

  ofl_err HandleEchoReply(struct ofl_msg_echo* msg,
                          Ptr<const RemoteSwitch> swtch, uint32_t xid) override;

 private:
  void SendPacketOut(Ptr<const RemoteSwitch> swtch, uint32_t inPort,
                     uint32_t bufferId, const uint8_t* data, size_t dataLen,
                     uint32_t outPort);
  void SendPacketOutGroup(Ptr<const RemoteSwitch> swtch, uint32_t inPort,
                          uint32_t bufferId, const uint8_t* data,
                          size_t dataLen, uint32_t groupId);
  void ForwardPacket(Ptr<const RemoteSwitch> swtch, uint32_t inPort,
                     struct ofl_msg_packet_in* msg, uint64_t srcMac,
                     uint64_t dstMac);
  void SendSingleLldp(Ptr<const RemoteSwitch> swtch, uint64_t dpid,
                      uint32_t port);
  void FloodViaST(Ptr<const RemoteSwitch> inSwtch, uint32_t inPort,
                  uint32_t bufferId, const uint8_t* data, size_t dataLen);

  void TriggerLldp();
  void TriggerStats();
  void TriggerEcho();

  void HandleLldpPacket(uint64_t dpid, uint32_t inPort, const uint8_t* data,
                        size_t len);
  void HandleArpPacket(const uint8_t* data, size_t len);
  void HandlePortDescReply(struct ofl_msg_multipart_reply_port_desc* reply,
                           uint64_t dpid);
  void HandlePortStatsReply(struct ofl_msg_multipart_reply_port* reply,
                            uint64_t dpid);
  void HandleQueueStatsReply(struct ofl_msg_multipart_reply_queue* reply,
                             uint64_t dpid);

  void InstallFlow(uint64_t dpid, uint64_t dstMac, uint32_t outPort);
  void InstallOrUpdateFloodGroup(uint64_t dpid);

  void RecomputeAllRoutes();
  void RebuildSpanningTree();

  // Formats an integer ip into dotted notation
  static std::string FormatIp(uint32_t ip);

  void ComputeSwitchObservations(uint64_t dpid);

  // Writes the final topology state to a json
  void WriteStateToJson();

  // ML loop helpers
  void MlOpenSocket();
  void MlSendHello();
  void MlTick();
  std::string BuildMlStatePayload();

  double ComputeMlReward();
  // Calculate standard deviation of residual_energy_frac across energy-tracked
  // switches. Used by the reward (balance term) and the state payload.
  double ComputeResidualEnergyStddev() const;
  // Returns action_scale once tapered.
  double CurrentActionScale() const;
  // Applies the model new link weight deltas
  void ApplyDeltaCosts(const std::vector<double>& deltas);

  // Constants
  static constexpr uint32_t kMaxLldpProbe =
      8;  // Max lldp framess sent per cycle per switch
  static constexpr double kEchoIntervalSec =
      60;  // How often to ping switch for liveness
  static constexpr uint32_t kEchoMaxMissed =
      3;  // Concecutive missed echo repliess before switch dead
  static constexpr uint32_t kFloodGroupId = 1;  // Openflow Group ID for floodss

  // Control Flags
  double m_statsIntervalS = 30.0; // Interval for statss
  bool m_congestionDirty =
      false;  // Used to recompute routes after factor threshold exceeded

  // Sdn Topology
  std::map<uint64_t, Ptr<const RemoteSwitch>>
      m_switchMap;  // Maps dpid to switches
  std::unordered_map<uint64_t, std::unordered_set<uint32_t>>
      m_switchPorts;  // Map dpit to known ports
  Topology m_topology;
  std::unordered_map<uint64_t, std::pair<uint64_t, uint32_t>>
      m_macToLoc;  // Map mac to (dpid, port)

  // Flow Tracking
  std::unordered_map<uint64_t, std::unordered_map<uint64_t, uint32_t>>
      m_installedFlows;  // Map dpid -> destMac -> egress port

  // Port/Link Monitoring
  std::unordered_map<uint64_t, std::unordered_map<uint32_t, PortStatsEntry>>
      m_portStats;  // Map switch per port stats
  std::unordered_map<uint64_t, std::unordered_map<uint32_t, uint32_t>>
      m_portSpeeds;  // Map switch per port speed [PORT_DESC] (kbpss)
  std::unordered_map<uint64_t, SwitchObservation>
      m_switchObs;  // Map switch observations

  // Address Resolution
  std::unordered_map<uint64_t, uint32_t> m_hostIpMap;  // MAC -> IPv4
  std::unordered_map<uint32_t, uint64_t> m_ipToMac;    // IPv4 -> MAC

  // LLDP Discovery
  std::unordered_map<uint64_t, std::unordered_map<uint32_t, uint64_t>>
      m_lldpSendNs;  // LLDP send timestamps: dpid -> port -> nanoseconds

  // Control Plan Liveness
  std::unordered_map<uint64_t, uint64_t>
      m_echoSendNs;  // Echo timestamps: dpid -> nanoseconds of last outgoing
  std::unordered_map<uint64_t, uint64_t>
      m_echoRttNs;  // Echo RTTs: dpid -> last measured control-plane RTT ns
  std::unordered_map<uint64_t, uint32_t>
      m_echoMissCount;  // Liveness: dpid -> consecutive echo requests no rep

  // Spanning Tree
  std::unordered_map<uint64_t, std::unordered_set<uint32_t>> 
    m_spanningTree; // dpid -> spanning tree ports
  std::unordered_set<uint64_t> m_floodGroupInstalled; // Dpids with ST installed

  // Energy Model
  std::unordered_map<uint64_t, SwitchEnergyModel> m_switchEnergyModel; // Dpid -> energy model
  std::unordered_map<uint64_t, double> m_switchResidualEnergy; // Dpid -> Remaining energy

  // Scenario Metadata
  std::unordered_map<uint64_t, HostAnnotation> m_hostAnnotations;

  // ML Agent //
  // Ml Config
  MlConfig m_ml; // ML Config
  // Ml Zmq Bridge
  std::unique_ptr<zmq::context_t> m_mlCtx;
  std::unique_ptr<zmq::socket_t> m_mlSock;
  uint64_t m_mlTick = 0; // Current ML Tick
  // Canonical link ordering — frozen at first MlTick so the action vector index
  // → link mapping is stable across ticks.
  std::vector<std::pair<uint64_t, uint64_t>> m_mlLinkOrder;
  // Canonical node ordering — list of dpids frozen at first MlTick. Defines the
  // dpid → sequential index mapping the Python GNN uses to build edge_index.
  std::vector<uint64_t> m_mlNodeOrder;
  // Snapshot of m_switchObs from the *previous* tick, used by ComputeMlReward.
  std::unordered_map<uint64_t, SwitchObservation> m_mlPrevObs;
  bool m_mlHavePrevObs = false;
  double m_mlPrevReward = 0.0;
  // L1(a_t - a_{t-1}) / max_swing in [0, 1]. Updated in ApplyDeltaCosts;
  // consumed one tick later by ComputeMlReward as the churn penalty.
  double m_lastChurnNorm = 0.0;
};

}  // namespace ns3

#endif  // ZMQ_OPENFLOW_CONTROLLER_H
