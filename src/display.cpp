#include "display.h"

#include <iomanip>
#include <iostream>
#include <sstream>

namespace netlab {
namespace {

std::string hex16(std::uint16_t value) {
    std::ostringstream out;
    out << "0x" << std::uppercase << std::hex << std::setw(4)
        << std::setfill('0') << value;
    return out.str();
}

std::string hex8(std::uint8_t value) {
    std::ostringstream out;
    out << "0x" << std::uppercase << std::hex << std::setw(2)
        << std::setfill('0') << static_cast<unsigned>(value);
    return out.str();
}

}

Display::Display(const std::string& log_file) {
    if (!log_file.empty()) {
        log_.open(log_file, std::ios::out | std::ios::trunc);
        if (!log_.is_open()) {
            error_ = "Cannot open log file: " + log_file;
        }
    }
}

bool Display::ready() const {
    return error_.empty();
}

const std::string& Display::error() const {
    return error_;
}

void Display::line(const std::string& message) {
    std::cout << message << '\n';
    if (log_.is_open()) {
        log_ << message << '\n';
    }
}

void Display::packet(const PacketRecord& record) {
    line("----------------------------------------------------------------");
    line("[Frame #" + std::to_string(record.sequence) + "] " + record.local_time +
         " | captured=" + std::to_string(record.captured_length) +
         " | original=" + std::to_string(record.wire_length));

    if (record.status != ParseStatus::Ok) {
        line("  [SKIP] " + describe_status(record.status));
        return;
    }

    const EthernetFrame& frame = record.frame;
    line("  Destination MAC  : " + format_mac(frame.destination));
    line("  Source MAC       : " + format_mac(frame.source));
    line("  Type/length      : " + hex16(frame.type_or_length) +
         " (" + describe_protocol(frame.type_or_length) + ")");

    // 先输出外层类型字段后的第一个字节。
    if (frame.has_first_byte) {
        line("  First byte       : " + hex8(frame.first_byte) +
             " (" + std::to_string(static_cast<unsigned>(frame.first_byte)) + ")");
    } else {
        line("  First byte       : N/A");
    }

    for (std::size_t i = 0; i < frame.vlans.size(); ++i) {
        const auto& tag = frame.vlans[i];
        line("  VLAN #" + std::to_string(i + 1) + "           : TPID=" +
             hex16(tag.tpid) + ", ID=" + std::to_string(tag.id()) +
             ", PCP=" + std::to_string(tag.priority()) +
             ", DEI=" + std::to_string(tag.drop_eligible() ? 1 : 0));
    }

    if (!frame.vlans.empty()) {
        line("  Inner type/length: " + hex16(frame.protocol_field) +
             " (" + describe_protocol(frame.protocol_field) + ")");
        line("  Payload offset   : " + std::to_string(frame.data_offset));
        line("  Payload first    : " +
             (frame.has_payload_first_byte ? hex8(frame.payload_first_byte) : "N/A"));
    }

    if (record.captured_length < record.wire_length) {
        line("  [NOTE] Truncated capture");
    }
}

}
