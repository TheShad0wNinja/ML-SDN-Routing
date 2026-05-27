#include "openflow_builders.h"

#include <arpa/inet.h>

#include <cstdlib>
#include <cstring>
#include <string>

#include "ns3/ofswitch13-module.h"

namespace ns3 {

std::string FormatMac(uint64_t mac) {
  char buf[18];
  snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x",
           (unsigned)((mac >> 40) & 0xFF), (unsigned)((mac >> 32) & 0xFF),
           (unsigned)((mac >> 24) & 0xFF), (unsigned)((mac >> 16) & 0xFF),
           (unsigned)((mac >> 8) & 0xFF), (unsigned)(mac & 0xFF));
  return buf;
}

std::vector<uint8_t> BuildLldpFrame(uint64_t chassisId, uint32_t portId,
                                    uint64_t timestampNs) {
  std::vector<uint8_t> frame;
  frame.reserve(128);

  // --- Ethernet header -------------------------------------------------
  frame.insert(frame.end(), LldpTlv::MulticastDst,
               LldpTlv::MulticastDst + OFP_ETH_ALEN);

  // Source MAC: lower 48 bits of chassis ID (deterministic, unique per switch)
  uint8_t srcMac[OFP_ETH_ALEN] = {0};
  uint64_t id = chassisId;
  for (int i = OFP_ETH_ALEN - 1; i >= 0; --i) {
    srcMac[i] = static_cast<uint8_t>(id & 0xFF);
    id >>= 8;
  }
  frame.insert(frame.end(), srcMac, srcMac + OFP_ETH_ALEN);

  // EtherType: 0x88CC big-endian
  frame.push_back(static_cast<uint8_t>(LldpTlv::EtherType >> 8));
  frame.push_back(static_cast<uint8_t>(LldpTlv::EtherType & 0xFF));

  // --- TLV helper ------------------------------------------------------
  auto appendTlv = [&](LldpTlv::Type type, const std::vector<uint8_t>& value) {
    uint16_t hdr = LldpTlv::MakeHeader(type, value.size());
    frame.push_back(static_cast<uint8_t>(hdr >> 8));
    frame.push_back(static_cast<uint8_t>(hdr & 0xFF));
    frame.insert(frame.end(), value.begin(), value.end());
  };

  // Chassis ID
  std::string chassisStr = std::to_string(chassisId);
  std::vector<uint8_t> chassisValue;
  chassisValue.reserve(1 + chassisStr.size());
  chassisValue.push_back(
      static_cast<uint8_t>(LldpTlv::ChassisIdSubtype::LocallyAssigned));
  chassisValue.insert(chassisValue.end(), chassisStr.begin(), chassisStr.end());
  appendTlv(LldpTlv::Type::ChassisId, chassisValue);

  // Port ID
  std::string portStr = std::to_string(portId);
  std::vector<uint8_t> portValue;
  portValue.reserve(1 + portStr.size());
  portValue.push_back(
      static_cast<uint8_t>(LldpTlv::PortIdSubtype::LocallyAssigned));
  portValue.insert(portValue.end(), portStr.begin(), portStr.end());
  appendTlv(LldpTlv::Type::PortId, portValue);

  // TTL
  constexpr uint16_t kTtlSeconds = 120;
  std::vector<uint8_t> ttlValue = {static_cast<uint8_t>(kTtlSeconds >> 8),
                                   static_cast<uint8_t>(kTtlSeconds & 0xFF)};
  appendTlv(LldpTlv::Type::TimeToLive, ttlValue);

  // Optional latency timestamp (custom TLV type 9)
  if (timestampNs != 0) {
    std::vector<uint8_t> tsVal(8);
    for (int i = 7; i >= 0; --i) {
      tsVal[i] = static_cast<uint8_t>(timestampNs & 0xFF);
      timestampNs >>= 8;
    }
    appendTlv(LldpTlv::Type::LatencyTag, tsVal);
  }

  // End-of-LLDPDU
  appendTlv(LldpTlv::Type::EndOfLldpdu, {});

  return frame;
}

struct ofl_msg_header* BuildLldpPacketOut(uint32_t portNo, const uint8_t* frame,
                                          std::size_t frameLen) {
  struct ofl_msg_packet_out* msg =
      static_cast<struct ofl_msg_packet_out*>(std::malloc(sizeof(*msg)));
  if (!msg) {
    return nullptr;
  }
  std::memset(msg, 0, sizeof(*msg));

  msg->header.type = OFPT_PACKET_OUT;
  msg->buffer_id = OFP_NO_BUFFER;
  msg->in_port = OFPP_CONTROLLER;

  if (frame && frameLen > 0) {
    msg->data_length = frameLen;
    msg->data = static_cast<uint8_t*>(std::malloc(frameLen));
    if (!msg->data) {
      std::free(msg);
      return nullptr;
    }
    std::memcpy(msg->data, frame, frameLen);
  }

  struct ofl_action_output* action =
      static_cast<struct ofl_action_output*>(std::malloc(sizeof(*action)));
  if (!action) {
    if (msg->data) {
      std::free(msg->data);
    }
    std::free(msg);
    return nullptr;
  }

  action->header.type = OFPAT_OUTPUT;
  action->header.len = sizeof(*action);
  action->port = portNo;
  action->max_len = 0;

  msg->actions_num = 1;
  msg->actions = static_cast<struct ofl_action_header**>(
      std::malloc(sizeof(struct ofl_action_header*)));
  if (!msg->actions) {
    std::free(action);
    if (msg->data) {
      std::free(msg->data);
    }
    std::free(msg);
    return nullptr;
  }
  msg->actions[0] = reinterpret_cast<struct ofl_action_header*>(action);

  return reinterpret_cast<struct ofl_msg_header*>(msg);
}

struct ofl_msg_header* BuildPortStatsRequest() {
  struct ofl_msg_multipart_request_port* req =
      static_cast<struct ofl_msg_multipart_request_port*>(
          std::malloc(sizeof(*req)));
  if (!req) {
    return nullptr;
  }
  std::memset(req, 0, sizeof(*req));

  req->header.header.type = OFPT_MULTIPART_REQUEST;
  req->header.type = OFPMP_PORT_STATS;
  req->header.flags = 0;
  req->port_no = OFPP_ANY;

  return reinterpret_cast<struct ofl_msg_header*>(req);
}

struct ofl_msg_header* BuildPortDescRequest() {
  struct ofl_msg_multipart_request_header* req =
      static_cast<struct ofl_msg_multipart_request_header*>(
          std::malloc(sizeof(*req)));
  if (!req) return nullptr;
  std::memset(req, 0, sizeof(*req));
  req->header.type = OFPT_MULTIPART_REQUEST;
  req->type = OFPMP_PORT_DESC;
  req->flags = 0;
  return reinterpret_cast<struct ofl_msg_header*>(req);
}

struct ofl_msg_header* BuildQueueStatsRequest() {
  struct ofl_msg_multipart_request_queue* req =
      static_cast<struct ofl_msg_multipart_request_queue*>(
          std::malloc(sizeof(*req)));
  if (!req) return nullptr;
  std::memset(req, 0, sizeof(*req));
  req->header.header.type = OFPT_MULTIPART_REQUEST;
  req->header.type = OFPMP_QUEUE;
  req->header.flags = 0;
  req->port_no = OFPP_ANY;
  req->queue_id = OFPQ_ALL;
  return reinterpret_cast<struct ofl_msg_header*>(req);
}

// Build a 42-byte Ethernet+ARP reply frame (proxy ARP)
std::array<uint8_t, 60> BuildArpReply(uint64_t targetMac, uint32_t targetIp,
                                      uint64_t requesterMac,
                                      uint32_t requesterIp) {
  // Sized to Ethernet minimum frame (60 bytes payload + 4 FCS = 64). The
  // 18 trailing bytes are zero padding; without it some receivers truncate
  // the tail of the ARP payload.
  std::array<uint8_t, 60> f{};
  // Eth dst: requester MAC
  for (int i = 0; i < 6; ++i) f[i] = (requesterMac >> (8 * (5 - i))) & 0xFF;
  // Eth src: target MAC (the host we are proxying for)
  for (int i = 0; i < 6; ++i) f[6 + i] = (targetMac >> (8 * (5 - i))) & 0xFF;
  // Ethertype: 0x0806
  f[12] = 0x08;
  f[13] = 0x06;
  // HW type: 1 (Ethernet)
  f[14] = 0x00;
  f[15] = 0x01;
  // Proto type: 0x0800 (IPv4)
  f[16] = 0x08;
  f[17] = 0x00;
  // HW len, proto len
  f[18] = 6;
  f[19] = 4;
  // Op: 2 (reply)
  f[20] = 0x00;
  f[21] = 0x02;
  // Sender HW: target MAC
  for (int i = 0; i < 6; ++i) f[22 + i] = (targetMac >> (8 * (5 - i))) & 0xFF;
  // Sender proto: target IP (host byte order in memory; serialize big-endian)
  f[28] = (targetIp >> 24) & 0xFF;
  f[29] = (targetIp >> 16) & 0xFF;
  f[30] = (targetIp >> 8) & 0xFF;
  f[31] = (targetIp) & 0xFF;
  // Target HW: requester MAC
  for (int i = 0; i < 6; ++i)
    f[32 + i] = (requesterMac >> (8 * (5 - i))) & 0xFF;
  // Target proto: requester IP
  f[38] = (requesterIp >> 24) & 0xFF;
  f[39] = (requesterIp >> 16) & 0xFF;
  f[40] = (requesterIp >> 8) & 0xFF;
  f[41] = (requesterIp) & 0xFF;
  return f;
}

}  // namespace ns3