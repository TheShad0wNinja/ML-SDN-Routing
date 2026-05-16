#include "zmq-openflow-controller.h"

#include <arpa/inet.h>
#include <sys/stat.h>

#include <algorithm>
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
  m_portSpeeds.clear();
  m_switchObs.clear();
  m_hostAnnotations.clear();
  m_switchEnergyModel.clear();
  m_switchResidualEnergy.clear();
  m_hostIpMap.clear();
  m_ipToMac.clear();
  m_spanningTree.clear();
  m_lldpSendNs.clear();
  m_topology = Topology();

  if (m_socket) {
    try {
      m_socket->close();
    } catch (...) {
    }
  }
  if (m_zmqContext) {
    try {
      m_zmqContext->close();
    } catch (...) {
    }
  }

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

void ZmqOpenFlowController::SetStatsInterval(double seconds) {
  m_statsIntervalS = seconds;
}

void ZmqOpenFlowController::SetMlConfig(const MlConfig& cfg) {
  m_ml = cfg;
  if (!m_ml.enabled) {
    return;
  }

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
  cmd << "flow-mod cmd=add,table=0,prio=100,idle=30 eth_dst=" << macStr
      << " apply:output=" << outPort;
  DpctlExecute(dpid, cmd.str());
  m_installedFlows[dpid][dstMac] = outPort;
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
// UDP flows actually follow the ML-adjusted costs instead of sitting on the
// path that was current when the flow was first installed (idle=30 means
// continuous flows never expire on their own).
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

  bool changed = m_topology.AddLink(chassis_id, port_id, dpid, inPort, delayMs, 1.0);
  if (changed) {
    RebuildSpanningTree();
  }
  NS_LOG_INFO("[TOPO] Link: " << chassis_id << ":" << port_id << " <-> " << dpid
                              << ":" << inPort << " delay=" << delayMs << "ms cost=1.0");

  // Flush MAC entries learned on now-confirmed switch-link ports
  for (auto mit = m_macToLoc.begin(); mit != m_macToLoc.end();) {
    if (m_topology.IsSwitchLinkPort(mit->second.first, mit->second.second))
      mit = m_macToLoc.erase(mit);
    else
      ++mit;
  }

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
  NS_LOG_INFO("[PORT-DESC] Switch " << dpid << ": " << reply->stats_num << " ports");
  for (size_t i = 0; i < reply->stats_num; ++i) {
    uint32_t pno = reply->stats[i]->port_no;
    if (pno < OFPP_MAX) {
      m_switchPorts[dpid].insert(pno);
      uint32_t kbps = reply->stats[i]->curr_speed;
      m_portSpeeds[dpid][pno] = kbps;
      m_portStats[dpid][pno].speed_kbps = kbps;
      NS_LOG_INFO("[PORT-DESC]   port " << pno
                  << " name=" << (reply->stats[i]->name ? reply->stats[i]->name : "?")
                  << " speed=" << kbps << " kbps");
    }
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
  }
  return OFSwitch13Controller::HandleMultipartReply(msg, swtch, xid);
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

    // Compute instantaneous bit rates
    double dt = (ps.prev_time_s > 0) ? (nowSec - ps.prev_time_s) : 0;
    if (dt > 0) {
      ps.rx_rate_bps = static_cast<double>(s->rx_bytes - ps.prev_rx_bytes) * 8.0 / dt;
      ps.tx_rate_bps = static_cast<double>(s->tx_bytes - ps.prev_tx_bytes) * 8.0 / dt;
      ps.rx_rate_bps = std::max(0.0, ps.rx_rate_bps);
      ps.tx_rate_bps = std::max(0.0, ps.tx_rate_bps);
    }

    // Update snapshot for next interval
    ps.prev_rx_bytes = s->rx_bytes;
    ps.prev_tx_bytes = s->tx_bytes;
    ps.prev_time_s   = nowSec;

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

    // Backfill speed from PORT_DESC cache
    auto spIt = m_portSpeeds.find(dpid);
    if (spIt != m_portSpeeds.end()) {
      auto pIt = spIt->second.find(pno);
      if (pIt != spIt->second.end()) ps.speed_kbps = pIt->second;
    }

    // Stash the egress capacity into Topology so the ML state payload can
    // emit per-link absolute headroom even on ticks where stats arrive late.
    if (ps.speed_kbps > 0) {
      auto peerDpid = m_topology.GetPeerDpid(dpid, pno);
      if (peerDpid) {
        m_topology.SetLinkCapacityBps(dpid, *peerDpid,
                                       static_cast<double>(ps.speed_kbps) * 1000.0);
      }
    }

    NS_LOG_INFO("[PORT-STATS]   port " << pno
                << " rx=" << s->rx_bytes << "B tx=" << s->tx_bytes
                << "B rx_rate=" << ps.rx_rate_bps / 1e6 << " Mbps"
                << " tx_rate=" << ps.tx_rate_bps / 1e6 << " Mbps");
  }

  ComputeSwitchObservations(dpid);
}

void ZmqOpenFlowController::ComputeSwitchObservations(uint64_t dpid) {
  SwitchObservation obs;
  obs.K = 64; // OFSwitch13 default FIFO queue depth

  auto psIt = m_portStats.find(dpid);
  if (psIt == m_portStats.end()) {
    m_switchObs[dpid] = obs;
    return;
  }

  double dropRateBps = 0;
  double minSpeedBps = 0; // bottleneck link speed across all ports
  for (const auto& [pno, ps] : psIt->second) {
    obs.lambda_bps += ps.rx_rate_bps; // all incoming traffic, not just host ports
    double portSpeedBps = static_cast<double>(ps.speed_kbps) * 1000.0;
    if (portSpeedBps > 0 && (minSpeedBps == 0 || portSpeedBps < minSpeedBps))
      minSpeedBps = portSpeedBps;
    // Loss rate summed over all ports
    dropRateBps += (ps.rx_rate_bps > 0 && ps.rx_dropped > 0)
                       ? ps.rx_rate_bps * (static_cast<double>(ps.rx_dropped) /
                                           std::max((uint64_t)1, ps.rx_packets))
                       : 0;
  }
  obs.mu_max_bps = minSpeedBps;

  obs.L_bps = dropRateBps;
  obs.rbw_bps = std::max(0.0, obs.mu_max_bps - obs.lambda_bps);

  if (obs.mu_max_bps > 0) {
    obs.rho = obs.lambda_bps / obs.mu_max_bps;
    obs.rho = std::min(obs.rho, 0.9999); // cap for stable M/M/1
  }

  // M/M/1/K queue model (finite buffer, more accurate near saturation)
  if (obs.rho > 0 && obs.mu_max_bps > 0) {
    double rho = obs.rho;
    double K   = obs.K;
    double p0, pK;
    if (std::abs(rho - 1.0) < 1e-9) {
      p0 = 1.0 / (K + 1.0);
    } else {
      p0 = (1.0 - rho) / (1.0 - std::pow(rho, K + 1.0));
    }
    pK = p0 * std::pow(rho, K);
    obs.p_loss = std::min(pK, 1.0);

    if (std::abs(rho - 1.0) < 1e-9) {
      obs.N = K / 2.0;
    } else {
      obs.N = rho / (1.0 - rho)
              - (K + 1.0) * std::pow(rho, K + 1.0) / (1.0 - std::pow(rho, K + 1.0));
    }
    obs.N = std::max(0.0, obs.N);
    if (obs.lambda_bps > 0)
      obs.d_ms = (obs.N / (obs.lambda_bps / 8.0)) * 1000.0;
  }

  // Drain switch forwarding energy based on total TX rate across all ports
  auto emIt = m_switchEnergyModel.find(dpid);
  if (emIt != m_switchEnergyModel.end() && emIt->second.initial_energy_j >= 0) {
    double totalTxBps = 0.0;
    if (psIt != m_portStats.end())
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
              << " λ=" << obs.lambda_bps / 1e6 << "Mbps"
              << " μ=" << obs.mu_max_bps / 1e6 << "Mbps"
              << " ρ=" << obs.rho
              << " N=" << obs.N
              << " d=" << obs.d_ms << "ms"
              << " P_loss=" << obs.p_loss
              << " RBW=" << obs.rbw_bps / 1e6 << "Mbps"
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

  std::string output_file = state_dir + "/sdn_state.json";

  std::ostringstream json;
  json << std::fixed << std::setprecision(3);
  json << "{\n";

  auto now = std::time(nullptr);
  json << "  \"timestamp\": " << now << ",\n";

  json << "  \"controller\": {\n";
  json << "    \"id\": \"sdn-controller\",\n";
  json << "    \"label\": \"SDN Controller\",\n";
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

  // Per-switch DDPG observation vectors
  json << "  \"switch_observations\": {";
  first = true;
  for (const auto& kv : m_switchObs) {
    if (!first) json << ", ";
    const SwitchObservation& o = kv.second;
    json << "\n    \"" << kv.first << "\": {\n";
    json << "      \"lambda_bps\": "  << o.lambda_bps << ",\n";
    json << "      \"mu_max_bps\": "  << o.mu_max_bps << ",\n";
    json << "      \"rho\": "         << o.rho        << ",\n";
    json << "      \"K\": "           << o.K          << ",\n";
    json << "      \"N\": "           << o.N          << ",\n";
    json << "      \"d_ms\": "        << o.d_ms       << ",\n";
    json << "      \"p_loss\": "      << o.p_loss     << ",\n";
    json << "      \"L_bps\": "       << o.L_bps      << ",\n";
    json << "      \"rbw_bps\": "     << o.rbw_bps    << ",\n";
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

  // Global μ_max — maximum per-switch mu_max_bps (ATVM normalisation constant)
  double muMaxGlobal = 0;
  for (const auto& kv : m_switchObs)
    muMaxGlobal = std::max(muMaxGlobal, kv.second.mu_max_bps);
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

  // state_dim = 6 features per switch * |switches|
  size_t stateDim = 6 * m_switchMap.size();
  // action_dim = one ΔW per (deduplicated) link
  size_t actionDim = m_mlLinkOrder.size();

  std::ostringstream hello;
  hello << "{\"cmd\":\"hello\","
        << "\"state_dim\":" << stateDim << ","
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
    NS_LOG_INFO("[ML] hello ack: state_dim=" << stateDim
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
    << "\"state\":{";

  // ---- per-switch features ----
  s << "\"per_switch\":[";
  bool firstSw = true;
  for (const auto& [dpid, sw] : m_switchMap) {
    SwitchObservation obs = m_switchObs.count(dpid) ? m_switchObs.at(dpid)
                                                    : SwitchObservation{};
    const auto& em = m_switchEnergyModel.count(dpid)
                        ? m_switchEnergyModel.at(dpid)
                        : SwitchEnergyModel{};
    double energyFrac = (em.initial_energy_j > 0 && obs.residual_energy_j >= 0)
                            ? (obs.residual_energy_j / em.initial_energy_j)
                            : 1.0;
    double rttNs = m_echoRttNs.count(dpid) ? m_echoRttNs.at(dpid) : 0;

    if (!firstSw) s << ",";
    s << "{\"dpid\":" << dpid
      << ",\"rho\":" << obs.rho
      << ",\"d_ms\":" << obs.d_ms
      << ",\"p_loss\":" << obs.p_loss
      << ",\"residual_energy_frac\":" << energyFrac
      << ",\"echo_rtt_ns\":" << rttNs
      << ",\"mu_max_bps\":" << obs.mu_max_bps
      << "}";
    firstSw = false;
  }
  s << "],";

  // ---- per-link features ----
  s << "\"per_link\":[";
  bool firstLink = true;
  for (const auto& [a, b] : m_mlLinkOrder) {
    double cost     = m_topology.GetLinkCost(a, b);
    double baseCost = m_topology.GetBaseLinkCost(a, b);
    double cap      = std::max(m_topology.GetLinkCapacityBps(a, b),
                               m_topology.GetLinkCapacityBps(b, a));
    double delayMs  = m_topology.GetLinkDelay(a, b);

    // ATVM: tx_bps from src→dst egress port
    double txBps = 0.0;
    auto psIt = m_portStats.find(a);
    if (psIt != m_portStats.end()) {
      for (const auto& [pno, ps] : psIt->second) {
        auto peer = m_topology.GetPeerDpid(a, pno);
        if (peer && *peer == b) {
          txBps += ps.tx_rate_bps;
        }
      }
    }
    double util = (cap > 0) ? (txBps / cap) : 0.0;

    if (!firstLink) s << ",";
    s << "{\"src\":" << a << ",\"dst\":" << b
      << ",\"tx_bps\":" << txBps
      << ",\"capacity_bps\":" << cap
      << ",\"utilization\":" << util
      << ",\"cost\":" << cost
      << ",\"base_cost\":" << baseCost
      << ",\"delay_ms\":" << delayMs
      << "}";
    firstLink = false;
  }
  s << "]}}";
  return s.str();
}

double ZmqOpenFlowController::ComputeMlReward() {
  if (!m_mlHavePrevObs || m_switchObs.empty()) return 0.0;

  // Mean delay improvement (lower is better → reward positive when curr < prev).
  double prevDelay = 0.0, currDelay = 0.0;
  uint32_t prevN = 0, currN = 0;
  for (const auto& [dpid, o] : m_mlPrevObs) {
    if (o.d_ms > 0) { prevDelay += o.d_ms; ++prevN; }
  }
  for (const auto& [dpid, o] : m_switchObs) {
    if (o.d_ms > 0) { currDelay += o.d_ms; ++currN; }
  }
  double meanPrev = (prevN > 0) ? (prevDelay / prevN) : 0.0;
  double meanCurr = (currN > 0) ? (currDelay / currN) : 0.0;

  // Loss penalty: total dropped-bytes rate.
  double prevLoss = 0.0, currLoss = 0.0;
  for (const auto& [dpid, o] : m_mlPrevObs)   prevLoss += o.L_bps;
  for (const auto& [dpid, o] : m_switchObs)   currLoss += o.L_bps;

  // Energy-efficiency term: forwarded bytes weighted by residual energy fraction.
  double energyTerm = 0.0;
  for (const auto& [a, b] : m_mlLinkOrder) {
    auto psIt = m_portStats.find(a);
    if (psIt == m_portStats.end()) continue;
    double txBps = 0.0;
    for (const auto& [pno, ps] : psIt->second) {
      auto peer = m_topology.GetPeerDpid(a, pno);
      if (peer && *peer == b) txBps += ps.tx_rate_bps;
    }
    auto emIt = m_switchEnergyModel.find(a);
    if (emIt == m_switchEnergyModel.end()) continue;
    double frac = 1.0;
    if (emIt->second.initial_energy_j > 0) {
      auto reIt = m_switchResidualEnergy.find(a);
      if (reIt != m_switchResidualEnergy.end())
        frac = reIt->second / emIt->second.initial_energy_j;
    }
    energyTerm += txBps * emIt->second.energy_per_byte_j * frac;
  }

  double R = m_ml.reward_alpha * (meanPrev - meanCurr)
           - m_ml.reward_beta  * (currLoss - prevLoss) / 1e6  // bring loss into a comparable unit
           + m_ml.reward_gamma * energyTerm / 1e6;
  return R;
}

void ZmqOpenFlowController::ApplyDeltaCosts(const std::vector<double>& deltas) {
  size_t n = std::min(deltas.size(), m_mlLinkOrder.size());
  bool anyChanged = false;
  for (size_t i = 0; i < n; ++i) {
    double d = deltas[i];
    if (d > m_ml.action_scale)        d = m_ml.action_scale;
    else if (d < -m_ml.action_scale)  d = -m_ml.action_scale;

    auto [a, b] = m_mlLinkOrder[i];
    double base = m_topology.GetBaseLinkCost(a, b);
    if (base <= 0.0) continue;
    double newCost = base * (1.0 + d);
    double oldCost = m_topology.GetLinkCost(a, b);
    if (std::abs(newCost - oldCost) > 1e-9) {
      m_topology.SetLinkCost(a, b, newCost);
      anyChanged = true;
    }
  }
  // Critical: existing flow entries are keyed only on eth_dst with idle=30s,
  // so continuous UDP flows never re-PacketIn and never see the new costs.
  // Walk the routing table now and rewrite next-hop ports where Dijkstra has
  // moved.
  if (anyChanged) RecomputeAllRoutes();
}

void ZmqOpenFlowController::MlTick() {
  if (!m_ml.enabled) return;

  // Refresh observations: trigger a fresh port-stats sweep so curr obs reflects
  // the most recent interval. Stats arrive asynchronously, but the ML payload
  // is built from whatever m_switchObs currently holds — that's the prior
  // interval's view, which is exactly what the reward needs.
  for (const auto& kv : m_switchMap) {
    struct ofl_msg_header* req = BuildPortStatsRequest();
    if (req) {
      SendToSwitch(kv.second, req, 0);
      ofl_msg_free(req, nullptr);
    }
  }

  // Freeze link order at first tick (after LLDP discovery). Once frozen, link
  // additions/removals don't grow the action vector; that's OK for static
  // topologies and matches the plan's "stable index → link mapping" guarantee.
  bool firstTick = (m_mlTick == 0);
  if (firstTick) {
    for (const auto& link : m_topology.GetAllLinks()) {
      uint64_t a = link.src_dpid, b = link.dst_dpid;
      if (a > b) std::swap(a, b);
      m_mlLinkOrder.push_back({a, b});
    }
    NS_LOG_INFO("[ML] Frozen link order: " << m_mlLinkOrder.size() << " links");
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
