#include "protocol.h"
#include <ctime>
#include <cstdio>
#include <limits>

namespace chat {
// 将接收字节拆分为完整协议帧。
// TCP 字节流可能包含半帧或多帧。
bool extract_frames(std::string& pending, const char* data, std::size_t size,
                    std::vector<std::string>& frames) {
    if (size && !data) return false;
    pending.append(data, size);
    while (true) {
        const std::size_t pos = pending.find('\n');
        if (pos == std::string::npos) break;
        std::size_t end = pos;
        if (end && pending[end - 1] == '\r') --end;
        if (end > MAX_FRAME) return false;
        frames.emplace_back(pending.substr(0, end));
        pending.erase(0, pos + 1);
    }
    return pending.size() <= MAX_FRAME;
}

// 分割协议字段。
std::vector<std::string> split_fields(std::string_view input, std::size_t max_splits) {
    std::vector<std::string> result;
    std::size_t start = 0;
    for (std::size_t i = 0; i < max_splits; ++i) {
        const std::size_t pos = input.find('|', start);
        if (pos == std::string_view::npos) break;
        result.emplace_back(input.substr(start, pos - start));
        start = pos + 1;
    }
    result.emplace_back(input.substr(start));
    return result;
}

// 校验用户名字符和长度。
bool is_valid_name(std::string_view name) {
    if (name.empty() || name.size() > MAX_USERNAME) return false;
    for (const unsigned char c : name) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_')) return false;
    }
    return true;
}

// 校验 UTF-8 字节序列。
bool is_valid_utf8(std::string_view data) {
    for (std::size_t i = 0; i < data.size();) {
        const unsigned char c = static_cast<unsigned char>(data[i]);
        if (c < 0x80) { ++i; continue; }
        int continuation = 0;
        std::uint32_t code = 0;
        if ((c & 0xE0) == 0xC0) { continuation = 1; code = c & 0x1F; if (code == 0) return false; }
        else if ((c & 0xF0) == 0xE0) { continuation = 2; code = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { continuation = 3; code = c & 0x07; }
        else return false;
        if (i + static_cast<std::size_t>(continuation) >= data.size()) return false;
        for (int j = 1; j <= continuation; ++j) {
            const unsigned char b = static_cast<unsigned char>(data[i + j]);
            if ((b & 0xC0) != 0x80) return false;
            code = (code << 6) | (b & 0x3F);
        }
        if ((continuation == 1 && code < 0x80) || (continuation == 2 && code < 0x800) ||
            (continuation == 3 && code < 0x10000) || code > 0x10FFFF ||
            (code >= 0xD800 && code <= 0xDFFF)) return false;
        i += static_cast<std::size_t>(continuation) + 1;
    }
    return true;
}

// 校验消息正文。
bool is_valid_body(std::string_view body) {
    if (body.empty() || body.size() > MAX_BODY || !is_valid_utf8(body)) return false;
    for (unsigned char c : body) {
        if (c == '\n' || c == '\r' || c == 0 || (c < 0x20 && c != '\t') || c == 0x7F) return false;
    }
    return true;
}

// 解析消息序号。
bool parse_sequence(std::string_view text, std::uint64_t& value) {
    if (text.empty() || text.size() > 20) return false;
    std::uint64_t n = 0;
    for (char c : text) {
        if (c < '0' || c > '9') return false;
        const auto digit = static_cast<unsigned>(c - '0');
        if (n > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) return false;
        n = n * 10 + digit;
    }
    if (n == 0) return false;
    value = n;
    return true;
}

// 生成本地时间字符串。
std::string local_timestamp() {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char output[32]{};
    std::strftime(output, sizeof(output), "%Y-%m-%d %H:%M:%S", &tm);
    return output;
}

// 循环发送剩余字节。
bool send_all(socket_handle sock, std::string_view bytes) {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        const auto remaining = bytes.size() - sent;
        // 处理 send 返回值。
        const int requested = static_cast<int>(remaining > 16384 ? 16384 : remaining);
        #ifdef _WIN32
        const int send_flags = 0;
#else
        const int send_flags = MSG_NOSIGNAL;
#endif
        const int n = ::send(sock, bytes.data() + sent, requested, send_flags);
        if (n <= 0) return false;
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

// 发送以 LF 结束的协议帧。
bool send_frame(socket_handle sock, std::string_view text) {
    if (text.size() > MAX_FRAME || text.find('\r') != std::string_view::npos ||
        text.find('\n') != std::string_view::npos) return false;
    std::string packet(text);
    packet += '\n';
    return send_all(sock, packet);
}
}
