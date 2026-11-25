#pragma once

#include "ethernet_parser.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace netlab {

struct DeviceInfo {
    std::string name;
    std::string description;
    std::vector<std::string> ipv4_addresses;
    bool loopback = false;
};

struct CaptureOptions {
    std::string device_name;
    std::string filter;
    std::uint64_t count = 0;
    unsigned duration_seconds = 0;
};

struct CaptureStats {
    std::uint64_t received = 0;
    std::uint64_t parsed = 0;
    std::uint64_t rejected = 0;
    std::uint32_t ps_recv = 0;
    std::uint32_t ps_drop = 0;
    std::uint32_t ps_ifdrop = 0;
    bool has_driver_stats = false;
};

struct PacketRecord {
    std::uint64_t sequence = 0;
    std::string local_time;
    std::uint32_t captured_length = 0;
    std::uint32_t wire_length = 0;
    ParseStatus status = ParseStatus::InvalidInput;
    EthernetFrame frame;
};

struct CaptureCallbacks {
    std::function<void(const std::string&)> on_message;
    std::function<void(const PacketRecord&)> on_packet;
};

enum class CaptureError {
    None,
    Other,
    UnsupportedLinkType
};

// 获取设备列表及 IPv4 地址。
bool enumerate_devices(std::vector<DeviceInfo>& devices, std::string& error);

// 打开网卡、捕获帧、解析并回调输出。
bool capture_live(const CaptureOptions& options, const CaptureCallbacks& callbacks,
                  const std::atomic<bool>& running, CaptureStats& stats,
                  std::string& error, CaptureError* error_kind = nullptr);

}
