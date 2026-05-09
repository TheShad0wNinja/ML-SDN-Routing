#include "zmq-openflow-controller.h"

#include "openflow_builders.h"

#include "ns3/log.h"
#include "ns3/simulator.h"

#include <arpa/inet.h>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace ns3 {

NS_LOG_COMPONENT_DEFINE("ZmqOpenFlowController");

TypeId
ZmqOpenFlowController::GetTypeId() {
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

void ZmqOpenFlowController::DoDispose() {
    WriteStateToJson();
    m_switchMap.clear();
    m_macToLoc.clear();
    m_switchPorts.clear();
    m_portStats.clear();
    m_hostIpMap.clear();
    m_lldpSendNs.clear();
    m_topology = Topology();

    OFSwitch13Controller::DoDispose();
}

void ZmqOpenFlowController::StartApplication() {
    // Start LLDP early so topology is known before pings at t=2s
    Simulator::Schedule(Seconds(0.5), &ZmqOpenFlowController::TriggerLldp, this);
    Simulator::Schedule(Seconds(1.0), &ZmqOpenFlowController::TriggerEcho, this);
    Simulator::Schedule(Seconds(60), &ZmqOpenFlowController::TriggerStats, this);

    OFSwitch13Controller::StartApplication();
}

void ZmqOpenFlowController::StopApplication() {
    if (m_socket) {
        try { m_socket->close(); }
        catch (...) {}
    }
    if (m_zmqContext) {
        try { m_zmqContext->close(); }
        catch (...) {}
    }
    OFSwitch13Controller::StopApplication();
}

void
ZmqOpenFlowController::HandshakeSuccessful(Ptr<const RemoteSwitch> swtch) {
    NS_LOG_FUNCTION(this << swtch);

    uint64_t swDpId = swtch->GetDpId();
    m_switchMap[swDpId] = swtch;
    m_switchPorts[swDpId] = std::unordered_set<uint32_t>();

    DpctlExecute(swDpId, "flow-mod cmd=add,table=0,prio=0 apply:output=ctrl:128");
    DpctlExecute(swDpId, "set-config miss=128");
}

// ------------------------------------------------------------------
//  Helper: build and send a single-action OFPT_PACKET_OUT
// ------------------------------------------------------------------
void
ZmqOpenFlowController::SendPacketOut(Ptr<const RemoteSwitch> swtch,
    uint32_t inPort,
    uint32_t bufferId,
    const uint8_t* data,
    size_t dataLen,
    uint32_t outPort) {
    struct ofl_msg_packet_out* po = (struct ofl_msg_packet_out*)malloc(sizeof(*po));
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
    po->actions = (struct ofl_action_header**)malloc(sizeof(struct ofl_action_header*));
    po->actions[0] = (struct ofl_action_header*)a;
    SendToSwitch(swtch, (struct ofl_msg_header*)po, 0);
    free(po->actions);
    free(a);
    if (po->data)
        free(po->data);
    free(po);
}

// ------------------------------------------------------------------
//  Install a MAC-destination flow with idle timeout
// ------------------------------------------------------------------
void
ZmqOpenFlowController::InstallFlow(uint64_t dpid, uint64_t dstMac, uint32_t outPort) {
    std::string macStr = FormatMac(dstMac);
    std::ostringstream cmd;
    cmd << "flow-mod cmd=add,table=0,prio=100,idle=30 eth_dst="
        << macStr << " apply:output=" << outPort;
    DpctlExecute(dpid, cmd.str());
}

// ------------------------------------------------------------------
//  Format a 32-bit IPv4 address (host-byte-order) as dotted-decimal
// ------------------------------------------------------------------
std::string
ZmqOpenFlowController::FormatIp(uint32_t ip) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
        (ip >> 24) & 0xFF,
        (ip >> 16) & 0xFF,
        (ip >> 8) & 0xFF,
        (ip) & 0xFF);
    return buf;
}

// ------------------------------------------------------------------
//  LLDP packet handler (extracted from HandlePacketIn)
// ------------------------------------------------------------------
void
ZmqOpenFlowController::HandleLldpPacket(uint64_t dpid, uint32_t inPort,
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
        if (tlv_type == 0)
            break;
        if (off + tlen > len)
            break;
        const uint8_t* val = data + off;

        if (tlv_type == 1 && tlen > 1) {
            try { chassis_id = std::stoull(std::string((const char*)(val + 1), tlen - 1)); }
            catch (...) {}
        }
        else if (tlv_type == 2 && tlen > 1) {
            try { port_id = static_cast<uint32_t>(std::stoul(std::string((const char*)(val + 1), tlen - 1))); }
            catch (...) {}
        }
        else if (tlv_type == 9 && tlen == 8) {
            // LatencyTag: 8 bytes big-endian timestamp
            for (int i = 0; i < 8; ++i)
                sendNs = (sendNs << 8) | val[i];
        }
        off += tlen;
    }

    if (chassis_id == 0 || port_id == 0)
        return;

    double costMs = 1.0;
    if (sendNs != 0) {
        uint64_t nowNs = Simulator::Now().GetNanoSeconds();
        if (nowNs > sendNs) {
            uint64_t totalNs    = nowNs - sendNs;
            uint64_t halfRttA   = m_echoRttNs.count(chassis_id) ? m_echoRttNs.at(chassis_id) / 2 : 0;
            uint64_t halfRttB   = m_echoRttNs.count(dpid)       ? m_echoRttNs.at(dpid)       / 2 : 0;
            uint64_t correction = halfRttA + halfRttB;
            uint64_t linkNs     = (totalNs > correction) ? totalNs - correction : totalNs;
            costMs = std::max(0.001, static_cast<double>(linkNs) / 1e6);
        }
    }

    m_topology.AddLink(chassis_id, port_id, dpid, inPort, costMs);
    NS_LOG_INFO("[TOPO] Link: " << chassis_id << ":" << port_id
        << " <-> " << dpid << ":" << inPort
        << " cost=" << costMs << "ms");

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
void
ZmqOpenFlowController::HandleArpPacket(const uint8_t* data, size_t len) {
    // Ethernet (14) + ARP (28) = 42 bytes minimum
    if (len < 42)
        return;

    const uint8_t* arp = data + 14;
    uint16_t hw_type = (uint16_t)arp[0] << 8 | arp[1];
    uint16_t proto = (uint16_t)arp[2] << 8 | arp[3];
    uint8_t  hw_len = arp[4];
    uint8_t  proto_len = arp[5];

    if (hw_type != 1 || proto != 0x0800 || hw_len != 6 || proto_len != 4)
        return;

    // Sender MAC: bytes 8-13 of ARP header (offset 22 from Ethernet start)
    uint64_t senderMac = 0;
    for (int i = 0; i < 6; ++i)
        senderMac = (senderMac << 8) | arp[8 + i];

    // Sender IP: bytes 14-17 of ARP header (offset 28 from Ethernet start)
    uint32_t senderIp = ((uint32_t)arp[14] << 24) |
        ((uint32_t)arp[15] << 16) |
        ((uint32_t)arp[16] << 8) |
        (uint32_t)arp[17];

    if (senderMac != 0 && senderIp != 0) {
        m_hostIpMap[senderMac] = senderIp;
    }
}

// ------------------------------------------------------------------
//  ForwardPacket — lookup dst and route or flood
// ------------------------------------------------------------------
void
ZmqOpenFlowController::ForwardPacket(Ptr<const RemoteSwitch> swtch,
    uint32_t inPort,
    struct ofl_msg_packet_in* msg,
    uint64_t srcMac, uint64_t dstMac) {
    uint64_t dpid = swtch->GetDpId();

    auto dstIt = m_macToLoc.find(dstMac);
    if (dstIt == m_macToLoc.end()) {
        SendPacketOut(swtch, inPort, msg->buffer_id, msg->data, msg->data_length, OFPP_FLOOD);
        return;
    }

    uint64_t dst_dpid = dstIt->second.first;
    uint32_t dst_port = dstIt->second.second;

    if (dst_dpid == dpid) {
        InstallFlow(dpid, dstMac, dst_port);
        SendPacketOut(swtch, inPort, msg->buffer_id, msg->data, msg->data_length, dst_port);
        return;
    }

    auto opt_path = m_topology.ShortestPath(dpid, dst_dpid);
    if (!opt_path || opt_path->size() < 2) {
        SendPacketOut(swtch, inPort, msg->buffer_id, msg->data, msg->data_length, OFPP_FLOOD);
        return;
    }

    const std::vector<uint64_t>& path = *opt_path;
    for (size_t i = 0; i + 1 < path.size(); ++i) {
        auto outp_opt = m_topology.GetOutPort(path[i], path[i + 1]);
        if (outp_opt)
            InstallFlow(path[i], dstMac, *outp_opt);
    }
    InstallFlow(dst_dpid, dstMac, dst_port);

    auto first_out = m_topology.GetOutPort(path[0], path[1]);
    if (first_out)
        SendPacketOut(swtch, inPort, msg->buffer_id, msg->data, msg->data_length, *first_out);
}

// ------------------------------------------------------------------
//  OpenFlow event handler
// ------------------------------------------------------------------
ofl_err
ZmqOpenFlowController::HandlePacketIn(struct ofl_msg_packet_in* msg,
    Ptr<const RemoteSwitch> swtch,
    uint32_t xid) {
    uint64_t dpid = swtch->GetDpId();

    uint32_t inPort = 0;
    struct ofl_match_tlv* input = oxm_match_lookup(OXM_OF_IN_PORT, (struct ofl_match*)msg->match);
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
        }
    }

    struct ofl_match_tlv* ethSrc = oxm_match_lookup(OXM_OF_ETH_SRC, (struct ofl_match*)msg->match);
    struct ofl_match_tlv* ethDst = oxm_match_lookup(OXM_OF_ETH_DST, (struct ofl_match*)msg->match);
    if (!ethSrc || !ethDst || !ethSrc->value || !ethDst->value) {
        SendPacketOut(swtch, inPort, msg->buffer_id, msg->data, msg->data_length, OFPP_FLOOD);
        ofl_msg_free((struct ofl_msg_header*)msg, nullptr);
        return 0;
    }

    uint64_t src_mac = 0, dst_mac = 0;
    for (int i = 0; i < 6; ++i) {
        src_mac = (src_mac << 8) | (uint8_t)ethSrc->value[i];
        dst_mac = (dst_mac << 8) | (uint8_t)ethDst->value[i];
    }

    if (!m_topology.IsSwitchLinkPort(dpid, inPort)) {
        m_macToLoc[src_mac] = { dpid, inPort };
    }

    ForwardPacket(swtch, inPort, msg, src_mac, dst_mac);

    ofl_msg_free((struct ofl_msg_header*)msg, nullptr);
    return 0;
}

ofl_err
ZmqOpenFlowController::HandlePortStatus(struct ofl_msg_port_status* msg,
    Ptr<const RemoteSwitch> swtch,
    uint32_t xid) {
    uint64_t dpid = swtch->GetDpId();

    if (msg && msg->desc) {
        uint32_t port_no = msg->desc->port_no;

        if (msg->reason == OFPPR_DELETE) {
            auto it = m_switchPorts.find(dpid);
            if (it != m_switchPorts.end())
                it->second.erase(port_no);

            m_topology.RemovePort(dpid, port_no);

            for (auto mit = m_macToLoc.begin(); mit != m_macToLoc.end();) {
                if (mit->second.first == dpid && mit->second.second == port_no)
                    mit = m_macToLoc.erase(mit);
                else
                    ++mit;
            }
            // WriteStateToJson();
        }
        else if (msg->reason == OFPPR_ADD) {
            m_switchPorts[dpid].insert(port_no);
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

ofl_err
ZmqOpenFlowController::HandleMultipartReply(struct ofl_msg_multipart_reply_header* msg,
    Ptr<const RemoteSwitch> swtch,
    uint32_t xid) {
    return OFSwitch13Controller::HandleMultipartReply(msg, swtch, xid);
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
            if (hasLinks
                && m_switchPorts[src_dpid].count(p)
                && !m_topology.IsSwitchLinkPort(src_dpid, p)) {
                continue;
            }

            // Stagger ports by 100 µs
            Time portOffset = MicroSeconds(p * 100);
            Simulator::Schedule(baseOffset + portOffset, &ZmqOpenFlowController::SendSingleLldp, this, sw, src_dpid, p);
        }
        switchIdx++;
    }

    // 5 s until topology is complete, then back off to 30 s
    double nextLldp = hasLinks ? 30.0 : 5.0;
    Simulator::Schedule(Seconds(nextLldp), &ZmqOpenFlowController::TriggerLldp, this);
}

void ZmqOpenFlowController::SendSingleLldp(Ptr<const RemoteSwitch> swtch, uint64_t dpid, uint32_t port) {
    uint64_t nowNs = Simulator::Now().GetNanoSeconds();
    m_lldpSendNs[dpid][port] = nowNs;

    auto frame = BuildLldpFrame(dpid, port, nowNs);
    auto* po = BuildLldpPacketOut(port, frame.data(), frame.size());
    if (po) {
        SendToSwitch(swtch, po, 0);
        ofl_msg_free(po, nullptr);
    }
}

void
ZmqOpenFlowController::TriggerEcho() {
    for (const auto& [dpid, swtch] : m_switchMap) {
        auto& miss = m_echoMissCount[dpid];
        if (miss >= kEchoMaxMissed) {
            NS_LOG_WARN("[ECHO] Switch " << dpid << " missed " << miss
                        << " consecutive echo replies — link may be down");
        }
        m_echoSendNs[dpid] = Simulator::Now().GetNanoSeconds();
        miss++;
        SendEchoRequest(swtch, 0);
    }
    Simulator::Schedule(Seconds(kEchoIntervalSec),
                        &ZmqOpenFlowController::TriggerEcho, this);
}

ofl_err
ZmqOpenFlowController::HandleEchoReply(struct ofl_msg_echo* msg,
                                       Ptr<const RemoteSwitch> swtch,
                                       uint32_t xid)
{
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

void
ZmqOpenFlowController::TriggerStats() {
    for (const auto& kv : m_switchMap) {
        struct ofl_msg_header* req = BuildPortStatsRequest();
        if (req) {
            SendToSwitch(kv.second, req, 0);
            ofl_msg_free(req, nullptr);
        }
    }
    Simulator::Schedule(Seconds(60), &ZmqOpenFlowController::TriggerStats, this);
}

// ------------------------------------------------------------------
//  State serialization
// ------------------------------------------------------------------
void
ZmqOpenFlowController::WriteStateToJson() {
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

    // Switches: array of {dpid, name} objects (name = "S{dpid-1}")
    json << "  \"switches\": [";
    bool first = true;
    for (const auto& kv : m_switchMap) {
        if (!first) json << ", ";
        json << "\n    {\"dpid\": " << kv.first
            << ", \"name\": \"S" << (kv.first - 1) << "\"}";
        first = false;
    }
    if (!first) json << "\n  ";
    json << "],\n";

    // Hosts with colon-separated MAC and optional IP
    json << "  \"hosts\": [";
    first = true;
    for (const auto& kv : m_macToLoc) {
        uint64_t mac = kv.first;
        uint64_t dpid = kv.second.first;
        uint32_t port = kv.second.second;

        if (!first) json << ", ";
        json << "\n    {\n";
        json << "      \"mac\": \"" << FormatMac(mac) << "\",\n";
        json << "      \"dpid\": " << dpid << ",\n";
        json << "      \"port\": " << port;

        auto ipIt = m_hostIpMap.find(mac);
        if (ipIt != m_hostIpMap.end()) {
            json << ",\n      \"ip\": \"" << FormatIp(ipIt->second) << "\"";
        }
        json << "\n    }";
        first = false;
    }
    if (!first) json << "\n  ";
    json << "],\n";

    // Links with cost_ms
    json << "  \"links\": [";
    first = true;
    for (const auto& link : m_topology.GetAllLinks()) {
        if (!first) json << ", ";
        json << "\n    {\n";
        json << "      \"src_dpid\": " << link.src_dpid << ",\n";
        json << "      \"src_port\": " << link.src_port << ",\n";
        json << "      \"dst_dpid\": " << link.dst_dpid << ",\n";
        json << "      \"dst_port\": " << link.dst_port << ",\n";
        json << "      \"cost_ms\": " << link.cost_ms << "\n";
        json << "    }";
        first = false;
    }
    if (!first) json << "\n  ";
    json << "],\n";

    // Port statistics
    json << "  \"stats\": {";
    first = true;
    for (const auto& sw_kv : m_portStats) {
        if (!first) json << ", ";
        json << "\n    \"" << sw_kv.first << "\": {";
        bool first_port = true;
        for (const auto& port_kv : sw_kv.second) {
            if (!first_port) json << ", ";
            json << "\n      \"" << port_kv.first << "\": {"
                << "\"rx_packets\": 0, \"tx_packets\": 0, "
                << "\"rx_bytes\": 0, \"tx_bytes\": " << port_kv.second << "}";
            first_port = false;
        }
        if (!first_port) json << "\n    ";
        json << "}";
        first = false;
    }
    if (!first) json << "\n  ";
    json << "},\n";

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
    }
    else {
        NS_LOG_WARN("Failed to open " << output_file);
    }
}

} // namespace ns3
