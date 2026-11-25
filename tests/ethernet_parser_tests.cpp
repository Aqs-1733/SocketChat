#include "../src/ethernet_parser.h"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

int checks = 0;
int failures = 0;

void check(bool condition, const std::string& name) {
    ++checks;
    if (!condition) {
        ++failures;
        std::cerr << "[FAIL] " << name << '\n';
    }
}

const std::vector<std::uint8_t> base = {
    0x00, 0x11, 0x22, 0x33, 0x44, 0x55,
    0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB,
    0x08, 0x00,
    0x45, 0x00, 0x00, 0x28
};

void test_normal() {
    netlab::EthernetFrame result;
    check(netlab::parse_ethernet(base.data(), base.size(), result) == netlab::ParseStatus::Ok,
          "IPv4 parse");
    check(netlab::format_mac(result.destination) == "00-11-22-33-44-55", "destination MAC");
    check(netlab::format_mac(result.source) == "66-77-88-99-AA-BB", "source MAC");
    check(result.type_or_length == 0x0800 && result.protocol_field == 0x0800, "IPv4 type");
    check(result.has_first_byte && result.first_byte == 0x45 && result.data_offset == 14,
          "first byte after type");
    check(result.has_payload_first_byte && result.payload_first_byte == 0x45,
          "first payload byte");
    check(result.vlans.empty() && !result.is_length_field, "no VLAN");
}

void test_protocols() {
    netlab::EthernetFrame result;
    auto bytes = base;
    bytes[12] = 0x08;
    bytes[13] = 0x06;
    bytes[14] = 0x00;
    check(netlab::parse_ethernet(bytes.data(), bytes.size(), result) == netlab::ParseStatus::Ok &&
              result.protocol_field == 0x0806 && result.first_byte == 0x00, "ARP");

    bytes[12] = 0x86;
    bytes[13] = 0xDD;
    bytes[14] = 0x60;
    check(netlab::parse_ethernet(bytes.data(), bytes.size(), result) == netlab::ParseStatus::Ok &&
              result.first_byte == 0x60, "IPv6");

    bytes[12] = 0x00;
    bytes[13] = 0x04;
    bytes[14] = 0xAA;
    check(netlab::parse_ethernet(bytes.data(), bytes.size(), result) == netlab::ParseStatus::Ok &&
              result.is_length_field && result.first_byte == 0xAA, "802.3 length field");

    bytes[12] = 0x05;
    bytes[13] = 0xDD;
    check(netlab::parse_ethernet(bytes.data(), bytes.size(), result) == netlab::ParseStatus::Ok &&
              !result.is_length_field, "reserved field");
    check(netlab::describe_protocol(0x0800) == "IPv4", "IPv4 description");
    check(netlab::describe_protocol(0x0806) == "ARP", "ARP description");
    check(netlab::describe_protocol(0x86DD) == "IPv6", "IPv6 description");
}

void test_vlan() {
    netlab::EthernetFrame result;
    auto bytes = base;
    bytes[12] = 0x81;
    bytes[13] = 0x00;
    bytes.insert(bytes.begin() + 14, {0xA0, 0x64, 0x08, 0x00});

    check(netlab::parse_ethernet(bytes.data(), bytes.size(), result) == netlab::ParseStatus::Ok,
          "802.1Q parse");
    check(result.vlans.size() == 1 && result.vlans[0].id() == 100 &&
              result.vlans[0].priority() == 5, "802.1Q fields");
    check(result.type_or_length == 0x8100 && result.protocol_field == 0x0800,
          "802.1Q EtherTypes");

    // 外层字段之后是 VLAN TCI，内层字段之后才是 IPv4 数据。
    check(result.has_first_byte && result.first_byte == 0xA0,
          "802.1Q raw byte after outer type");
    check(result.has_payload_first_byte && result.payload_first_byte == 0x45 &&
              result.data_offset == 18, "802.1Q payload first byte");
}

void test_qinq() {
    netlab::EthernetFrame result;
    auto bytes = base;
    bytes[12] = 0x88;
    bytes[13] = 0xA8;
    bytes.insert(bytes.begin() + 14,
                 {0x00, 0x0A, 0x81, 0x00, 0x00, 0x14, 0x08, 0x00});

    check(netlab::parse_ethernet(bytes.data(), bytes.size(), result) == netlab::ParseStatus::Ok,
          "QinQ parse");
    check(result.vlans.size() == 2 && result.vlans[0].id() == 10 &&
              result.vlans[1].id() == 20, "QinQ VLAN ids");
    check(result.first_byte == 0x00 && result.payload_first_byte == 0x45 &&
              result.data_offset == 22, "QinQ raw and payload bytes");

    bytes[20] = 0x81;
    bytes[21] = 0x00;
    check(netlab::parse_ethernet(bytes.data(), bytes.size(), result) ==
              netlab::ParseStatus::TooManyVlanTags, "triple VLAN rejected");
}

void test_bounds() {
    netlab::EthernetFrame result;
    for (std::size_t n = 0; n < 14; ++n) {
        check(netlab::parse_ethernet(base.data(), n, result) ==
                  netlab::ParseStatus::ShortEthernetHeader,
              "short frame " + std::to_string(n));
    }

    check(netlab::parse_ethernet(nullptr, 0, result) == netlab::ParseStatus::InvalidInput,
          "null buffer");
    check(netlab::parse_ethernet(base.data(), 14, result) == netlab::ParseStatus::Ok &&
              !result.has_first_byte && !result.has_payload_first_byte,
          "14-byte header with no payload");

    auto bytes = base;
    bytes[12] = 0x81;
    bytes[13] = 0x00;
    for (std::size_t n = 14; n < 18; ++n) {
        check(netlab::parse_ethernet(bytes.data(), n, result) ==
                  netlab::ParseStatus::ShortVlanHeader,
              "short VLAN " + std::to_string(n));
    }

    check(netlab::parse_ethernet(bytes.data(), 18, result) == netlab::ParseStatus::Ok &&
              result.has_first_byte && !result.has_payload_first_byte,
          "VLAN with no payload");
    check(netlab::parse_ethernet(base.data(), base.size(), result) == netlab::ParseStatus::Ok &&
              result.vlans.empty() && result.first_byte == 0x45,
          "state resets for next frame");
    check(netlab::format_mac({0, 0, 0, 0, 0, 0}) == "00-00-00-00-00-00",
          "zero MAC formatting");
    check(netlab::format_mac({255, 255, 255, 255, 255, 255}) == "FF-FF-FF-FF-FF-FF",
          "broadcast MAC formatting");
}

}

int main() {
    test_normal();
    test_protocols();
    test_vlan();
    test_qinq();
    test_bounds();

    if (failures == 0) {
        std::cout << "[PASS] " << checks << " assertions\n";
        return 0;
    }
    std::cerr << "[FAIL] " << failures << " / " << checks << "\n";
    return 1;
}
