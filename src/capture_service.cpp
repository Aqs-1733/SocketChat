
#include "capture_service.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <pcap.h>
#else
#error Npcap capture requires Windows.
#endif

#include <chrono>
#include <ctime>
#include <iomanip>
#include <memory>
#include <sstream>
#include <thread>
#include <utility>

namespace netlab {
namespace {

struct PcapDeleter {
    void operator()(pcap_t* ptr) const {
        if (ptr != nullptr) {
            pcap_close(ptr);
        }
    }
};

struct DeviceDeleter {
    void operator()(pcap_if_t* ptr) const {
        if (ptr != nullptr) {
            pcap_freealldevs(ptr);
        }
    }
};

using PcapHandle = std::unique_ptr<pcap_t, PcapDeleter>;
using DeviceList = std::unique_ptr<pcap_if_t, DeviceDeleter>;

struct CompiledFilter {
    bpf_program program{};
    bool compiled = false;

    ~CompiledFilter() {
        if (compiled) {
            pcap_freecode(&program);
        }
    }

    CompiledFilter() = default;
    CompiledFilter(const CompiledFilter&) = delete;
    CompiledFilter& operator=(const CompiledFilter&) = delete;
};

std::string format_timestamp(const timeval& ts) {
    const std::time_t seconds = static_cast<std::time_t>(ts.tv_sec);
    std::tm calendar{};

    if (localtime_s(&calendar, &seconds) != 0) {
        return "invalid-time";
    }

    std::ostringstream out;
    out << std::put_time(&calendar, "%Y-%m-%d %H:%M:%S")
        << '.' << std::setw(6) << std::setfill('0') << ts.tv_usec;

    return out.str();
}

std::string ipv4_to_string(const sockaddr* address) {
    if (address == nullptr || address->sa_family != AF_INET) {
        return {};
    }

    const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(address);
    char ip[INET_ADDRSTRLEN]{};

    if (InetNtopA(AF_INET, const_cast<IN_ADDR*>(&ipv4->sin_addr),
                  ip, static_cast<DWORD>(sizeof(ip))) == nullptr) {
        return {};
    }

    return ip;
}

void emit_message(const CaptureCallbacks& callbacks, const std::string& message) {
    if (callbacks.on_message) {
        callbacks.on_message(message);
    }
}

}

bool enumerate_devices(std::vector<DeviceInfo>& devices, std::string& error) {
    devices.clear();
    error.clear();

    pcap_if_t* raw = nullptr;
    char errbuf[PCAP_ERRBUF_SIZE]{};

    // 获取本机网卡设备列表。
    if (pcap_findalldevs(&raw, errbuf) != 0) {
        error = std::string("pcap_findalldevs failed: ") + errbuf;
        return false;
    }

    DeviceList all(raw);

    // 遍历设备并保存网卡信息。
    for (const pcap_if_t* current = all.get(); current != nullptr;
         current = current->next) {
        if (current->name == nullptr || *current->name == '\0') {
            continue;
        }

        DeviceInfo info;
        info.name = current->name;
        info.description = current->description
                               ? current->description
                               : "(No description)";
        info.loopback = (current->flags & PCAP_IF_LOOPBACK) != 0;

        // 获取网卡绑定的 IPv4 地址。
        for (const pcap_addr_t* address = current->addresses;
             address != nullptr; address = address->next) {
            const std::string ip = ipv4_to_string(address->addr);

            if (!ip.empty()) {
                info.ipv4_addresses.push_back(ip);
            }
        }

        devices.push_back(std::move(info));
    }

    return true;
}

bool capture_live(const CaptureOptions& options, const CaptureCallbacks& callbacks,
                  const std::atomic<bool>& running, CaptureStats& stats,
                  std::string& error, CaptureError* error_kind) {
    stats = CaptureStats{};
    error.clear();
    if (error_kind != nullptr) {
        *error_kind = CaptureError::Other;
    }

    char errbuf[PCAP_ERRBUF_SIZE]{};

    // 打开网卡，读取超时设为 100 毫秒。
    PcapHandle handle(
        pcap_open_live(options.device_name.c_str(), 65535, 1, 100, errbuf)
    );

    if (!handle) {
        error = std::string("pcap_open_live failed: ") + errbuf;
        return false;
    }

    // 仅解析以太网链路类型。
    const int link_type = pcap_datalink(handle.get());

    if (link_type < 0) {
        error = std::string("pcap_datalink failed: ") + pcap_geterr(handle.get());
        return false;
    }

    if (link_type != DLT_EN10MB) {
        if (error_kind != nullptr) {
            *error_kind = CaptureError::UnsupportedLinkType;
        }
        error = "当前网卡的链路类型为 " + std::to_string(link_type);
        if (link_type == DLT_NULL) {
            error += "（DLT_NULL，Npcap Loopback 回环链路），数据不含以太网 MAC 首部，"
                     "不适合本实验的 Ethernet II MAC 字段解析。";
        } else {
            const char* name = pcap_datalink_val_to_name(link_type);
            if (name != nullptr) {
                error += "（" + std::string(name) + "）";
            }
            error += "，不是 DLT_EN10MB（以太网），本实验不按 Ethernet II 格式解析此链路。";
        }
        return false;
    }

    // 编译并应用 BPF 过滤条件。
    CompiledFilter compiled;

    if (!options.filter.empty()) {
        if (pcap_compile(handle.get(), &compiled.program, options.filter.c_str(),
                         1, PCAP_NETMASK_UNKNOWN) != 0) {
            error = std::string("pcap_compile failed: ") +
                    pcap_geterr(handle.get());
            return false;
        }

        compiled.compiled = true;

        if (pcap_setfilter(handle.get(), &compiled.program) != 0) {
            error = std::string("pcap_setfilter failed: ") +
                    pcap_geterr(handle.get());
            return false;
        }
    }

    // 使用非阻塞模式，保证停止操作及时响应。
    if (pcap_setnonblock(handle.get(), 1, errbuf) != 0) {
        error = std::string("pcap_setnonblock failed: ") + errbuf;
        return false;
    }

    // 降低读取阈值，及时获取捕获的数据帧。
    if (pcap_setmintocopy(handle.get(), 0) != 0) {
        error = std::string("pcap_setmintocopy failed: ") +
                pcap_geterr(handle.get());
        return false;
    }

    emit_message(callbacks, "Capture started: " + options.device_name);
    emit_message(callbacks, "Filter: " +
                 (options.filter.empty() ? "(none)" : options.filter));

    const auto start = std::chrono::steady_clock::now();

    // 持续捕获数据帧，直到满足停止条件。
    while (running.load()) {

        // 达到指定帧数后停止。
        if (options.count > 0 && stats.received >= options.count) {
            break;
        }

        // 达到指定捕获时长后停止。
        if (options.duration_seconds > 0 &&
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - start
            ).count() >= static_cast<std::int64_t>(options.duration_seconds)) {
            break;
        }

        pcap_pkthdr* header = nullptr;
        const u_char* packet = nullptr;

        // 从网卡读取下一帧数据。
        const int rc = pcap_next_ex(handle.get(), &header, &packet);

        // 暂时没有数据时短暂休眠。
        if (rc == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }

        // 捕获结束。
        if (rc == -2) {
            break;
        }

        // 处理抓包错误。
        if (rc == -1) {
            error = std::string("pcap_next_ex failed: ") +
                    pcap_geterr(handle.get());
            break;
        }

        if (rc != 1 || header == nullptr || packet == nullptr) {
            error = "Invalid capture result";
            break;
        }

        // 保存数据帧的基本信息。
        PacketRecord record;
        record.sequence = ++stats.received;
        record.local_time = format_timestamp(header->ts);
        record.captured_length = header->caplen;
        record.wire_length = header->len;

        // 解析以太网帧字段。
        record.status = parse_ethernet(packet, header->caplen, record.frame);

        // 统计解析结果。
        if (record.status == ParseStatus::Ok) {
            ++stats.parsed;
        } else {
            ++stats.rejected;
        }

        // 将捕获结果交给显示模块。
        if (callbacks.on_packet) {
            callbacks.on_packet(record);
        }
    }

    // 获取 Npcap 驱动统计信息。
    pcap_stat driver_stats{};

    if (pcap_stats(handle.get(), &driver_stats) == 0) {
        stats.has_driver_stats = true;
        stats.ps_recv = driver_stats.ps_recv;
        stats.ps_drop = driver_stats.ps_drop;
        stats.ps_ifdrop = driver_stats.ps_ifdrop;
    }

    // 输出捕获结束信息。
    emit_message(callbacks,
                 "Capture stopped. Frames=" + std::to_string(stats.received) +
                 ", parsed=" + std::to_string(stats.parsed) +
                 ", skipped=" + std::to_string(stats.rejected));

    if (error_kind != nullptr && error.empty()) {
        *error_kind = CaptureError::None;
    }
    return error.empty();
}

}
