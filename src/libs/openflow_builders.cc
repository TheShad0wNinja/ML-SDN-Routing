#include "openflow_builders.h"
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
#include <arpa/inet.h>

namespace ns3 {

struct ofl_msg_header* BuildLldpPacketOut(uint32_t port_no,
                                         const uint8_t* frame,
                                         size_t frame_len) {
    struct ofl_msg_packet_out* msg = (struct ofl_msg_packet_out*)malloc(sizeof(*msg));
    if (!msg) return nullptr;
    memset(msg, 0, sizeof(*msg));
    msg->header.type = OFPT_PACKET_OUT;
    msg->buffer_id = OFP_NO_BUFFER;
    msg->in_port = OFPP_CONTROLLER;

    // copy frame data
    if (frame && frame_len > 0) {
        msg->data_length = frame_len;
        msg->data = (uint8_t*)malloc(frame_len);
        if (!msg->data) {
            free(msg);
            return nullptr;
        }
        memcpy(msg->data, frame, frame_len);
    } else {
        msg->data_length = 0;
        msg->data = nullptr;
    }

    // create single output action
    struct ofl_action_output* a = (struct ofl_action_output*)malloc(sizeof(*a));
    if (!a) {
        if (msg->data) free(msg->data);
        free(msg);
        return nullptr;
    }
    a->header.type = OFPAT_OUTPUT;
    a->header.len = sizeof(struct ofl_action_output);
    a->port = port_no;
    a->max_len = 0;

    msg->actions_num = 1;
    msg->actions = (struct ofl_action_header**)malloc(sizeof(struct ofl_action_header*));
    msg->actions[0] = (struct ofl_action_header*)a;

    return (struct ofl_msg_header*)msg;
}

struct ofl_msg_header* BuildPortStatsRequest() {
    struct ofl_msg_multipart_request_port* req =
        (struct ofl_msg_multipart_request_port*)malloc(sizeof(*req));
    if (!req) return nullptr;
    memset(req, 0, sizeof(*req));
    req->header.header.type = OFPT_MULTIPART_REQUEST;
    req->header.type = OFPMP_PORT_STATS;
    req->header.flags = 0;
    req->port_no = OFPP_ANY;
    return (struct ofl_msg_header*)req;
}

std::vector<uint8_t> BuildLldpFrame(uint64_t dpid, uint32_t port_no) {
    // Build LLDP frame similar to Python implementation in scratch/python/controller/lldp.py
    std::vector<uint8_t> frame;
    // dst MAC 01:80:c2:00:00:0e
    uint8_t dst[6] = {0x01, 0x80, 0xc2, 0x00, 0x00, 0x0e};
    uint8_t src[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
    frame.insert(frame.end(), dst, dst+6);
    frame.insert(frame.end(), src, src+6);
    uint16_t ethertype = htons(0x88CC);
    frame.push_back(reinterpret_cast<uint8_t*>(&ethertype)[0]);
    frame.push_back(reinterpret_cast<uint8_t*>(&ethertype)[1]);

    auto push_tlv = [&](uint16_t type, const std::vector<uint8_t>& value){
        uint16_t hdr = (type << 9) | (value.size());
        uint8_t b0 = (hdr >> 8) & 0xff;
        uint8_t b1 = hdr & 0xff;
        frame.push_back(b0);
        frame.push_back(b1);
        frame.insert(frame.end(), value.begin(), value.end());
    };

    // chassis id TLV: subtype 7 + dpid string
    std::string dpid_s = std::to_string(dpid);
    std::vector<uint8_t> chassis;
    chassis.push_back(7); // locally assigned
    chassis.insert(chassis.end(), dpid_s.begin(), dpid_s.end());
    push_tlv(1, chassis);

    // port id TLV: subtype 5 + port string
    std::string port_s = std::to_string(port_no);
    std::vector<uint8_t> portv;
    portv.push_back(5);
    portv.insert(portv.end(), port_s.begin(), port_s.end());
    push_tlv(2, portv);

    // ttl TLV: 2 bytes big-endian
    uint16_t ttl = htons(120);
    std::vector<uint8_t> ttlv{reinterpret_cast<uint8_t*>(&ttl)[0], reinterpret_cast<uint8_t*>(&ttl)[1]};
    push_tlv(3, ttlv);

    // end TLV
    push_tlv(0, std::vector<uint8_t>{});

    return frame;
}

} // namespace ns3
