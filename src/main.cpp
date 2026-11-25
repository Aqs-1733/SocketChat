#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>
#include <pcap.h>
#else
#error This entry point runs on Windows with Npcap.
#endif

#include "capture_service.h"
#include "display.h"
#include "npcap_runtime.h"

#include <atomic>
#include <cctype>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

std::atomic<bool> running{true};

BOOL WINAPI console_handler(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT) {
        running.store(false);
        return TRUE;
    }
    return FALSE;
}

struct ProgramOptions {
    bool help = false;
    bool list_only = false;
    // 0 表示交互选择网卡。
    std::size_t adapter_index = 0;
    std::string device_name;
    std::string filter;
    std::string log_file;
    std::uint64_t count = 0;
    unsigned duration_seconds = 0;
};

void print_help() {
    std::cout
        << "Ethernet Packet Capture Lab 2 (C++17 / Npcap)\n"
        << "Usage: packet_capture.exe [options]\n"
        << "  --list              List local capture devices and exit\n"
        << "  --adapter N         Select device index (1-based)\n"
        << "  --device NAME       Select exact Npcap device name\n"
        << "  --filter BPF        Apply optional BPF filter (quote spaces)\n"
        << "  --count N           Stop after N captured frames\n"
        << "  --duration N        Stop after N seconds\n"
        << "                      --count 与 --duration 可同时使用，先达到任一限制即停止\n"
        << "  --log FILE          Also write text logs to FILE (optional)\n"
        << "  --help              Print this help\n"
        << "Examples:\n"
        << "  packet_capture.exe --list\n"
        << "  packet_capture.exe --adapter 1 --count 20\n"
        << "  packet_capture.exe --adapter 2 --filter \"arp or ip or ip6\" --duration 30\n"
        << "  packet_capture.exe --adapter 1 --count 20 --log capture-log.txt\n";
}

bool parse_positive(const std::string& value, std::uint64_t& parsed) {
    if (value.empty()) return false;
    std::uint64_t number = 0;
    for (char character : value) {
        if (character < '0' || character > '9') return false;
        const auto digit = static_cast<unsigned>(character - '0');
        if (number > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U)
            return false;
        number = number * 10U + digit;
    }
    if (number == 0) return false;
    parsed = number;
    return true;
}

// 检查命令行参数及数值范围。
bool parse_options(int argc, char* argv[], ProgramOptions& options,
                   std::string& error) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        if (arg == "--help" || arg == "-h") { options.help = true; return true; }
        if (arg == "--list") { options.list_only = true; continue; }
        if (arg != "--adapter" && arg != "--device" && arg != "--filter" &&
            arg != "--count" && arg != "--duration" && arg != "--log") {
            error = "Unknown option: " + arg;
            return false;
        }
        if (++i >= argc) { error = "Missing value after " + arg; return false; }
        const std::string value(argv[i]);
        if (arg == "--device") options.device_name = value;
        else if (arg == "--filter") options.filter = value;
        else if (arg == "--log") options.log_file = value;
        else {
            std::uint64_t number = 0;
            if (!parse_positive(value, number)) {
                error = "Invalid positive integer for " + arg + ": " + value;
                return false;
            }
            if (arg == "--adapter") {
                if (number > std::numeric_limits<std::size_t>::max()) {
                    error = "Device index out of range";
                    return false;
                }
                options.adapter_index = static_cast<std::size_t>(number);
            } else if (arg == "--count") {
                options.count = number;
            } else {
                if (number > std::numeric_limits<unsigned>::max()) {
                    error = "Duration out of range";
                    return false;
                }
                options.duration_seconds = static_cast<unsigned>(number);
            }
        }
    }
    if (options.adapter_index && !options.device_name.empty()) {
        error = "Use --adapter or --device, not both";
        return false;
    }
    return true;
}

// 输出所有可捕获网卡及其地址。
void print_devices(const std::vector<netlab::DeviceInfo>& devices) {
    std::cout << "========== Local capture devices ==========\n";
    for (std::size_t i = 0; i < devices.size(); ++i) {
        const auto& device = devices[i];
        std::cout << "[" << i + 1 << "] " << device.description;
        if (device.loopback) {
            std::cout << " [Npcap Loopback / 回环：不适合本实验的 Ethernet II MAC 字段解析]";
        }
        std::cout << "\n    Name: " << device.name << '\n';
        if (!device.ipv4_addresses.empty()) {
            std::cout << "    IPv4: ";
            for (std::size_t j = 0; j < device.ipv4_addresses.size(); ++j) {
                if (j) std::cout << ", ";
                std::cout << device.ipv4_addresses[j];
            }
            std::cout << '\n';
        }
    }
    std::cout << "提示：请选择实际联网的以太网或 Wi-Fi 网卡；设备是否支持解析以实际链路类型为准。\n";
}

// 通过序号或名称选定网卡。
bool select_device(const ProgramOptions& options,
                   const std::vector<netlab::DeviceInfo>& devices,
                   std::string& selected, std::string& error) {
    if (!options.device_name.empty()) {
        for (const auto& device : devices) {
            if (device.name == options.device_name) {
                selected = device.name;
                return true;
            }
        }
        error = "--device name not found. Run --list first.";
        return false;
    }
    std::size_t index = options.adapter_index;
    if (index == 0) {
        std::cout << "请选择网卡编号 (1-" << devices.size() << "): " << std::flush;
        std::string input;
        if (!std::getline(std::cin, input)) {
            error = "No selection provided";
            return false;
        }
        std::uint64_t parsed = 0;
        if (!parse_positive(input, parsed) || parsed > devices.size()) {
            error = "Invalid device selection: " + input;
            return false;
        }
        index = static_cast<std::size_t>(parsed);
    }
    if (index < 1 || index > devices.size()) {
        error = "Device index out of range";
        return false;
    }
    selected = devices[index - 1].name;
    return true;
}

}

// 初始化 Npcap，开始捕获并退出时清理资源。
int main(int argc, char* argv[]) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    SetConsoleCtrlHandler(console_handler, TRUE);
    ProgramOptions options;
    std::string error;
    if (!parse_options(argc, argv, options, error)) {
        std::cerr << "[ERROR] " << error << "\n";
        print_help();
        return 2;
    }
    if (options.help) { print_help(); return 0; }

    if (!netlab::prepare_npcap(error)) {
        std::cerr << "[ERROR] " << error << '\n';
        return 1;
    }

    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        std::cerr << "[ERROR] WSAStartup failed\n";
        return 1;
    }
    char pcap_error[PCAP_ERRBUF_SIZE]{};
    if (pcap_init(PCAP_CHAR_ENC_UTF_8, pcap_error) != 0) {
        std::cerr << "[ERROR] pcap_init failed: " << pcap_error << '\n';
        WSACleanup();
        return 1;
    }
    std::vector<netlab::DeviceInfo> devices;
    std::cout << "[Npcap] " << pcap_lib_version() << '\n';
    if (!netlab::enumerate_devices(devices, error)) {
        std::cerr << "[ERROR] " << error << '\n';
        WSACleanup();
        return 1;
    }
    if (devices.empty()) {
        std::cerr << "[ERROR] No devices found. Check Npcap installation and privileges.\n";
        WSACleanup();
        return 1;
    }
    print_devices(devices);
    if (options.list_only) { WSACleanup(); return 0; }

    std::string device_name;
    if (!select_device(options, devices, device_name, error)) {
        std::cerr << "[ERROR] " << error << '\n';
        WSACleanup();
        return 2;
    }
    netlab::Display display(options.log_file);
    if (!display.ready()) {
        std::cerr << "[ERROR] " << display.error() << '\n';
        WSACleanup();
        return 1;
    }
    netlab::CaptureOptions capture_options;
    capture_options.device_name = device_name;
    capture_options.filter = options.filter;
    capture_options.count = options.count;
    capture_options.duration_seconds = options.duration_seconds;
    netlab::CaptureCallbacks callbacks;
    callbacks.on_message = [&](const std::string& message) {
        display.line(message);
    };
    callbacks.on_packet = [&](const netlab::PacketRecord& packet) {
        display.packet(packet);
    };

    netlab::CaptureStats stats;
    bool success = false;
    for (;;) {
        netlab::CaptureError error_kind = netlab::CaptureError::None;
        success = netlab::capture_live(capture_options, callbacks, running, stats,
                                      error, &error_kind);
        if (success || error_kind != netlab::CaptureError::UnsupportedLinkType ||
            !running.load()) {
            break;
        }

        std::cerr << "[ERROR] " << error << '\n';
        std::cout << "请重新选择网卡，程序不会自动切换设备。\n";

        // 重选只改变设备，保留过滤条件和停止限制。
        ProgramOptions retry_options;
        if (!select_device(retry_options, devices, device_name, error)) {
            std::cerr << "[ERROR] " << error << '\n';
            WSACleanup();
            return 2;
        }
        capture_options.device_name = device_name;
    }

    if (stats.has_driver_stats) {
        display.line("[STATS] Driver received=" + std::to_string(stats.ps_recv) +
                     ", dropped=" + std::to_string(stats.ps_drop) +
                     ", interface dropped=" + std::to_string(stats.ps_ifdrop));
    }
    if (!success) std::cerr << "[ERROR] " << error << '\n';
    WSACleanup();
    return success ? 0 : 1;
}
