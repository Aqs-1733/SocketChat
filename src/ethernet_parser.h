#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace netlab {

using MacAddress = std::array<std::uint8_t, 6>;

struct VlanTag {
    std::uint16_t tpid = 0;
    std::uint16_t tci = 0;

    std::uint16_t id() const;
    unsigned priority() const;
    bool drop_eligible() const;
};

enum class ParseStatus {
    Ok,
    ShortEthernetHeader,
    ShortVlanHeader,
    TooManyVlanTags,
    InvalidInput
};

struct EthernetFrame {
    MacAddress destination{};
    MacAddress source{};
    std::uint16_t type_or_length = 0;
    std::uint16_t protocol_field = 0;
    bool is_length_field = false;
    std::vector<VlanTag> vlans;

    // 外层类型字段后第一个字节，固定在偏移 14 处。
    bool has_first_byte = false;
    std::uint8_t first_byte = 0;

    // 跳过 VLAN 标签后的数据起始位置。
    std::size_t data_offset = 14;
    bool has_payload_first_byte = false;
    std::uint8_t payload_first_byte = 0;
};

// 使用实际捕获长度检查边界，避免读取不完整帧。
ParseStatus parse_ethernet(const std::uint8_t* bytes, std::size_t length,
                           EthernetFrame& result);

std::string format_mac(const MacAddress& mac);
std::string describe_protocol(std::uint16_t value);
std::string describe_status(ParseStatus status);

}
