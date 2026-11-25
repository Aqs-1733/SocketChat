#include "ethernet_parser.h"

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace netlab {
namespace {

std::uint16_t read_be16(const std::uint8_t* bytes) {
    return static_cast<std::uint16_t>((static_cast<unsigned>(bytes[0]) << 8U) |
                                      static_cast<unsigned>(bytes[1]));
}

bool is_vlan_tpid(std::uint16_t value) {
    return value == 0x8100U || value == 0x88A8U || value == 0x9100U;
}

}

std::uint16_t VlanTag::id() const {
    return static_cast<std::uint16_t>(tci & 0x0FFFU);
}

unsigned VlanTag::priority() const {
    return (tci >> 13U) & 0x07U;
}

bool VlanTag::drop_eligible() const {
    return (tci & 0x1000U) != 0;
}

ParseStatus parse_ethernet(const std::uint8_t* bytes, std::size_t length,
                           EthernetFrame& result) {
    result = EthernetFrame{};

    if (bytes == nullptr) {
        return ParseStatus::InvalidInput;
    }
    if (length < 14) {
        return ParseStatus::ShortEthernetHeader;
    }

    // 先读取以太网头中的三个必需字段。
    std::copy_n(bytes, 6, result.destination.begin());
    std::copy_n(bytes + 6, 6, result.source.begin());
    result.type_or_length = read_be16(bytes + 12);

    // 保存外层类型字段后的第一个字节。
    if (length > 14) {
        result.first_byte = bytes[14];
        result.has_first_byte = true;
    }

    std::uint16_t field = result.type_or_length;
    std::size_t offset = 14;

    // 解开最多两层 VLAN 标签。
    while (is_vlan_tpid(field)) {
        if (result.vlans.size() >= 2) {
            return ParseStatus::TooManyVlanTags;
        }
        if (length - offset < 4) {
            return ParseStatus::ShortVlanHeader;
        }

        VlanTag tag;
        tag.tpid = field;
        tag.tci = read_be16(bytes + offset);
        result.vlans.push_back(tag);
        field = read_be16(bytes + offset + 2);
        offset += 4;
    }

    result.protocol_field = field;
    result.is_length_field = field <= 1500U;
    result.data_offset = offset;

    if (offset < length) {
        result.payload_first_byte = bytes[offset];
        result.has_payload_first_byte = true;
    }

    return ParseStatus::Ok;
}

std::string format_mac(const MacAddress& mac) {
    std::ostringstream output;
    output << std::uppercase << std::hex << std::setfill('0');

    for (std::size_t i = 0; i < mac.size(); ++i) {
        if (i != 0) {
            output << '-';
        }
        output << std::setw(2) << static_cast<unsigned>(mac[i]);
    }

    return output.str();
}

std::string describe_protocol(std::uint16_t value) {
    switch (value) {
        case 0x0800U: return "IPv4";
        case 0x0806U: return "ARP";
        case 0x86DDU: return "IPv6";
        case 0x8035U: return "RARP";
        case 0x8847U: return "MPLS unicast";
        case 0x8848U: return "MPLS multicast";
        case 0x8863U: return "PPPoE discovery";
        case 0x8864U: return "PPPoE session";
        case 0x88CCU: return "LLDP";
        default:
            if (value <= 1500U) return "IEEE 802.3 length";
            if (value < 1536U) return "Reserved";
            return "Other EtherType";
    }
}

std::string describe_status(ParseStatus status) {
    switch (status) {
        case ParseStatus::Ok: return "OK";
        case ParseStatus::ShortEthernetHeader: return "Ethernet header shorter than 14 bytes";
        case ParseStatus::ShortVlanHeader: return "Incomplete VLAN header";
        case ParseStatus::TooManyVlanTags: return "More than two VLAN tags";
        case ParseStatus::InvalidInput: return "Null packet buffer";
    }
    return "Unknown parse status";
}

}
