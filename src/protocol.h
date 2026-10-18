#pragma once
#include "net_platform.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace chat {
// 消息正文最大字节数。
constexpr std::size_t MAX_BODY = 4096;
// 协议帧最大字节数。
constexpr std::size_t MAX_FRAME = 8192;
constexpr std::size_t MAX_USERNAME = 16;

// 缓存并提取 LF 结束的协议帧。
bool extract_frames(std::string& pending, const char* data, std::size_t size,
                    std::vector<std::string>& frames);

// 按分隔符拆分协议字段。
std::vector<std::string> split_fields(std::string_view input, std::size_t max_splits);
bool is_valid_name(std::string_view name);
bool is_valid_utf8(std::string_view data);
bool is_valid_body(std::string_view body);
bool parse_sequence(std::string_view text, std::uint64_t& value);
std::string local_timestamp();
bool send_all(socket_handle sock, std::string_view bytes);
bool send_frame(socket_handle sock, std::string_view text);
}
