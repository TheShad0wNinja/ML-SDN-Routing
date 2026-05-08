#ifndef ZMQ_OPENFLOW_BUILDERS_H
#define ZMQ_OPENFLOW_BUILDERS_H

#include <cstdint>
#include <cstddef>
#include <vector>

extern "C" {
#include <bofuss/ofl-messages.h>
#include <bofuss/ofl-actions.h>
#include <bofuss/openflow.h>
}

namespace ns3 {

// Build an OFPT_PACKET_OUT containing the provided frame and a single
// OFPAT_OUTPUT action to `port_no`. Caller is responsible for calling
// ofl_msg_free on the returned pointer when done.
struct ofl_msg_header* BuildLldpPacketOut(uint32_t port_no,
                                         const uint8_t* frame,
                                         size_t frame_len);

// Build an OFPT_MULTIPART_REQUEST (PORT_STATS) requesting stats for all
// ports. Caller must free with ofl_msg_free.
struct ofl_msg_header* BuildPortStatsRequest();

// Build LLDP Ethernet frame bytes matching Python `build_lldp_frame`.
// Returns a vector containing the full Ethernet frame.
std::vector<uint8_t> BuildLldpFrame(uint64_t dpid, uint32_t port_no);

} // namespace ns3

#endif // ZMQ_OPENFLOW_BUILDERS_H
