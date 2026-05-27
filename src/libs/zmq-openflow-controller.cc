#include "zmq-openflow-controller.h"

#include <arpa/inet.h>
#include <sys/stat.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "ns3/log.h"
#include "ns3/simulator.h"
#include "openflow_builders.h"
#include <arpa/inet.h>
#include "ns3/ofswitch13-module.h"

namespace ns3 {

NS_LOG_COMPONENT_DEFINE("ZmqOpenFlowController");

// Map (delay, capacity) → static link cost. Low-delay, high-capacity links
// are cheap; the floors keep early-LLDP links (capacity unknown until
// PORT_STATS arrives) on the same order of magnitude as discovered links.
static double ComputeBaseCost(double delayMs, double capBps) {
  double d = std::clamp(delayMs, 1.0, 1000.0);
  double cap_gbps = std::max(capBps, 10e6) / 1e9;  // 10 Mbps floor
  return d / cap_gbps;
}

TypeId ZmqOpenFlowController::GetTypeId() {
  static TypeId tid = TypeId("ns3::ZmqOpenFlowController")
                          .SetParent<OFSwitch13Controller>()
                          .SetGroupName("OpenFlow13")
                          .AddConstructor<ZmqOpenFlowController>();
  return tid;
}

ZmqOpenFlowController::ZmqOpenFlowController() {}

ZmqOpenFlowController::~ZmqOpenFlowController() {}

void ZmqOpenFlowController::DoDispose() {
  WriteStateToJson();
  m_switchMap.clear();
  m_macToLoc.clear();
  m_switchPorts.clear();
  m_portStats.clear();
  m_switchObs.clear();
  m_hostAnnotations.clear();
  m_switchEnergyModel.clear();
  m_switchResidualEnergy.clear();
  m_hostIpMap.clear();
  m_ipToMac.clear();
  m_spanningTree.clear();
  m_lldpSendNs.clear();
  m_topology = Topology();

  if (m_mlSock) {
    try {
      m_mlSock->close();
    } catch (...) {
    }
  }
  if (m_mlCtx) {
    try {
      m_mlCtx->close();
    } catch (...) {
    }
  }

  OFSwitch13Controller::DoDispose();
}

void ZmqOpenFlowController::StartApplication() {
  // Start LLDP early so topology is known before pings at t=2s
  Simulator::Schedule(Seconds(0.5), &ZmqOpenFlowController::TriggerLldp, this);
  Simulator::Schedule(Seconds(1.0), &ZmqOpenFlowController::TriggerEcho, this);
  Simulator::Schedule(Seconds(m_statsIntervalS), &ZmqOpenFlowController::TriggerStats, this);

  OFSwitch13Controller::StartApplication();
}

void ZmqOpenFlowController::StopApplication() {
  OFSwitch13Controller::StopApplication();
}

void ZmqOpenFlowController::SetHostAnnotation(uint64_t mac,
                                               const HostAnnotation& ann) {
  m_hostAnnotations[mac] = ann;
}

void ZmqOpenFlowController::SetSwitchEnergyModel(uint64_t dpid,
                                                   double initial_j,
                                                   double per_byte_j) {
  m_switchEnergyModel[dpid] = {initial_j, per_byte_j};
  if (initial_j >= 0)
    m_switchResidualEnergy[dpid] = initial_j;
}

double ZmqOpenFlowController::GetSwitchInitialEnergyJ(uint64_t dpid) const {
  auto it = m_switchEnergyModel.find(dpid);
  if (it == m_switchEnergyModel.end()) return -1.0;
  return it->second.initial_energy_j;
}

double ZmqOpenFlowController::GetSwitchResidualEnergyJ(uint64_t dpid) const {
  auto it = m_switchResidualEnergy.find(dpid);
  if (it == m_switchResidualEnergy.end()) return -1.0;
  return it->second;
}

void ZmqOpenFlowController::SetStatsInterval(double seconds) {
  m_statsIntervalS = seconds;
}

void ZmqOpenFlowController::SetMlConfig(const MlConfig& cfg) {
  m_ml = cfg;
  if (!m_ml.enabled) {
    return;
  }

  // Apply priority preset unless it' scustom
  switch(cfg.priority_preset){
    case MlConfig::MlPriority::BALANCED: {
      m_ml.reward_alpha = 1.0;
      m_ml.reward_beta = 2.0;
      m_ml.reward_gamma = 1.5;
      m_ml.reward_delta = 1.0;
      m_ml.reward_zeta = 0.5;
      m_ml.reward_eta = 1.5;
      m_ml.reward_theta = 1.0;
    }
    case MlConfig::MlPriority::THROUGHPUT: {
      m_ml.reward_alpha = 2.5;
      m_ml.reward_beta = 3.0;
      m_ml.reward_gamma = 0.5;
      m_ml.reward_delta = 1.0;
      m_ml.reward_zeta = 0.2;
      m_ml.reward_eta = 0.8;
      m_ml.reward_theta = 0.5;
    }
    case MlConfig::MlPriority::ENERGY: {
      m_ml.reward_alpha = 1.0;
      m_ml.reward_beta = 2.0;  // pos bound = 3.0
      m_ml.reward_gamma = 2.0;
      m_ml.reward_delta = 0.5;
      m_ml.reward_zeta = 1.0;
      m_ml.reward_eta = 2.5;
      m_ml.reward_theta = 1.5;  // neg bound = 7.5
    }
    default: break;
  }

  NS_LOG_INFO("[ML] preset=" << m_ml.priority_preset
              << " α=" << m_ml.reward_alpha << " β=" << m_ml.reward_beta
              << " γ=" << m_ml.reward_gamma << " δ=" << m_ml.reward_delta
              << " ζ=" << m_ml.reward_zeta  << " η=" << m_ml.reward_eta
              << " θ=" << m_ml.reward_theta);

  // Make sure stats roll fast enough that the agent gets fresh observations
  // every tick. Without this, the default 60 s interval would starve MlTick.
  if (m_statsIntervalS > m_ml.interval_s) {
    m_statsIntervalS = m_ml.interval_s;
  }

  MlOpenSocket();
  // Schedule first tick a bit after LLDP discovery (default trigger at 5 s)
  // so the link order frozen at first tick reflects the real topology.
  Simulator::Schedule(Seconds(std::max(6.0, m_ml.interval_s * 2)),
                      &ZmqOpenFlowController::MlTick, this);
}

// Handles initial connection with an OF Switch
void ZmqOpenFlowController::HandshakeSuccessful(Ptr<const RemoteSwitch> swtch) {
  NS_LOG_FUNCTION(this << swtch);

  uint64_t swDpId = swtch->GetDpId();
  m_switchMap[swDpId] = swtch;
  m_switchPorts[swDpId] = std::unordered_set<uint32_t>();

  // Adds default table-miss behaviour
  DpctlExecute(swDpId, "flow-mod cmd=add,table=0,prio=0 apply:output=ctrl:128");
  DpctlExecute(swDpId, "set-config miss=128");

  // ARP → controller. Sits above the broadcast→flood-group rule so the
  // controller can answer known targets directly (proxy ARP) and decide
  // when to flood unknown ones.
  DpctlExecute(swDpId,
      "flow-mod cmd=add,table=0,prio=20 eth_type=0x0806 apply:output=ctrl:128");

  // Request port info from connected switch
  struct ofl_msg_header* pdReq = BuildPortDescRequest();
  if (pdReq) {
    SendToSwitch(swtch, pdReq, 0);
    ofl_msg_free(pdReq, nullptr);
  }
}

// ------------------------------------------------------------------
//  Helper: build and send a single-action OFPT_PACKET_OUT
// ------------------------------------------------------------------
void ZmqOpenFlowController::SendPacketOut(Ptr<const RemoteSwitch> swtch,
                                          uint32_t inPort, uint32_t bufferId,
                                          const uint8_t* data, size_t dataLen,
                                          uint32_t outPort) {
  struct ofl_msg_packet_out* po =
      (struct ofl_msg_packet_out*)malloc(sizeof(*po));
  memset(po, 0, sizeof(*po));
  po->header.type = OFPT_PACKET_OUT;
  po->buffer_id = bufferId;
  po->in_port = inPort;
  if (bufferId == OFP_NO_BUFFER && data && dataLen > 0) {
    po->data_length = dataLen;
    po->data = (uint8_t*)malloc(dataLen);
    memcpy(po->data, data, dataLen);
  }
  struct ofl_action_output* a = (struct ofl_action_output*)malloc(sizeof(*a));
  a->header.type = OFPAT_OUTPUT;
  a->header.len = sizeof(*a);
  a->port = outPort;
  a->max_len = 0;
  po->actions_num = 1;
  po->actions =
      (struct ofl_action_header**)malloc(sizeof(struct ofl_action_header*));
  po->actions[0] = (struct ofl_action_header*)a;
  SendToSwitch(swtch, (struct ofl_msg_header*)po, 0);
  free(po->actions);
  free(a);
  if (po->data) free(po->data);
  free(po);
}

// ------------------------------------------------------------------
//  Helper: build and send a PACKET_OUT with a single group action
// ------------------------------------------------------------------
void ZmqOpenFlowController::SendPacketOutGroup(Ptr<const RemoteSwitch> swtch,
                                               uint32_t inPort, uint32_t bufferId,
                                               const uint8_t* data, size_t dataLen,
                                               uint32_t groupId) {
  struct ofl_msg_packet_out* po =
      (struct ofl_msg_packet_out*)malloc(sizeof(*po));
  memset(po, 0, sizeof(*po));
  po->header.type = OFPT_PACKET_OUT;
  po->buffer_id = bufferId;
  po->in_port = inPort;
  if (bufferId == OFP_NO_BUFFER && data && dataLen > 0) {
    po->data_length = dataLen;
    po->data = (uint8_t*)malloc(dataLen);
    memcpy(po->data, data, dataLen);
  }
  struct ofl_action_group* a = (struct ofl_action_group*)malloc(sizeof(*a));
  a->header.type = OFPAT_GROUP;
  a->header.len = sizeof(*a);
  a->group_id = groupId;
  po->actions_num = 1;
  po->actions =
      (struct ofl_action_header**)malloc(sizeof(struct ofl_action_header*));
  po->actions[0] = (struct ofl_action_header*)a;
  SendToSwitch(swtch, (struct ofl_msg_header*)po, 0);
  free(po->actions);
  free(a);
  if (po->data) free(po->data);
  free(po);
}

// ------------------------------------------------------------------
//  Build a 42-byte Ethernet+ARP reply frame (proxy ARP)
// ------------------------------------------------------------------
std::array<uint8_t, 60>
ZmqOpenFlowController::BuildArpReply(uint64_t targetMac, uint32_t targetIp,
                                     uint64_t requesterMac, uint32_t requesterIp) {
  // Sized to Ethernet minimum frame (60 bytes payload + 4 FCS = 64). The
  // 18 trailing bytes are zero padding; without it some receivers truncate
  // the tail of the ARP payload.
  std::array<uint8_t, 60> f{};
  // Eth dst: requester MAC
  for (int i = 0; i < 6; ++i) f[i] = (requesterMac >> (8 * (5 - i))) & 0xFF;
  // Eth src: target MAC (the host we are proxying for)
  for (int i = 0; i < 6; ++i) f[6 + i] = (targetMac >> (8 * (5 - i))) & 0xFF;
  // Ethertype: 0x0806
  f[12] = 0x08; f[13] = 0x06;
  // HW type: 1 (Ethernet)
  f[14] = 0x00; f[15] = 0x01;
  // Proto type: 0x0800 (IPv4)
  f[16] = 0x08; f[17] = 0x00;
  // HW len, proto len
  f[18] = 6; f[19] = 4;
  // Op: 2 (reply)
  f[20] = 0x00; f[21] = 0x02;
  // Sender HW: target MAC
  for (int i = 0; i < 6; ++i) f[22 + i] = (targetMac >> (8 * (5 - i))) & 0xFF;
  // Sender proto: target IP (host byte order in memory; serialize big-endian)
  f[28] = (targetIp >> 24) & 0xFF;
  f[29] = (targetIp >> 16) & 0xFF;
  f[30] = (targetIp >>  8) & 0xFF;
  f[31] = (targetIp      ) & 0xFF;
  // Target HW: requester MAC
  for (int i = 0; i < 6; ++i) f[32 + i] = (requesterMac >> (8 * (5 - i))) & 0xFF;
  // Target proto: requester IP
  f[38] = (requesterIp >> 24) & 0xFF;
  f[39] = (requesterIp >> 16) & 0xFF;
  f[40] = (requesterIp >>  8) & 0xFF;
  f[41] = (requesterIp      ) & 0xFF;
  return f;
}

// ------------------------------------------------------------------
//  Install a MAC-destination flow with idle timeout
// ------------------------------------------------------------------
void ZmqOpenFlowController::InstallFlow(uint64_t dpid, uint64_t dstMac,
                                        uint32_t outPort) {
  std::string macStr = FormatMac(dstMac);
  std::ostringstream cmd;
  // No idle timeout. Path freshness is maintained explicitly by
  // RecomputeAllRoutes (called when ML applies cost deltas) and by
  // PreInstallAllPaths at scenario boot. Idle expiry under sustained TCP
  // load triggered a BOFUSS cleanup cascade that crashed simulations
  // around the 30s mark.
  cmd << "flow-mod cmd=add,table=0,prio=100 eth_dst=" << macStr
      << " apply:output=" << outPort;
  DpctlExecute(dpid, cmd.str());
  m_installedFlows[dpid][dstMac] = outPort;
}

// ------------------------------------------------------------------
//  Proactive installer — see header for invariants.
// ------------------------------------------------------------------
void ZmqOpenFlowController::PreInstallAllPaths(
    const std::vector<HostInfo>& hosts) {
  for (const auto& h : hosts) {
    m_macToLoc[h.mac] = {h.dpid, h.port};
  }
  uint32_t installs = 0;
  uint32_t skips = 0;
  for (const auto& [srcDpid, srcSw] : m_switchMap) {
    for (const auto& h : hosts) {
      if (srcDpid == h.dpid) {
        InstallFlow(srcDpid, h.mac, h.port);
        ++installs;
        continue;
      }
      auto outOpt = m_topology.GetOutPort(srcDpid, h.dpid);
      if (!outOpt) {
        ++skips;
        continue;
      }
      InstallFlow(srcDpid, h.mac, *outOpt);
      ++installs;
    }
  }
  NS_LOG_INFO("PreInstallAllPaths: installed=" << installs
                                                << " skipped=" << skips
                                                << " hosts=" << hosts.size()
                                                << " switches="
                                                << m_switchMap.size());
}

// ------------------------------------------------------------------
//  Inter-domain (Option A) — pre-install static flow-mods routing every
//  external MAC toward this domain's border switch, and on the border
//  switch itself toward the inter-domain CSMA link. See header comment.
// ------------------------------------------------------------------
void ZmqOpenFlowController::InstallExternalHostRoutes(
    const std::vector<ExternalHostRoute>& routes) {
  uint32_t installs = 0;
  uint32_t skips = 0;
  for (const auto& r : routes) {
    for (const auto& [srcDpid, srcSw] : m_switchMap) {
      if (srcDpid == r.border_dpid) {
        InstallFlow(srcDpid, r.dst_mac, r.border_out_port);
        ++installs;
        continue;
      }
      auto outOpt = m_topology.GetOutPort(srcDpid, r.border_dpid);
      if (!outOpt) {
        ++skips;
        continue;
      }
      InstallFlow(srcDpid, r.dst_mac, *outOpt);
      ++installs;
    }
  }
  NS_LOG_INFO("InstallExternalHostRoutes: ctrl_id=" << m_ml.controller_id
              << " routes=" << routes.size()
              << " installs=" << installs
              << " skips=" << skips);
}

// ------------------------------------------------------------------
//  Install / refresh the per-switch flood group (type=ALL).
//  Buckets = host-facing ports ∪ spanning-tree ports for this switch.
//  On first install also adds the broadcast→group flow rule.
// ------------------------------------------------------------------
void ZmqOpenFlowController::InstallOrUpdateFloodGroup(uint64_t dpid) {
  std::set<uint32_t> ports; // ordered for stable log output

  auto portIt = m_switchPorts.find(dpid);
  if (portIt != m_switchPorts.end()) {
    for (uint32_t p : portIt->second) {
      if (p == 0 || p >= OFPP_MAX) continue;
      if (!m_topology.IsSwitchLinkPort(dpid, p)) ports.insert(p);
    }
  }
  auto stIt = m_spanningTree.find(dpid);
  if (stIt != m_spanningTree.end()) {
    for (uint32_t p : stIt->second) {
      if (p == 0 || p >= OFPP_MAX) continue;
      ports.insert(p);
    }
  }

  if (ports.empty()) return;

  bool firstInstall = !m_floodGroupInstalled.count(dpid);
  std::ostringstream gm;
  gm << "group-mod cmd=" << (firstInstall ? "add" : "mod")
     << ",type=all,group=" << kFloodGroupId;
  std::ostringstream logPorts;
  bool first = true;
  for (uint32_t p : ports) {
    gm << " weight=0,port=any,group=any output=" << p;
    if (!first) logPorts << ",";
    logPorts << p;
    first = false;
  }
  DpctlExecute(dpid, gm.str());

  if (firstInstall) {
    std::ostringstream fm;
    fm << "flow-mod cmd=add,table=0,prio=10 eth_dst=ff:ff:ff:ff:ff:ff"
       << " apply:group=" << kFloodGroupId;
    DpctlExecute(dpid, fm.str());
    m_floodGroupInstalled.insert(dpid);
  }

  NS_LOG_INFO("[GROUP] " << (firstInstall ? "Installed" : "Updated")
              << " flood group on dpid=" << dpid
              << " ports=[" << logPorts.str() << "]");
}

// Recompute next-hop for every (switch, knownDst) pair; reinstall the flow
// only when Dijkstra has moved the path. Called from ApplyDeltaCosts so live
// flows actually follow the ML-adjusted costs instead of sitting on the
// path that was current when the flow was first installed.
void ZmqOpenFlowController::RecomputeAllRoutes() {
  size_t rewrites = 0;
  for (const auto& [mac, loc] : m_macToLoc) {
    uint64_t dst_dpid = loc.first;
    uint32_t dst_port = loc.second;

    for (const auto& [dpid, sw] : m_switchMap) {
      uint32_t newOut;
      if (dpid == dst_dpid) {
        newOut = dst_port;
      } else {
        auto opt_path = m_topology.ShortestPath(dpid, dst_dpid);
        if (!opt_path || opt_path->size() < 2) continue;
        auto outp = m_topology.GetOutPort((*opt_path)[0], (*opt_path)[1]);
        if (!outp) continue;
        newOut = *outp;
      }
      auto dpIt = m_installedFlows.find(dpid);
      if (dpIt != m_installedFlows.end()) {
        auto mIt = dpIt->second.find(mac);
        if (mIt != dpIt->second.end() && mIt->second == newOut) continue;
      }
      InstallFlow(dpid, mac, newOut);
      ++rewrites;
    }
  }
  if (rewrites) NS_LOG_INFO("[ML] RecomputeAllRoutes rewrote " << rewrites << " flow entries");
}

// ------------------------------------------------------------------
//  Format a 32-bit IPv4 address (host-byte-order) as dotted-decimal
// ------------------------------------------------------------------
std::string ZmqOpenFlowController::FormatIp(uint32_t ip) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%u.%u.%u.%u", (ip >> 24) & 0xFF,
           (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, (ip) & 0xFF);
  return buf;
}

// ------------------------------------------------------------------
//  LLDP packet handler (extracted from HandlePacketIn)
// ------------------------------------------------------------------
void ZmqOpenFlowController::HandleLldpPacket(uint64_t dpid, uint32_t inPort,
                                             const uint8_t* data, size_t len) {
  size_t off = 14;
  uint64_t chassis_id = 0;
  uint32_t port_id = 0;
  uint64_t sendNs = 0;

  while (off + 2 <= len) {
    uint16_t hdr = (uint16_t)data[off] << 8 | (uint16_t)data[off + 1];
    off += 2;
    uint16_t tlv_type = (hdr >> 9) & 0x7f;
    uint16_t tlen = hdr & 0x1ff;
    if (tlv_type == 0) break;
    if (off + tlen > len) break;
    const uint8_t* val = data + off;

    if (tlv_type == 1 && tlen > 1) {
      try {
        chassis_id = std::stoull(std::string((const char*)(val + 1), tlen - 1));
      } catch (...) {
      }
    } else if (tlv_type == 2 && tlen > 1) {
      try {
        port_id = static_cast<uint32_t>(
            std::stoul(std::string((const char*)(val + 1), tlen - 1)));
      } catch (...) {
      }
    } else if (tlv_type == 9 && tlen == 8) {
      // LatencyTag: 8 bytes big-endian timestamp
      for (int i = 0; i < 8; ++i) sendNs = (sendNs << 8) | val[i];
    }
    off += tlen;
  }

  if (chassis_id == 0 || port_id == 0) return;

  double delayMs = 1.0;
  if (sendNs != 0) {
    uint64_t nowNs = Simulator::Now().GetNanoSeconds();
    if (nowNs > sendNs) {
      uint64_t totalNs = nowNs - sendNs;
      uint64_t halfRttA =
          m_echoRttNs.count(chassis_id) ? m_echoRttNs.at(chassis_id) / 2 : 0;
      uint64_t halfRttB =
          m_echoRttNs.count(dpid) ? m_echoRttNs.at(dpid) / 2 : 0;
      uint64_t correction = halfRttA + halfRttB;
      uint64_t linkNs = (totalNs > correction) ? totalNs - correction : totalNs;
      delayMs = std::max(0.001, static_cast<double>(linkNs) / 1e6);
    }
  }

  // Use real capacity from PORT_DESC if already received; fall back to 10 Mbps
  // floor so ComputeBaseCost is always finite.
  auto getSpeed = [&](uint64_t sw, uint32_t pno) -> uint32_t {
    auto sit = m_portStats.find(sw);
    if (sit == m_portStats.end()) return 0;
    auto pit = sit->second.find(pno);
    return (pit == sit->second.end()) ? 0 : pit->second.speed_kbps;
  };
  uint32_t kbps = getSpeed(dpid, inPort);
  if (!kbps) kbps = getSpeed(chassis_id, port_id);
  double capBps = kbps ? static_cast<double>(kbps) * 1000.0 : 0.0;
  double baseCost = ComputeBaseCost(delayMs, capBps);
  NS_LOG_DEBUG("[TRACE] HandleLldp t=" << Simulator::Now().GetSeconds()
               << " " << chassis_id << ":" << port_id << " <-> " << dpid
               << ":" << inPort << " before AddLink");
  bool changed = m_topology.AddLink(chassis_id, port_id, dpid, inPort, delayMs, baseCost);
  NS_LOG_DEBUG("[TRACE] HandleLldp after AddLink changed=" << changed);
  if (changed && capBps > 0) {
    m_topology.SetLinkCapacityBps(chassis_id, dpid, capBps);
    m_topology.SetLinkBaseCost(chassis_id, dpid, baseCost);
  }
  if (changed) {
    NS_LOG_DEBUG("[TRACE] HandleLldp before RebuildSpanningTree");
    RebuildSpanningTree();
    NS_LOG_DEBUG("[TRACE] HandleLldp after RebuildSpanningTree");
  }
  NS_LOG_INFO("[TOPO] Link: " << chassis_id << ":" << port_id << " <-> " << dpid
                              << ":" << inPort << " delay=" << delayMs
                              << "ms base_cost=" << baseCost << " cap=" << capBps / 1e6 << "Mbps");

  NS_LOG_DEBUG("[TRACE] HandleLldp before macToLoc flush size="
               << m_macToLoc.size());
  // Flush MAC entries learned on now-confirmed switch-link ports
  for (auto mit = m_macToLoc.begin(); mit != m_macToLoc.end();) {
    if (m_topology.IsSwitchLinkPort(mit->second.first, mit->second.second))
      mit = m_macToLoc.erase(mit);
    else
      ++mit;
  }
  NS_LOG_DEBUG("[TRACE] HandleLldp END");

  // WriteStateToJson();
}

// ------------------------------------------------------------------
//  ARP handler — extract sender IP for host IP map
// ------------------------------------------------------------------
void ZmqOpenFlowController::HandleArpPacket(const uint8_t* data, size_t len) {
  // Ethernet (14) + ARP (28) = 42 bytes minimum
  if (len < 42) return;

  const uint8_t* arp = data + 14;
  uint16_t hw_type = (uint16_t)arp[0] << 8 | arp[1];
  uint16_t proto = (uint16_t)arp[2] << 8 | arp[3];
  uint8_t hw_len = arp[4];
  uint8_t proto_len = arp[5];

  if (hw_type != 1 || proto != 0x0800 || hw_len != 6 || proto_len != 4) return;

  // Sender MAC: bytes 8-13 of ARP header (offset 22 from Ethernet start)
  uint64_t senderMac = 0;
  for (int i = 0; i < 6; ++i) senderMac = (senderMac << 8) | arp[8 + i];

  // Sender IP: bytes 14-17 of ARP header (offset 28 from Ethernet start)
  uint32_t senderIp = ((uint32_t)arp[14] << 24) | ((uint32_t)arp[15] << 16) |
                      ((uint32_t)arp[16] << 8) | (uint32_t)arp[17];

  // Target IP: bytes 24-27 of ARP header
  uint32_t targetIp = ((uint32_t)arp[24] << 24) | ((uint32_t)arp[25] << 16) |
                      ((uint32_t)arp[26] << 8) | (uint32_t)arp[27];

  // ARP operation: bytes 6-7
  uint16_t arpOp = (uint16_t)arp[6] << 8 | arp[7];

  // Log ARP requests and replies
  if (arpOp == 1 || arpOp == 2) {
    uint8_t s0 = (senderIp >> 24) & 0xFF;
    uint8_t s1 = (senderIp >> 16) & 0xFF;
    uint8_t s2 = (senderIp >> 8) & 0xFF;
    uint8_t s3 = senderIp & 0xFF;

    uint8_t t0 = (targetIp >> 24) & 0xFF;
    uint8_t t1 = (targetIp >> 16) & 0xFF;
    uint8_t t2 = (targetIp >> 8) & 0xFF;
    uint8_t t3 = targetIp & 0xFF;

    NS_LOG_INFO("[ARP] " << (arpOp == 1 ? "Request" : "Reply") << " from "
                         << (uint32_t)s0 << "." << (uint32_t)s1 << "."
                         << (uint32_t)s2 << "." << (uint32_t)s3 << " to "
                         << (uint32_t)t0 << "." << (uint32_t)t1 << "."
                         << (uint32_t)t2 << "." << (uint32_t)t3);
  }

  if (senderMac != 0 && senderIp != 0) {
    m_hostIpMap[senderMac] = senderIp;
    m_ipToMac[senderIp] = senderMac;
  }
}

// ------------------------------------------------------------------
//  ForwardPacket — lookup dst and route or flood
// ------------------------------------------------------------------
void ZmqOpenFlowController::ForwardPacket(Ptr<const RemoteSwitch> swtch,
                                          uint32_t inPort,
                                          struct ofl_msg_packet_in* msg,
                                          uint64_t srcMac, uint64_t dstMac) {
  uint64_t dpid = swtch->GetDpId();

  auto dstIt = m_macToLoc.find(dstMac);
  if (dstIt == m_macToLoc.end()) {
    FloodViaST(swtch, inPort, msg->buffer_id, msg->data, msg->data_length);
    return;
  }

  uint64_t dst_dpid = dstIt->second.first;
  uint32_t dst_port = dstIt->second.second;

  if (dst_dpid == dpid) {
    InstallFlow(dpid, dstMac, dst_port);
    SendPacketOut(swtch, inPort, msg->buffer_id, msg->data, msg->data_length,
                  dst_port);
    return;
  }

  auto opt_path = m_topology.ShortestPath(dpid, dst_dpid);
  if (!opt_path || opt_path->size() < 2) {
    FloodViaST(swtch, inPort, msg->buffer_id, msg->data, msg->data_length);
    return;
  }

  const std::vector<uint64_t>& path = *opt_path;
  for (size_t i = 0; i + 1 < path.size(); ++i) {
    auto outp_opt = m_topology.GetOutPort(path[i], path[i + 1]);
    if (outp_opt) InstallFlow(path[i], dstMac, *outp_opt);
  }
  InstallFlow(dst_dpid, dstMac, dst_port);

  auto first_out = m_topology.GetOutPort(path[0], path[1]);
  if (first_out)
    SendPacketOut(swtch, inPort, msg->buffer_id, msg->data, msg->data_length,
                  *first_out);
}

// ------------------------------------------------------------------
//  OpenFlow event handler
// ------------------------------------------------------------------
ofl_err ZmqOpenFlowController::HandlePacketIn(struct ofl_msg_packet_in* msg,
                                              Ptr<const RemoteSwitch> swtch,
                                              uint32_t xid) {
  uint64_t dpid = swtch->GetDpId();

  uint32_t inPort = 0;
  struct ofl_match_tlv* input =
      oxm_match_lookup(OXM_OF_IN_PORT, (struct ofl_match*)msg->match);
  if (input && input->value) {
    memcpy(&inPort, input->value, OXM_LENGTH(OXM_OF_IN_PORT));
  }

  m_switchPorts[dpid].insert(inPort);

  if (msg->data && msg->data_length >= 14) {
    const uint8_t* data = msg->data;
    uint16_t ethertype = (uint16_t)data[12] << 8 | (uint16_t)data[13];


    if (ethertype == 0x88CC) {
      HandleLldpPacket(dpid, inPort, data, msg->data_length);
      ofl_msg_free((struct ofl_msg_header*)msg, nullptr);
      return 0;
    }

    if (ethertype == 0x0806) {
      HandleArpPacket(data, msg->data_length);

      // Proxy ARP: synthesize the reply at the controller when we know the
      // target IP→MAC binding. Falls back to flood on miss.
      if (msg->data_length >= 42) {
        const uint8_t* arp = data + 14;
        uint16_t arpOp = (uint16_t)arp[6] << 8 | arp[7];
        if (arpOp == 1) {
          // Learn sender MAC now — a matching flow-mod on this switch may
          // silently route subsequent ICMP without generating another PacketIn.
          uint64_t senderMac = 0;
          for (int i = 6; i < 12; ++i) senderMac = (senderMac << 8) | data[i];
          if (senderMac && !m_topology.IsSwitchLinkPort(dpid, inPort)) {
            m_macToLoc[senderMac] = {dpid, inPort};
          }
          uint32_t senderIp = ((uint32_t)arp[14] << 24) |
                              ((uint32_t)arp[15] << 16) |
                              ((uint32_t)arp[16] << 8) | (uint32_t)arp[17];
          uint32_t targetIp = ((uint32_t)arp[24] << 24) |
                              ((uint32_t)arp[25] << 16) |
                              ((uint32_t)arp[26] << 8) | (uint32_t)arp[27];

          auto ipIt = m_ipToMac.find(targetIp);
          if (ipIt != m_ipToMac.end() && senderMac && senderIp) {
            uint64_t targetMac = ipIt->second;
            auto reply = BuildArpReply(targetMac, targetIp, senderMac, senderIp);
            SendPacketOut(swtch, OFPP_CONTROLLER, OFP_NO_BUFFER,
                          reply.data(), reply.size(), inPort);
            NS_LOG_INFO("[ARP] Proxy reply " << FormatIp(targetIp)
                        << " is-at " << FormatMac(targetMac)
                        << " -> " << FormatIp(senderIp));
            ofl_msg_free((struct ofl_msg_header*)msg, nullptr);
            return 0;
          }
          // Target unknown — flood via spanning tree.
          NS_LOG_INFO("[ARP] Flood (target " << FormatIp(targetIp) << " unknown)");
          FloodViaST(swtch, inPort, msg->buffer_id, msg->data,
                     msg->data_length);
          ofl_msg_free((struct ofl_msg_header*)msg, nullptr);
          return 0;
        }
        // ARP reply: fall through to normal ForwardPacket (dst MAC is
        // unicast).
      }
    }
  }

  struct ofl_match_tlv* ethSrc =
      oxm_match_lookup(OXM_OF_ETH_SRC, (struct ofl_match*)msg->match);
  struct ofl_match_tlv* ethDst =
      oxm_match_lookup(OXM_OF_ETH_DST, (struct ofl_match*)msg->match);
  if (!ethSrc || !ethDst || !ethSrc->value || !ethDst->value) {
    FloodViaST(swtch, inPort, msg->buffer_id, msg->data, msg->data_length);
    ofl_msg_free((struct ofl_msg_header*)msg, nullptr);
    return 0;
  }

  uint64_t src_mac = 0, dst_mac = 0;
  for (int i = 0; i < 6; ++i) {
    src_mac = (src_mac << 8) | (uint8_t)ethSrc->value[i];
    dst_mac = (dst_mac << 8) | (uint8_t)ethDst->value[i];
  }

  if (!m_topology.IsSwitchLinkPort(dpid, inPort)) {
    m_macToLoc[src_mac] = {dpid, inPort};
  }

  ForwardPacket(swtch, inPort, msg, src_mac, dst_mac);

  ofl_msg_free((struct ofl_msg_header*)msg, nullptr);
  return 0;
}

ofl_err ZmqOpenFlowController::HandlePortStatus(struct ofl_msg_port_status* msg,
                                                Ptr<const RemoteSwitch> swtch,
                                                uint32_t xid) {
  uint64_t dpid = swtch->GetDpId();

  if (msg && msg->desc) {
    uint32_t port_no = msg->desc->port_no;

    if (msg->reason == OFPPR_DELETE) {
      auto it = m_switchPorts.find(dpid);
      if (it != m_switchPorts.end()) it->second.erase(port_no);

      m_topology.RemovePort(dpid, port_no);

      for (auto mit = m_macToLoc.begin(); mit != m_macToLoc.end();) {
        if (mit->second.first == dpid && mit->second.second == port_no)
          mit = m_macToLoc.erase(mit);
        else
          ++mit;
      }
      InstallOrUpdateFloodGroup(dpid);
      // WriteStateToJson();
    } else if (msg->reason == OFPPR_ADD) {
      m_switchPorts[dpid].insert(port_no);
      InstallOrUpdateFloodGroup(dpid);
      // Probe the new port immediately for LLDP
      auto swIt = m_switchMap.find(dpid);
      if (swIt != m_switchMap.end()) {
        uint64_t nowNs = Simulator::Now().GetNanoSeconds();
        m_lldpSendNs[dpid][port_no] = nowNs;
        auto frame = BuildLldpFrame(dpid, port_no, nowNs);
        auto* po = BuildLldpPacketOut(port_no, frame.data(), frame.size());
        if (po) {
          SendToSwitch(swIt->second, po, 0);
          ofl_msg_free(po, nullptr);
        }
      }
    }
  }

  return OFSwitch13Controller::HandlePortStatus(msg, swtch, xid);
}

void ZmqOpenFlowController::HandlePortDescReply(
    struct ofl_msg_multipart_reply_port_desc* reply, uint64_t dpid) {
  NS_LOG_INFO("[PORT-DESC] Switch " << dpid << ": " << reply->stats_num
                                    << " ports");

  for (size_t i = 0; i < reply->stats_num; ++i) {
    uint32_t pno = reply->stats[i]->port_no;
    if (pno >= OFPP_MAX) continue;
    m_switchPorts[dpid].insert(pno);

    uint32_t kbps = reply->stats[i]->curr_speed;
    m_portStats[dpid][pno].speed_kbps = kbps;
    NS_LOG_INFO("[PORT-DESC]   port "
                << pno << " name="
                << (reply->stats[i]->name ? reply->stats[i]->name : "?")
                << " speed=" << kbps << " kbps");
  }
  InstallOrUpdateFloodGroup(dpid);
}

ofl_err ZmqOpenFlowController::HandleMultipartReply(
    struct ofl_msg_multipart_reply_header* msg, Ptr<const RemoteSwitch> swtch,
    uint32_t xid) {
  uint64_t dpid = swtch->GetDpId();
  if (msg->type == OFPMP_PORT_DESC) {
    HandlePortDescReply(
        reinterpret_cast<struct ofl_msg_multipart_reply_port_desc*>(msg), dpid);
  } else if (msg->type == OFPMP_PORT_STATS) {
    HandlePortStatsReply(
        reinterpret_cast<struct ofl_msg_multipart_reply_port*>(msg), dpid);
  } else if (msg->type == OFPMP_QUEUE) {
    HandleQueueStatsReply(
        reinterpret_cast<struct ofl_msg_multipart_reply_queue*>(msg), dpid);
  }
  return OFSwitch13Controller::HandleMultipartReply(msg, swtch, xid);
}

void ZmqOpenFlowController::HandleQueueStatsReply(
    struct ofl_msg_multipart_reply_queue* reply, uint64_t dpid) {
  double nowSec = Simulator::Now().GetSeconds();

  // Aggregate per-queue tx_errors (overrun drops) into a per-port total.
  // BOFUSS reports stats per (port_no, queue_id); we sum over queue_ids
  // because the default OFSwitch13 config uses a single queue per port,
  // but the aggregation is correct for multi-queue configs too.
  std::unordered_map<uint32_t, uint64_t> portTxErrors;
  for (size_t i = 0; i < reply->stats_num; ++i) {
    const struct ofl_queue_stats* s = reply->stats[i];
    if (s->port_no >= OFPP_MAX) continue;
    portTxErrors[s->port_no] += s->tx_errors;
  }

  for (const auto& [pno, errs] : portTxErrors) {
    PortStatsEntry& ps = m_portStats[dpid][pno];
    if (ps.prev_time_s > 0) {
      double dt = nowSec - ps.prev_time_s;
      if (dt > 0) {
        ps.q_tx_error_rate_pps =
            static_cast<double>(errs - ps.prev_q_tx_errors) / dt;
      }
    }
    ps.prev_q_tx_errors = errs;
    ps.q_tx_errors = errs;
  }
}

void ZmqOpenFlowController::HandlePortStatsReply(
    struct ofl_msg_multipart_reply_port* reply, uint64_t dpid) {
  double nowSec = Simulator::Now().GetSeconds();
  NS_LOG_INFO("[PORT-STATS] Switch " << dpid << ": " << reply->stats_num << " ports");

  for (size_t i = 0; i < reply->stats_num; ++i) {
    const struct ofl_port_stats* s = reply->stats[i];
    uint32_t pno = s->port_no;
    if (pno >= OFPP_MAX) continue;

    PortStatsEntry& ps = m_portStats[dpid][pno];

    // Compute instantaneous bit rates AND drop byte-rates from deltas BEFORE
    // overwriting any prev_* counters. Drop byte-rate ≈ Δdropped_pkts × avg
    // pkt size × 8 / dt — OF1.3 stats only give us packet counts for drops,
    // so we approximate byte size from the running average over the same window.
    double dt = (ps.prev_time_s > 0) ? (nowSec - ps.prev_time_s) : 0;
    if (dt > 0) {
      ps.rx_rate_bps = static_cast<double>(s->rx_bytes - ps.prev_rx_bytes) * 8.0 / dt;
      ps.tx_rate_bps = static_cast<double>(s->tx_bytes - ps.prev_tx_bytes) * 8.0 / dt;
      ps.rx_rate_bps = std::max(0.0, ps.rx_rate_bps);
      ps.tx_rate_bps = std::max(0.0, ps.tx_rate_bps);

      uint64_t dRxPkts  = (s->rx_packets > ps.prev_rx_packets)
                            ? (s->rx_packets - ps.prev_rx_packets) : 0;
      uint64_t dTxPkts  = (s->tx_packets > ps.prev_tx_packets)
                            ? (s->tx_packets - ps.prev_tx_packets) : 0;
      uint64_t dRxDrop  = (s->rx_dropped > ps.prev_rx_dropped)
                            ? (s->rx_dropped - ps.prev_rx_dropped) : 0;
      uint64_t dTxDrop  = (s->tx_dropped > ps.prev_tx_dropped)
                            ? (s->tx_dropped - ps.prev_tx_dropped) : 0;
      double avgRxPktB  = dRxPkts > 0
                            ? static_cast<double>(s->rx_bytes - ps.prev_rx_bytes) / dRxPkts
                            : 0.0;
      double avgTxPktB  = dTxPkts > 0
                            ? static_cast<double>(s->tx_bytes - ps.prev_tx_bytes) / dTxPkts
                            : 0.0;
      ps.rx_drop_rate_bps = dRxDrop * avgRxPktB * 8.0 / dt;
      ps.tx_drop_rate_bps = dTxDrop * avgTxPktB * 8.0 / dt;
    }

    // Update snapshot for next interval
    ps.prev_rx_bytes   = s->rx_bytes;
    ps.prev_tx_bytes   = s->tx_bytes;
    ps.prev_rx_packets = s->rx_packets;
    ps.prev_tx_packets = s->tx_packets;
    ps.prev_rx_dropped = s->rx_dropped;
    ps.prev_tx_dropped = s->tx_dropped;
    ps.prev_time_s     = nowSec;

    // Store all raw counters
    ps.rx_packets  = s->rx_packets;
    ps.tx_packets  = s->tx_packets;
    ps.rx_bytes    = s->rx_bytes;
    ps.tx_bytes    = s->tx_bytes;
    ps.rx_dropped  = s->rx_dropped;
    ps.tx_dropped  = s->tx_dropped;
    ps.rx_errors   = s->rx_errors;
    ps.tx_errors   = s->tx_errors;
    ps.duration_sec = s->duration_sec;

    // Assign link congestion stats based on usage
    // Only if the ml is disabled
    if (!m_ml.enabled && ps.speed_kbps > 0) {
      auto peerDpid = m_topology.GetPeerDpid(dpid, pno);
      if (peerDpid) {
        // Derive congestion factor from utilization with EWMA smoothing.
        double capBps = static_cast<double>(ps.speed_kbps) * 1000.0;
        double util = std::clamp(ps.tx_rate_bps / std::max(1.0, capBps), 0.0, 0.99);
        double cong = std::min(5.0, util * util / std::max(1e-3, 1.0 - util));
        double prev = m_topology.GetLinkCongestion(dpid, *peerDpid);
        double smooth = 0.7 * prev + 0.3 * cong;
        if (std::abs(smooth - prev) > 0.05) {
          m_topology.SetLinkCongestion(dpid, *peerDpid, smooth);
          m_congestionDirty = true;
        }
      }
    }

    NS_LOG_INFO("[PORT-STATS]   port " << pno
                << " rx=" << s->rx_bytes << "B tx=" << s->tx_bytes
                << "B rx_rate=" << ps.rx_rate_bps / 1e6 << " Mbps"
                << " tx_rate=" << ps.tx_rate_bps / 1e6 << " Mbps");
  }

  ComputeSwitchObservations(dpid);

  if (m_congestionDirty) {
    m_congestionDirty = false;
    RecomputeAllRoutes();
  }
}

void ZmqOpenFlowController::ComputeSwitchObservations(uint64_t dpid) {
  SwitchObservation obs;

  auto psIt = m_portStats.find(dpid);
  if (psIt == m_portStats.end()) {
    m_switchObs[dpid] = obs;
    return;
  }

  double maxDelayMs = 0.0, sumLBps = 0.0;
  for (const auto& [pno, ps] : psIt->second) {
    double cap = static_cast<double>(ps.speed_kbps) * 1000.0;
    // M/M/1 wait-time proxy: W ≈ base_delay · u / (1 - u). This is the only
    // queueing signal a real OF1.3 controller can compute — utilization and
    // link propagation delay are both standard. ε-guards keep the term
    // finite at u→1. Per-port `max` preserves the "worst egress" semantic
    // the old queue-depth-derived d_ms had.
    if (cap > 0) {
      double txBps = std::max(0.0, ps.tx_rate_bps);
      double util  = std::clamp(txBps / cap, 0.0, 0.999);
      auto peer = m_topology.GetPeerDpid(dpid, pno);
      double baseDelayMs = peer ? m_topology.GetLinkDelay(dpid, *peer) : 1.0;
      double dMs = baseDelayMs * (util / std::max(1e-3, 1.0 - util));
      maxDelayMs = std::max(maxDelayMs, dMs);
    }
    sumLBps += ps.rx_drop_rate_bps + ps.tx_drop_rate_bps;
  }
  obs.d_ms             = maxDelayMs;
  obs.L_bps            = sumLBps;

  // Drain switch forwarding energy based on total TX rate across all ports
  auto emIt = m_switchEnergyModel.find(dpid);
  if (emIt != m_switchEnergyModel.end() && emIt->second.initial_energy_j >= 0) {
    double totalTxBps = 0.0;
    for (const auto& [pno2, ps2] : psIt->second)
      totalTxBps += ps2.tx_rate_bps;
    double bytesForwarded = (totalTxBps / 8.0) * m_statsIntervalS;
    auto reIt = m_switchResidualEnergy.find(dpid);
    if (reIt != m_switchResidualEnergy.end()) {
      reIt->second = std::max(0.0, reIt->second -
                                   bytesForwarded * emIt->second.energy_per_byte_j);
      obs.residual_energy_j = reIt->second;
    }
  }

  m_switchObs[dpid] = obs;
  NS_LOG_INFO("[OBS] Switch " << dpid
              << " d_max=" << obs.d_ms << "ms"
              << " L=" << obs.L_bps / 1e6 << "Mbps"
              << " E=" << obs.residual_energy_j << "J");
}

// ------------------------------------------------------------------
//  Simulated-time triggers
// ------------------------------------------------------------------
void ZmqOpenFlowController::TriggerLldp() {
  bool hasLinks = !m_topology.GetAllLinks().empty();
  int switchIdx = 0;

  for (const auto& kv : m_switchMap) {
    uint64_t src_dpid = kv.first;
    Ptr<const RemoteSwitch> sw = kv.second;

    // Stagger switches by 2ms so their pipelines don't convoy
    Time baseOffset = MilliSeconds(switchIdx * 2);

    for (uint32_t p = 1; p <= kMaxLldpProbe; ++p) {
      // Once topology is stable, skip confirmed host-facing ports
      if (hasLinks && m_switchPorts[src_dpid].count(p) &&
          !m_topology.IsSwitchLinkPort(src_dpid, p)) {
        continue;
      }

      // Stagger ports by 100 µs
      Time portOffset = MicroSeconds(p * 100);
      Simulator::Schedule(baseOffset + portOffset,
                          &ZmqOpenFlowController::SendSingleLldp, this, sw,
                          src_dpid, p);
    }
    switchIdx++;
  }

  // 5 s until topology is complete, then back off to 30 s
  double nextLldp = hasLinks ? 30.0 : 5.0;
  Simulator::Schedule(Seconds(nextLldp), &ZmqOpenFlowController::TriggerLldp,
                      this);
}

void ZmqOpenFlowController::SendSingleLldp(Ptr<const RemoteSwitch> swtch,
                                           uint64_t dpid, uint32_t port) {
  uint64_t nowNs = Simulator::Now().GetNanoSeconds();
  m_lldpSendNs[dpid][port] = nowNs;

  auto frame = BuildLldpFrame(dpid, port, nowNs);
  auto* po = BuildLldpPacketOut(port, frame.data(), frame.size());
  if (po) {
    SendToSwitch(swtch, po, 0);
    ofl_msg_free(po, nullptr);
  }
}

void ZmqOpenFlowController::TriggerEcho() {
  for (const auto& [dpid, swtch] : m_switchMap) {
    auto& miss = m_echoMissCount[dpid];
    if (miss >= kEchoMaxMissed) {
      NS_LOG_WARN("[ECHO] Switch "
                  << dpid << " missed " << miss
                  << " consecutive echo replies — link may be down");
    }
    m_echoSendNs[dpid] = Simulator::Now().GetNanoSeconds();
    miss++;
    SendEchoRequest(swtch, 0);
  }
  Simulator::Schedule(Seconds(kEchoIntervalSec),
                      &ZmqOpenFlowController::TriggerEcho, this);
}

ofl_err ZmqOpenFlowController::HandleEchoReply(struct ofl_msg_echo* msg,
                                               Ptr<const RemoteSwitch> swtch,
                                               uint32_t xid) {
  uint64_t dpid = swtch->GetDpId();
  auto it = m_echoSendNs.find(dpid);
  if (it != m_echoSendNs.end()) {
    uint64_t rttNs = Simulator::Now().GetNanoSeconds() - it->second;
    m_echoRttNs[dpid] = rttNs;
    m_echoMissCount[dpid] = 0;
    m_echoSendNs.erase(it);
    NS_LOG_INFO("[ECHO] Switch " << dpid << " RTT=" << (rttNs / 1e6) << "ms");
  }
  return OFSwitch13Controller::HandleEchoReply(msg, swtch, xid);
}

void ZmqOpenFlowController::RebuildSpanningTree() {
  m_spanningTree = m_topology.ComputeSpanningTree();
  for (const auto& kv : m_switchMap) {
    InstallOrUpdateFloodGroup(kv.first);
  }
}

void ZmqOpenFlowController::FloodViaST(Ptr<const RemoteSwitch> inSwtch,
                                       uint32_t inPort, uint32_t bufferId,
                                       const uint8_t* data, size_t dataLen) {
  uint64_t inDpid = inSwtch->GetDpId();

  // Fast path: dataplane flood via the per-switch group table. The group's
  // OFPP_IN_PORT suppression keeps the spanning-tree fan-out loop-free, and
  // each downstream switch matches its own broadcast→group flow rule, so this
  // single PacketOut produces the full network-wide flood without any further
  // controller involvement.
  if (m_floodGroupInstalled.count(inDpid)) {
    SendPacketOutGroup(inSwtch, inPort, bufferId, data, dataLen, kFloodGroupId);
    return;
  }

  // If we have no spanning tree yet (topology not discovered), fall back to
  // flooding only on the ingress switch's host-facing ports, or all ports if
  // no host-facing ports are known yet.
  if (m_spanningTree.empty() && m_switchMap.size() == 1) {
    SendPacketOut(inSwtch, inPort, bufferId, data, dataLen, OFPP_FLOOD);
    return;
  }

  // parentPort[dpid] = the port on dpid through which the BFS parent is
  // reached (we skip it on output to avoid sending back the way we came).
  std::unordered_map<uint64_t, uint32_t> parentPort;
  std::unordered_set<uint64_t> visited;
  std::queue<uint64_t> q;

  parentPort[inDpid] = inPort;
  visited.insert(inDpid);
  q.push(inDpid);

  while (!q.empty()) {
    uint64_t dpid = q.front();
    q.pop();
    auto swIt = m_switchMap.find(dpid);
    if (swIt == m_switchMap.end()) continue;

    uint32_t skipPort = parentPort.count(dpid) ? parentPort.at(dpid) : 0;
    std::vector<uint32_t> outPorts;

    // Host-facing ports (known, non-switch-link), excluding the skip port.
    auto& known = m_switchPorts[dpid];
    for (uint32_t p : known) {
      if (p == skipPort) continue;
      if (!m_topology.IsSwitchLinkPort(dpid, p)) outPorts.push_back(p);
    }

    // Spanning-tree inter-switch ports, excluding the skip port.
    auto stIt = m_spanningTree.find(dpid);
    if (stIt != m_spanningTree.end()) {
      for (uint32_t p : stIt->second) {
        if (p == skipPort) continue;
        outPorts.push_back(p);

        auto peerDpid = m_topology.GetPeerDpid(dpid, p);
        if (peerDpid && !visited.count(*peerDpid)) {
          auto peerPort = m_topology.GetPeerPort(dpid, p);
          if (peerPort) parentPort[*peerDpid] = *peerPort;
          visited.insert(*peerDpid);
          q.push(*peerDpid);
        }
      }
    }

    if (outPorts.empty()) continue;

    bool isIngress = (dpid == inDpid);
    for (uint32_t p : outPorts) {
      if (isIngress)
        SendPacketOut(swIt->second, inPort, bufferId, data, dataLen, p);
      else
        SendPacketOut(swIt->second, OFPP_CONTROLLER, OFP_NO_BUFFER, data,
                      dataLen, p);
    }
  }
}

void ZmqOpenFlowController::TriggerStats() {
  for (const auto& kv : m_switchMap) {
    struct ofl_msg_header* req = BuildPortStatsRequest();
    if (req) {
      SendToSwitch(kv.second, req, 0);
      ofl_msg_free(req, nullptr);
    }
    struct ofl_msg_header* qreq = BuildQueueStatsRequest();
    if (qreq) {
      SendToSwitch(kv.second, qreq, 0);
      ofl_msg_free(qreq, nullptr);
    }
  }
  Simulator::Schedule(Seconds(m_statsIntervalS),
                      &ZmqOpenFlowController::TriggerStats, this);
}

// ------------------------------------------------------------------
//  State serialization
// ------------------------------------------------------------------
void ZmqOpenFlowController::WriteStateToJson() {
  std::string state_dir = "scratch/data/state";
  mkdir("scratch/data", 0755);
  mkdir(state_dir.c_str(), 0755);

  // Multi-controller (Phase 2): each instance writes to its own file so
  // simultaneously-running controllers don't clobber each other's state.
  // Default controller_id=0 keeps the legacy filename for single-ctrl runs.
  std::string output_file;
  if (m_ml.controller_id == 0) {
    output_file = state_dir + "/sdn_state.json";
  } else {
    output_file = state_dir + "/sdn_state_ctrl" +
                  std::to_string(m_ml.controller_id) + ".json";
  }

  std::ostringstream json;
  json << std::fixed << std::setprecision(3);
  json << "{\n";

  auto now = std::time(nullptr);
  json << "  \"timestamp\": " << now << ",\n";

  json << "  \"controller\": {\n";
  json << "    \"id\": \"sdn-controller-" << m_ml.controller_id << "\",\n";
  json << "    \"label\": \"SDN Controller " << m_ml.controller_id << "\",\n";
  json << "    \"controller_id\": " << m_ml.controller_id << ",\n";
  json << "    \"detail\": \"OpenFlow 1.3 Controller\"\n";
  json << "  },\n";

  // Switches
  json << "  \"switches\": [";
  bool first = true;
  for (const auto& kv : m_switchMap) {
    if (!first) json << ", ";
    json << "\n    {\"dpid\": " << kv.first << ", \"name\": \"S"
         << (kv.first - 1) << "\"}";
    first = false;
  }
  if (!first) json << "\n  ";
  json << "],\n";

  // Hosts — name and node_type for topology viewer display
  json << "  \"hosts\": [";
  first = true;
  for (const auto& kv : m_macToLoc) {
    uint64_t mac  = kv.first;
    uint64_t dpid = kv.second.first;
    uint32_t port = kv.second.second;

    if (!first) json << ", ";
    json << "\n    {\n";
    json << "      \"mac\": \"" << FormatMac(mac) << "\",\n";
    json << "      \"dpid\": " << dpid << ",\n";
    json << "      \"port\": " << port;

    auto ipIt = m_hostIpMap.find(mac);
    if (ipIt != m_hostIpMap.end())
      json << ",\n      \"ip\": \"" << FormatIp(ipIt->second) << "\"";

    auto annIt = m_hostAnnotations.find(mac);
    if (annIt != m_hostAnnotations.end()) {
      if (!annIt->second.name.empty())
        json << ",\n      \"name\": \"" << annIt->second.name << "\"";
      json << ",\n      \"node_type\": \"" << annIt->second.node_type << "\"";
    } else {
      json << ",\n      \"node_type\": \"host\"";
    }

    json << "\n    }";
    first = false;
  }
  if (!first) json << "\n  ";
  json << "],\n";

  // Links
  json << "  \"links\": [";
  first = true;
  for (const auto& link : m_topology.GetAllLinks()) {
    if (!first) json << ", ";
    double deltaPct = (link.base_cost > 0)
                          ? ((link.cost - link.base_cost) / link.base_cost) * 100.0
                          : 0.0;
    json << "\n    {\n";
    json << "      \"src_dpid\": " << link.src_dpid << ",\n";
    json << "      \"src_port\": " << link.src_port << ",\n";
    json << "      \"dst_dpid\": " << link.dst_dpid << ",\n";
    json << "      \"dst_port\": " << link.dst_port << ",\n";
    json << "      \"delay_ms\": " << link.delay_ms << ",\n";
    json << "      \"cost\": " << link.cost << ",\n";
    json << "      \"base_cost\": " << link.base_cost << ",\n";
    json << "      \"cost_delta_pct\": " << deltaPct << ",\n";
    json << "      \"capacity_bps\": " << link.capacity_bps << "\n";
    json << "    }";
    first = false;
  }
  if (!first) json << "\n  ";
  json << "],\n";

  // Full port statistics
  json << "  \"stats\": {";
  first = true;
  for (const auto& sw_kv : m_portStats) {
    if (!first) json << ", ";
    json << "\n    \"" << sw_kv.first << "\": {";
    bool first_port = true;
    for (const auto& port_kv : sw_kv.second) {
      const PortStatsEntry& ps = port_kv.second;
      if (!first_port) json << ", ";
      json << "\n      \"" << port_kv.first << "\": {\n";
      json << "        \"rx_packets\": "  << ps.rx_packets  << ",\n";
      json << "        \"tx_packets\": "  << ps.tx_packets  << ",\n";
      json << "        \"rx_bytes\": "    << ps.rx_bytes    << ",\n";
      json << "        \"tx_bytes\": "    << ps.tx_bytes    << ",\n";
      json << "        \"rx_dropped\": "  << ps.rx_dropped  << ",\n";
      json << "        \"tx_dropped\": "  << ps.tx_dropped  << ",\n";
      json << "        \"rx_errors\": "   << ps.rx_errors   << ",\n";
      json << "        \"tx_errors\": "   << ps.tx_errors   << ",\n";
      json << "        \"duration_sec\": " << ps.duration_sec << ",\n";
      json << "        \"rx_rate_bps\": " << ps.rx_rate_bps << ",\n";
      json << "        \"tx_rate_bps\": " << ps.tx_rate_bps << ",\n";
      json << "        \"rx_drop_rate_bps\": " << ps.rx_drop_rate_bps << ",\n";
      json << "        \"tx_drop_rate_bps\": " << ps.tx_drop_rate_bps << ",\n";
      json << "        \"q_tx_errors\": "         << ps.q_tx_errors         << ",\n";
      json << "        \"q_tx_error_rate_pps\": " << ps.q_tx_error_rate_pps << ",\n";
      json << "        \"speed_kbps\": "  << ps.speed_kbps  << "\n";
      json << "      }";
      first_port = false;
    }
    if (!first_port) json << "\n    ";
    json << "}";
    first = false;
  }
  if (!first) json << "\n  ";
  json << "},\n";

  // Per-switch observation snapshot — reward-debug fields only. The ML
  // observation now flows through edge_attr (see BuildMlStatePayload),
  // so the legacy M/M/1/K aggregates (lambda_bps, mu_max_bps, rho, K, N,
  // p_loss, rbw_bps) are gone.
  json << "  \"switch_observations\": {";
  first = true;
  for (const auto& kv : m_switchObs) {
    if (!first) json << ", ";
    const SwitchObservation& o = kv.second;
    json << "\n    \"" << kv.first << "\": {\n";
    json << "      \"d_ms\": "             << o.d_ms             << ",\n";
    json << "      \"L_bps\": "            << o.L_bps            << ",\n";
    if (o.residual_energy_j >= 0)
      json << "      \"residual_energy_j\": " << o.residual_energy_j << "\n";
    else
      json << "      \"residual_energy_j\": null\n";
    json << "    }";
    first = false;
  }
  if (!first) json << "\n  ";
  json << "},\n";

  // ATVM — inter-switch traffic volumes, both directions per link
  // GetAllLinks() deduplicates (lower DPID first), so we emit both
  // src→dst and dst→src explicitly to capture one-way flows correctly.
  json << "  \"atvm\": [";
  first = true;
  auto emitAtvm = [&](uint64_t srcDpid, uint32_t srcPort,
                       uint64_t dstDpid) {
    auto psIt = m_portStats.find(srcDpid);
    if (psIt == m_portStats.end()) return;
    auto ppIt = psIt->second.find(srcPort);
    if (ppIt == psIt->second.end()) return;
    if (!first) json << ", ";
    json << "\n    {\"src\": " << srcDpid
         << ", \"dst\": " << dstDpid
         << ", \"tx_bps\": " << ppIt->second.tx_rate_bps << "}";
    first = false;
  };
  for (const auto& link : m_topology.GetAllLinks()) {
    emitAtvm(link.src_dpid, link.src_port, link.dst_dpid);
    emitAtvm(link.dst_dpid, link.dst_port, link.src_dpid);
  }
  if (!first) json << "\n  ";
  json << "],\n";

  // Global μ_max — max port speed across the topology (ATVM normalisation
  // constant). Used by the visualizer; no longer derived from the gone
  // per-switch mu_max_bps M/M/1/K aggregate.
  double muMaxGlobal = 0;
  for (const auto& sw_kv : m_portStats)
    for (const auto& port_kv : sw_kv.second) {
      double cap = static_cast<double>(port_kv.second.speed_kbps) * 1000.0;
      muMaxGlobal = std::max(muMaxGlobal, cap);
    }
  json << "  \"mu_max_global_bps\": " << muMaxGlobal << ",\n";

  // Control links
  json << "  \"control_links\": [";
  first = true;
  for (const auto& kv : m_switchMap) {
    if (!first) json << ", ";
    json << "\n    {\"dpid\": " << kv.first << "}";
    first = false;
  }
  if (!first) json << "\n  ";
  json << "]\n";

  json << "}\n";

  std::ofstream file(output_file);
  if (file.is_open()) {
    file << json.str();
    NS_LOG_DEBUG("Wrote state to " << output_file);
  } else {
    NS_LOG_WARN("Failed to open " << output_file);
  }
}

// ------------------------------------------------------------------
//  Online FDRL local-agent loop
// ------------------------------------------------------------------
void ZmqOpenFlowController::MlOpenSocket() {
  try {
    m_mlCtx = std::make_unique<zmq::context_t>(1);
    m_mlSock = std::make_unique<zmq::socket_t>(*m_mlCtx, zmq::socket_type::req);
    // Short timeouts so a missing Python service can never stall the sim.
    m_mlSock->set(zmq::sockopt::rcvtimeo, 200);
    m_mlSock->set(zmq::sockopt::sndtimeo, 200);
    m_mlSock->set(zmq::sockopt::linger,   0);
    // REQ sockets normally lock send→recv→send into strict alternation; on a
    // recv timeout the next send would EFSM. REQ_RELAXED + REQ_CORRELATE let
    // us resend after a missed reply (necessary when Python is down).
    m_mlSock->set(zmq::sockopt::req_relaxed, 1);
    m_mlSock->set(zmq::sockopt::req_correlate, 1);
    m_mlSock->connect(m_ml.endpoint);
    NS_LOG_INFO("[ML] Connected ZMQ REQ to " << m_ml.endpoint);
  } catch (const std::exception& e) {
    NS_LOG_WARN("[ML] Failed to open ZMQ socket: " << e.what()
                << " — agent will be inert");
    m_mlSock.reset();
    m_mlCtx.reset();
  }
}

void ZmqOpenFlowController::MlSendHello() {
  if (!m_mlSock) return;

  // GNN dims — keep in sync with python's _build_graph_data():
  //   node_feat_dim = 2 (depletion, echo_rtt_norm)
  //   edge_feat_dim = 3 (drop_norm, utilization, cost_norm)
  // num_switches / num_links let the Python side allocate buffers and verify
  // payload shape at runtime. action_dim still maps 1:1 to m_mlLinkOrder.
  size_t numSwitches = m_mlNodeOrder.size();
  size_t numLinks    = m_mlLinkOrder.size();
  size_t actionDim   = numLinks;

  std::ostringstream hello;
  hello << "{\"cmd\":\"hello\","
        << "\"arch\":\"gnn-v3\","
        << "\"num_switches\":" << numSwitches << ","
        << "\"num_links\":" << numLinks << ","
        << "\"node_feat_dim\":2,"
        << "\"edge_feat_dim\":3,"
        << "\"action_dim\":" << actionDim << ","
        << "\"seed\":" << m_ml.seed << ","
        << "\"resume\":" << (m_ml.resume ? "true" : "false") << ","
        << "\"checkpoint_every_n_ticks\":" << m_ml.checkpoint_every_n_ticks
        << "}";

  std::string req = hello.str();
  try {
    zmq::message_t request(req.size());
    std::memcpy(request.data(), req.data(), req.size());
    auto sres = m_mlSock->send(request, zmq::send_flags::none);
    if (!sres) {
      NS_LOG_WARN("[ML] hello send timed out");
      return;
    }
    zmq::message_t reply;
    auto rres = m_mlSock->recv(reply, zmq::recv_flags::none);
    if (!rres) {
      NS_LOG_WARN("[ML] hello reply timed out");
      return;
    }
    NS_LOG_INFO("[ML] hello ack: num_switches=" << numSwitches
                << " num_links=" << numLinks
                << " action_dim=" << actionDim);
  } catch (const std::exception& e) {
    NS_LOG_WARN("[ML] hello failed: " << e.what());
  }
}

std::string ZmqOpenFlowController::BuildMlStatePayload() {
  std::ostringstream s;
  s << std::fixed << std::setprecision(6);
  s << "{\"cmd\":\"observe\","
    << "\"tick\":" << m_mlTick << ","
    << "\"prev_reward\":" << m_mlPrevReward << ","
    << "\"explore\":" << (m_ml.explore ? "true" : "false") << ","
    << "\"state\":{";

  // ---- node index (dpid list in frozen order) ----
  // Position i in this array == GNN node index for that dpid. Python uses it
  // to map per_link.src/dst → integer indices for edge_index. Re-emitted every
  // tick because m_mlNodeOrder is immutable after the first tick, and a fresh
  // copy keeps the payload self-contained for any consumer.
  s << "\"node_index\":[";
  for (size_t i = 0; i < m_mlNodeOrder.size(); ++i) {
    if (i) s << ",";
    s << m_mlNodeOrder[i];
  }
  s << "],";

  // ---- per-switch features (in frozen node order) ----
  // Health-only signals now. Traffic physics (queue depth, drop rate,
  // utilization) has moved to per_link edge_attr where the GAT can attend
  // over per-port truth instead of a switch-level reduction.
  s << "\"per_switch\":[";
  bool firstSw = true;
  for (uint64_t dpid : m_mlNodeOrder) {
    SwitchObservation obs = m_switchObs.count(dpid) ? m_switchObs.at(dpid)
                                                    : SwitchObservation{};
    const auto& em = m_switchEnergyModel.count(dpid)
                        ? m_switchEnergyModel.at(dpid)
                        : SwitchEnergyModel{};
    double energyFrac = (em.initial_energy_j > 0 && obs.residual_energy_j >= 0)
                            ? std::clamp(obs.residual_energy_j / em.initial_energy_j,
                                         0.0, 1.0)
                            : 1.0;
    double rttNs = m_echoRttNs.count(dpid) ? m_echoRttNs.at(dpid) : 0;

    if (!firstSw) s << ",";
    s << "{\"dpid\":" << dpid
      << ",\"depletion\":" << (1.0 - energyFrac)
      << ",\"echo_rtt_ns\":" << rttNs
      << "}";
    firstSw = false;
  }
  s << "],";

  // ---- per-link features ----
  // Each canonical link (a, b) carries both directions of traffic physics.
  // m_mlLinkOrder is one entry per physical link (frozen for action-vector
  // stability); Python uses the src_* fields for the canonical edge_attr
  // and the dst_* fields for the reverse-direction edge in bidirectional
  // message passing.
  auto findDirStats = [&](uint64_t srcDpid, uint64_t dstDpid,
                          double& txBpsOut, double& dropRateBpsOut) {
    txBpsOut = 0.0; dropRateBpsOut = 0.0;
    auto psIt = m_portStats.find(srcDpid);
    if (psIt == m_portStats.end()) return;
    for (const auto& [pno, ps] : psIt->second) {
      auto peer = m_topology.GetPeerDpid(srcDpid, pno);
      if (peer && *peer == dstDpid) {
        txBpsOut       += ps.tx_rate_bps;
        dropRateBpsOut += ps.tx_drop_rate_bps;
      }
    }
  };

  s << "\"per_link\":[";
  bool firstLink = true;
  for (const auto& [a, b] : m_mlLinkOrder) {
    double cost     = m_topology.GetLinkCost(a, b);
    double baseCost = m_topology.GetBaseLinkCost(a, b);
    double cap      = std::max(m_topology.GetLinkCapacityBps(a, b),
                               m_topology.GetLinkCapacityBps(b, a));
    double delayMs  = m_topology.GetLinkDelay(a, b);

    double srcTxBps = 0, dstTxBps = 0;
    double srcDropRateBps = 0, dstDropRateBps = 0;
    findDirStats(a, b, srcTxBps, srcDropRateBps);
    findDirStats(b, a, dstTxBps, dstDropRateBps);

    double srcUtil = (cap > 0) ? (srcTxBps / cap) : 0.0;
    double dstUtil = (cap > 0) ? (dstTxBps / cap) : 0.0;

    if (!firstLink) s << ",";
    s << "{\"src\":" << a << ",\"dst\":" << b
      // tx_bps / utilization are aliases for the src-direction (a→b) and
      // kept for backward-compat with non-ML consumers (visualizer, JSON
      // dumpers). Python ML reads src_* / dst_* explicitly for asymmetry.
      << ",\"tx_bps\":" << srcTxBps
      << ",\"capacity_bps\":" << cap
      << ",\"utilization\":" << srcUtil
      << ",\"cost\":" << cost
      << ",\"base_cost\":" << baseCost
      << ",\"delay_ms\":" << delayMs
      << ",\"src_tx_bps\":"        << srcTxBps
      << ",\"src_utilization\":"   << srcUtil
      << ",\"src_drop_rate_bps\":" << srcDropRateBps
      << ",\"dst_tx_bps\":"        << dstTxBps
      << ",\"dst_utilization\":"   << dstUtil
      << ",\"dst_drop_rate_bps\":" << dstDropRateBps
      << "}";
    firstLink = false;
  }
  s << "],";
  // Global energy-fairness signal — single scalar appended after per_link.
  s << "\"residual_energy_stddev\":" << ComputeResidualEnergyStddev();
  s << "}}";
  return s.str();
}

double ZmqOpenFlowController::ComputeMlReward() {
  if (m_switchObs.empty()) return 0.0;

  // ---- 1. Delay quality term (absolute, normalized) ----
  double currDelay = 0.0;
  uint32_t currN = 0;
  for (const auto& [dpid, o] : m_switchObs) {
    if (o.d_ms > 0) { currDelay += o.d_ms; ++currN; }
  }
  double meanCurr = (currN > 0) ? (currDelay / currN) : 0.0;
  double delayRef = std::max(1.0, m_ml.delay_ref_ms);
  double delayQuality = 1.0 - std::clamp(meanCurr / delayRef, 0.0, 1.0);

  // ---- 2. Loss quality term (absolute, normalized) ----
  double currLoss = 0.0;
  for (const auto& [dpid, o] : m_switchObs) currLoss += o.L_bps;
  double lossRef = std::max(1.0, m_ml.loss_ref_bps);
  double lossQuality = 1.0 - std::clamp(currLoss / lossRef, 0.0, 1.0);

  // ---- 3. Power-consumption penalty ----
  // currPower_W approximates instantaneous aggregate draw from per-switch
  // tx rates × per-byte energy cost. Sum across all switches with energy
  // models; switches without a configured model contribute 0.
  double currPowerW = 0.0;
  for (const auto& [dpid, em] : m_switchEnergyModel) {
    if (em.initial_energy_j <= 0) continue;
    auto psIt = m_portStats.find(dpid);
    if (psIt == m_portStats.end()) continue;
    double txBps = 0.0;
    for (const auto& [pno, ps] : psIt->second) txBps += ps.tx_rate_bps;
    // power_W = bytes/s × J/byte = J/s = W.
    currPowerW += (txBps / 8.0) * em.energy_per_byte_j;
  }
  double powerRef = std::max(1.0, m_ml.power_ref_w);
  double powerCost = std::clamp(currPowerW / powerRef, 0.0, 1.0);

  // ---- 4. Utilization penalty (mean + peak) ----
  double utilSum = 0.0, utilPeak = 0.0;
  uint32_t utilN = 0;
  // Also accumulate per-switch tx_bps for the reserve-aware term below.
  std::unordered_map<uint64_t, double> switchTxBps;
  double totalTxBps = 0.0;
  for (const auto& [a, b] : m_mlLinkOrder) {
    double cap = std::max(m_topology.GetLinkCapacityBps(a, b),
                          m_topology.GetLinkCapacityBps(b, a));
    double txBps = 0.0;
    auto psIt = m_portStats.find(a);
    if (psIt != m_portStats.end()) {
      for (const auto& [pno, ps] : psIt->second) {
        auto peer = m_topology.GetPeerDpid(a, pno);
        if (peer && *peer == b) txBps += ps.tx_rate_bps;
      }
    }
    switchTxBps[a] += txBps;
    totalTxBps += txBps;
    if (cap > 0.0) {
      double u = std::clamp(txBps / cap, 0.0, 1.0);
      utilSum += u;
      if (u > utilPeak) utilPeak = u;
      ++utilN;
    }
  }
  double meanUtil = (utilN > 0) ? (utilSum / utilN) : 0.0;
  double utilPenalty = 0.5 * meanUtil + 0.5 * utilPeak;

  // ---- 5. Active-switch footprint penalty ----
  // A switch counts as "active" if it forwarded ≥ ~1 kbps this interval.
  // Tiny bookkeeping traffic (echo, LLDP) is below this threshold.
  constexpr double kActiveThresholdBps = 1024.0;
  uint32_t activeCount = 0;
  for (const auto& [dpid, sw] : m_switchMap) {
    auto it = switchTxBps.find(dpid);
    if (it != switchTxBps.end() && it->second >= kActiveThresholdBps) ++activeCount;
  }
  double totalSw = std::max<size_t>(1, m_switchMap.size());
  double footprintPenalty = static_cast<double>(activeCount) / totalSw;

  // ---- 6. Reserve-aware traffic penalty ----
  // For each switch, share_of_total_tx · (1 - residual_frac)². Quadratic
  // in (1 - residual_frac) so the penalty is near-zero for fresh switches
  // and spikes sharply for nearly-depleted ones. Drives traffic AWAY from
  // low-reserve switches.
  double reserveAwarePenalty = 0.0;
  if (totalTxBps > 0.0) {
    for (const auto& [dpid, tx] : switchTxBps) {
      double frac = 1.0;
      auto emIt = m_switchEnergyModel.find(dpid);
      if (emIt != m_switchEnergyModel.end() && emIt->second.initial_energy_j > 0) {
        auto reIt = m_switchResidualEnergy.find(dpid);
        if (reIt != m_switchResidualEnergy.end())
          frac = std::clamp(reIt->second / emIt->second.initial_energy_j, 0.0, 1.0);
      }
      double share = tx / totalTxBps;
      double depletion = 1.0 - frac;
      reserveAwarePenalty += share * depletion * depletion;
    }
  }

  // ---- 7. Residual-energy variance (balance term) ----
  // stddev across all energy-tracked switches. Low stddev = even depletion;
  // forces the agent to round-robin its high-traffic paths over time.
  double stddevResidual = ComputeResidualEnergyStddev();
  // stddev is already in [0, 0.5] practically (residual_frac ∈ [0,1]);
  // multiply by 2 to push it into [0, 1] range for weight comparability.
  double balancePenalty = std::clamp(2.0 * stddevResidual, 0.0, 1.0);

  // ---- Combine ----
  double R =   m_ml.reward_alpha * delayQuality
             + m_ml.reward_beta  * lossQuality
             - m_ml.reward_gamma * powerCost
             - m_ml.reward_delta * utilPenalty
             - m_ml.reward_zeta  * footprintPenalty
             - m_ml.reward_eta   * reserveAwarePenalty
             - m_ml.reward_theta * balancePenalty;

  // Affine min-max scale R into [-1, 1] using the analytic bounds. Every term
  // above is clamped to [0, 1] individually, so R is bounded by:
  //   R_max =  alpha + beta              (positive-weighted terms maxed out)
  //   R_min = -(gamma + delta + zeta + eta + theta)
  // Linear rescale is a bijection over this range — distinct R values stay
  // distinct in R_norm, so the Critic still sees gradient between "mildly
  // bad" and "catastrophic" rewards (vs. a hard clamp which would flatten them).
  const double posBound = m_ml.reward_alpha + m_ml.reward_beta;
  const double negBound = m_ml.reward_gamma + m_ml.reward_delta
                        + m_ml.reward_zeta  + m_ml.reward_eta
                        + m_ml.reward_theta;
  const double span = posBound + negBound;
  double R_norm;
  if (span > 0.0) {
    R_norm = 2.0 * (R + negBound) / span - 1.0;
  } else {
    R_norm = 0.0;
  }
  if (R_norm < -1.0 || R_norm > 1.0) {
    NS_LOG_DEBUG("[ML] reward out of analytic bounds (term-clamp regression?)"
                 << " R=" << R << " R_norm=" << R_norm);
    R_norm = std::clamp(R_norm, -1.0, 1.0);
  }

  NS_LOG_DEBUG("[ML] reward tick=" << m_mlTick
               << " R=" << R << " R_norm=" << R_norm
               << " d=" << delayQuality << " l=" << lossQuality
               << " p=" << powerCost     << " u=" << utilPenalty
               << " f=" << footprintPenalty
               << " e=" << reserveAwarePenalty
               << " b=" << balancePenalty
               << " | P_W=" << currPowerW << " stddev=" << stddevResidual);
  return R_norm;
}

double ZmqOpenFlowController::ComputeResidualEnergyStddev() const {
  std::vector<double> fracs;
  fracs.reserve(m_switchEnergyModel.size());
  for (const auto& [dpid, em] : m_switchEnergyModel) {
    if (em.initial_energy_j <= 0) continue;
    auto reIt = m_switchResidualEnergy.find(dpid);
    double frac = (reIt != m_switchResidualEnergy.end())
                      ? std::clamp(reIt->second / em.initial_energy_j, 0.0, 1.0)
                      : 1.0;
    fracs.push_back(frac);
  }
  if (fracs.size() < 2) return 0.0;
  double mean = 0.0;
  for (double f : fracs) mean += f;
  mean /= fracs.size();
  double var = 0.0;
  for (double f : fracs) { double d = f - mean; var += d * d; }
  var /= fracs.size();
  return std::sqrt(var);
}

double ZmqOpenFlowController::CurrentActionScale() const {
  // Linear taper from action_scale_start → action_scale over taper_ticks.
  if (m_ml.taper_ticks == 0) return m_ml.action_scale;
  if (m_mlTick >= m_ml.taper_ticks) return m_ml.action_scale;
  double t = static_cast<double>(m_mlTick) / m_ml.taper_ticks;
  return m_ml.action_scale_start
         + (m_ml.action_scale - m_ml.action_scale_start) * t;
}

void ZmqOpenFlowController::ApplyDeltaCosts(const std::vector<double>& deltas) {
  size_t n = std::min(deltas.size(), m_mlLinkOrder.size());
  double scale = CurrentActionScale();
  bool anyChanged = false;
  for (size_t i = 0; i < n; ++i) {
    double d = deltas[i];
    if (d > scale)        d = scale;
    else if (d < -scale)  d = -scale;

    auto [a, b] = m_mlLinkOrder[i];
    double prev = m_topology.GetLinkMlDelta(a, b);
    if (std::abs(d - prev) > 1e-9) {
      m_topology.SetLinkMlDelta(a, b, d);
      anyChanged = true;
    }
  }
  // Critical: existing flow entries are keyed only on eth_dst and never
  // expire, so continuous flows never re-PacketIn and never see the new
  // costs. Walk the routing table now and rewrite next-hop ports where
  // Dijkstra has moved.
  if (anyChanged) RecomputeAllRoutes();
}

void ZmqOpenFlowController::MlTick() {
  if (!m_ml.enabled) return;

  // Observation refresh is owned by TriggerStats(): when ML is enabled,
  // m_statsIntervalS is collapsed to m_ml.interval_s so a port+queue stats
  // sweep already fires every tick. The ML payload below reads m_switchObs,
  // which TriggerStats keeps current, so there's no need to re-sweep here.

  // Freeze node + link order at first tick (after LLDP discovery). Once frozen,
  // additions/removals don't change index assignments; that's OK for static
  // topologies and matches the plan's "stable index → link mapping" guarantee.
  // Node order is what the Python GNN uses to map dpid → 0..N-1 when building
  // edge_index, so it must be frozen alongside the link order.
  bool firstTick = (m_mlTick == 0);
  if (firstTick) {
    for (const auto& kv : m_switchMap) {
      m_mlNodeOrder.push_back(kv.first);
    }
    for (const auto& link : m_topology.GetAllLinks()) {
      uint64_t a = link.src_dpid, b = link.dst_dpid;
      if (a > b) std::swap(a, b);
      m_mlLinkOrder.push_back({a, b});
    }
    NS_LOG_INFO("[ML] Frozen node order: " << m_mlNodeOrder.size() << " switches"
                << ", link order: " << m_mlLinkOrder.size() << " links");
    MlSendHello();
  }

  // Reward from previous tick's state vs current state.
  if (m_mlHavePrevObs) {
    m_mlPrevReward = ComputeMlReward();
  }

  std::string req = BuildMlStatePayload();

  if (m_mlSock) {
    try {
      zmq::message_t request(req.size());
      std::memcpy(request.data(), req.data(), req.size());
      auto sres = m_mlSock->send(request, zmq::send_flags::none);
      if (!sres) {
        NS_LOG_WARN("[ML] ZMQ send timed out — skipping action this tick");
      } else {
        zmq::message_t reply;
        auto rres = m_mlSock->recv(reply, zmq::recv_flags::none);
        if (!rres) {
          NS_LOG_WARN("[ML] ZMQ recv timed out — skipping action this tick");
        } else {
        // Minimal JSON parse: scan for "action":[ ... ] and split on commas.
        std::string body(static_cast<const char*>(reply.data()), reply.size());
        std::vector<double> action;
        auto k = body.find("\"action\"");
        if (k != std::string::npos) {
          auto lb = body.find('[', k);
          auto rb = body.find(']', lb);
          if (lb != std::string::npos && rb != std::string::npos && rb > lb) {
            std::string arr = body.substr(lb + 1, rb - lb - 1);
            std::stringstream ss(arr);
            std::string tok;
            while (std::getline(ss, tok, ',')) {
              try { action.push_back(std::stod(tok)); } catch (...) {}
            }
          }
        }
        if (!action.empty()) {
          ApplyDeltaCosts(action);
        }
        }  // end inner else (recv ok)
      }    // end outer else (send ok)
    } catch (const std::exception& e) {
      NS_LOG_WARN("[ML] ZMQ exchange failed: " << e.what());
    }
  }

  // Snapshot for next tick's reward computation.
  m_mlPrevObs       = m_switchObs;
  m_mlHavePrevObs   = true;
  m_mlTick++;

  // ---- Safety clamp & rollback (stubbed; flip on later). ----
  // Trigger condition: mean L_bps triples within 3 ticks → ResetLinkCosts()
  // + skip 5 ticks. Easy to wire when we have a stable baseline.
#if 0
  MaybeRollback();
#endif

  Simulator::Schedule(Seconds(m_ml.interval_s),
                      &ZmqOpenFlowController::MlTick, this);
}

}  // namespace ns3
