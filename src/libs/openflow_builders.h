#ifndef OPENFLOW_BUILDERS_H
#define OPENFLOW_BUILDERS_H

#include <cstddef>
#include <cstdint>
#include <vector>
#include "ns3/ofswitch13-module.h"

struct ofl_msg_header;

namespace ns3 {

namespace LldpTlv
{
    constexpr uint16_t EtherType = 0x88CC;
    constexpr uint8_t  MulticastDst[6] = {0x01, 0x80, 0xC2, 0x00, 0x00, 0x0E};

    enum class Type : uint16_t
    {
        EndOfLldpdu = 0,
        ChassisId   = 1,
        PortId      = 2,
        TimeToLive  = 3,
        LatencyTag  = 9,  // custom TLV: 8-byte big-endian send timestamp (ns)
    };

    enum class ChassisIdSubtype : uint8_t
    {
        ChassisComponent = 1,
        InterfaceAlias   = 2,
        PortComponent    = 3,
        MacAddress       = 4,
        NetworkAddress   = 5,
        InterfaceName    = 6,
        LocallyAssigned  = 7,
    };

    enum class PortIdSubtype : uint8_t
    {
        InterfaceAlias   = 1,
        PortComponent    = 2,
        MacAddress       = 3,
        NetworkAddress   = 4,
        InterfaceName    = 5,
        AgentCircuitId   = 6,
        LocallyAssigned  = 7,
    };

    inline constexpr uint16_t MakeHeader(Type type, std::size_t length)
    {
        return (static_cast<uint16_t>(type) << 9) | (length & 0x1FF);
    }
} // namespace LldpTlv

std::vector<uint8_t> BuildLldpFrame(uint64_t chassisId, uint32_t portId, uint64_t timestampNs = 0);

std::string FormatMac(uint64_t mac);

struct ofl_msg_header* BuildLldpPacketOut(uint32_t portNo,
                                          const uint8_t* frame,
                                          std::size_t frameLen);

struct ofl_msg_header* BuildPortStatsRequest();
struct ofl_msg_header* BuildPortDescRequest();

} // namespace ns3

#endif // OPENFLOW_BUILDERS_H