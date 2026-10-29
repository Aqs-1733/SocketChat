#include "protocol.h"

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

std::mutex output_lock;
std::atomic<bool> running{true};

#ifdef _WIN32
// 将 UTF-8 文本显示到 Windows 控制台。
std::wstring utf8_to_wide(const std::string& text) {
    if (text.empty()) return {};
    const int count = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), nullptr, 0);
    if (count <= 0) return L"[UTF-8 解码失败]";
    std::wstring wide(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                        static_cast<int>(text.size()), wide.data(), count);
    return wide;
}

// 将 Windows 控制台输入转换为 UTF-8。
std::string wide_to_utf8(const std::wstring& wide) {
    if (wide.empty()) return {};
    const int count = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
        static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) return {};
    std::string text(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
                        static_cast<int>(wide.size()), text.data(), count,
                        nullptr, nullptr);
    return text;
}
#endif

// 串行输出客户端信息。
void show(const std::string& message) {
    std::lock_guard<std::mutex> guard(output_lock);
#ifdef _WIN32
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (output != INVALID_HANDLE_VALUE && GetConsoleMode(output, &mode)) {
        const std::wstring wide = utf8_to_wide(message + "\n");
        DWORD written = 0;
        WriteConsoleW(output, wide.data(), static_cast<DWORD>(wide.size()),
                      &written, nullptr);
        return;
    }
#endif
    std::cout << message << std::endl;
}

// 读取一行控制台输入。
bool read_input(std::string& result) {
#ifdef _WIN32
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (input != INVALID_HANDLE_VALUE && GetConsoleMode(input, &mode)) {
        std::wstring wide;
        while (running.load() && WaitForSingleObject(input, 200) == WAIT_TIMEOUT) {
        }
        if (!running.load()) return false;

        wchar_t buffer[2048];
        for (;;) {
            DWORD count = 0;
            if (!ReadConsoleW(input, buffer, 2048, &count, nullptr) || count == 0) {
                return false;
            }
            wide.append(buffer, buffer + count);
            if (wide.find(L'\n') != std::wstring::npos) break;
            if (wide.size() > 10000) return false;
        }
        while (!wide.empty() && (wide.back() == L'\n' || wide.back() == L'\r')) {
            wide.pop_back();
        }
        result = wide_to_utf8(wide);
        return true;
    }
#endif
    return static_cast<bool>(std::getline(std::cin, result));
}

// 显示服务器协议帧。
void display_frame(const std::string& frame) {
    if (frame.rfind("MSG|", 0) == 0) {
        const auto fields = chat::split_fields(frame, 4);
        if (fields.size() == 5) {
            show("[" + fields[1] + "] " + fields[2] + " (#" +
                 fields[3] + "): " + fields[4]);
            return;
        }
    }
    if (frame.rfind("PM|", 0) == 0) {
        const auto fields = chat::split_fields(frame, 5);
        if (fields.size() == 6) {
            show("[" + fields[1] + "] [私聊] " + fields[2] + " -> " +
                 fields[3] + " (#" + fields[4] + "): " + fields[5]);
            return;
        }
    }
    if (frame.rfind("GMSG|", 0) == 0) {
        const auto fields = chat::split_fields(frame, 5);
        if (fields.size() == 6) {
            show("[" + fields[1] + "] [群 " + fields[2] + "] " +
                 fields[3] + " (#" + fields[4] + "): " + fields[5]);
            return;
        }
    }
    if (frame.rfind("GNOTICE|", 0) == 0) {
        const auto fields = chat::split_fields(frame, 3);
        if (fields.size() == 4) {
            show("[" + fields[1] + "] [群 " + fields[2] + "] " + fields[3]);
            return;
        }
    }
    if (frame.rfind("GROUP_OK|", 0) == 0) {
        show("[群组操作成功] " + frame.substr(9));
        return;
    }
    if (frame.rfind("GROUPS|", 0) == 0) {
        show("[群组列表] " + frame.substr(7));
        return;
    }
    if (frame.rfind("NOTICE|", 0) == 0) {
        const auto fields = chat::split_fields(frame, 2);
        if (fields.size() == 3) {
            show("[" + fields[1] + "] [系统] " + fields[2]);
            return;
        }
    }
    if (frame.rfind("USERS|", 0) == 0) {
        show("[在线用户] " + frame.substr(6));
        return;
    }
    if (frame.rfind("ERR|", 0) == 0) {
        show("[错误] " + frame.substr(4));
        return;
    }
    if (frame.rfind("BYE|", 0) == 0) {
        show("[退出确认] " + frame.substr(4));
        running.store(false);
        return;
    }
    if (frame.rfind("WELCOME|", 0) == 0) {
        show("[登录成功] " + frame.substr(8));
        return;
    }
    show("[协议帧] " + frame);
}

// 连续接收 TCP 字节并提取完整协议帧。
void receiver(socket_handle sock, std::string pending) {
    char buffer[4096];
    while (running.load()) {
        const int count = ::recv(sock, buffer, sizeof(buffer), 0);
        if (count <= 0) break;

        std::vector<std::string> frames;
        if (!chat::extract_frames(pending, buffer,
                                  static_cast<std::size_t>(count), frames)) {
            show("[错误] 接收帧超过长度限制");
            break;
        }
        for (const auto& frame : frames) display_frame(frame);
    }
    if (running.exchange(false)) {
        show("[系统] 服务器连接已关闭，按 Enter 结束程序。");
    }
}

#ifdef _WIN32
struct ReceiveArgs {
    socket_handle sock;
    std::string pending;
};

// CreateThread 的接收线程入口。
DWORD WINAPI receiver_entry(LPVOID parameter) {
    std::unique_ptr<ReceiveArgs> args(static_cast<ReceiveArgs*>(parameter));
    receiver(args->sock, std::move(args->pending));
    return 0;
}
#endif

// 解析命令行端口。
bool parse_port(const std::string& text, unsigned short& port) {
    if (text.empty()) return false;
    char* end = nullptr;
    const unsigned long value = std::strtoul(text.c_str(), &end, 10);
    if (end == nullptr || *end != '\0' || value == 0 || value > 65535) {
        return false;
    }
    port = static_cast<unsigned short>(value);
    return true;
}

// 提取命令后的两个参数和正文。
bool split_command(const std::string& input, std::string& first,
                   std::string& second, std::string& body) {
    const std::size_t first_space = input.find(' ');
    if (first_space == std::string::npos) return false;
    first = input.substr(0, first_space);

    const std::size_t second_start = input.find_first_not_of(' ', first_space);
    if (second_start == std::string::npos) return false;
    const std::size_t second_space = input.find(' ', second_start);
    if (second_space == std::string::npos) {
        second = input.substr(second_start);
        body.clear();
        return true;
    }
    second = input.substr(second_start, second_space - second_start);
    const std::size_t body_start = input.find_first_not_of(' ', second_space);
    body = body_start == std::string::npos ? "" : input.substr(body_start);
    return true;
}

// 显示命令行客户端可用指令。
void show_help() {
    show("命令：/list  /groups  /pm 用户 内容  /group create|join|leave 群名");
    show("      /g 群名 内容  /quit；其余输入发送到公共聊天室。");
}

// 根据输入构造一条应用层协议帧。
bool send_input(socket_handle sock, const std::string& input,
                std::uint64_t& sequence, bool& quitting) {
    if (input == "/quit") {
        quitting = chat::send_frame(sock, "QUIT");
        return quitting;
    }
    if (input == "/help") {
        show_help();
        return true;
    }
    if (input == "/list") return chat::send_frame(sock, "WHO");
    if (input == "/groups") return chat::send_frame(sock, "GROUP|LIST");

    std::string command;
    std::string target;
    std::string body;
    if (split_command(input, command, target, body)) {
        if (command == "/pm") {
            if (!chat::is_valid_name(target) || !chat::is_valid_body(body)) {
                show("[错误] 用法：/pm 用户 内容");
                return true;
            }
            const bool sent = chat::send_frame(
                sock, "PM|" + std::to_string(sequence) + "|" + target + "|" + body);
            if (sent) ++sequence;
            return sent;
        }
        if (command == "/g") {
            if (!chat::is_valid_name(target) || !chat::is_valid_body(body)) {
                show("[错误] 用法：/g 群名 内容");
                return true;
            }
            const bool sent = chat::send_frame(
                sock, "GTEXT|" + std::to_string(sequence) + "|" + target + "|" + body);
            if (sent) ++sequence;
            return sent;
        }
        if (command == "/group" &&
            (target == "create" || target == "join" || target == "leave") &&
            chat::is_valid_name(body)) {
            const std::string action = target == "create" ? "CREATE" :
                                       target == "join" ? "JOIN" : "LEAVE";
            return chat::send_frame(sock, "GROUP|" + action + "|" + body);
        }
    }

    if (!chat::is_valid_body(input)) {
        show("[错误] 消息应为 1–4096 字节的有效 UTF-8 单行文本。");
        return true;
    }
    const bool sent = chat::send_frame(
        sock, "TEXT|" + std::to_string(sequence) + "|" + input);
    if (sent) ++sequence;
    return sent;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
#endif
    if (argc != 4 || !chat::is_valid_name(argv[3])) {
        std::cerr << "Usage: client <IPv4> <port> <name>\n";
        return 1;
    }

    unsigned short port = 0;
    if (!parse_port(argv[2], port)) {
        std::cerr << "Invalid port\n";
        return 1;
    }
    if (!init_sockets()) {
        std::cerr << "Winsock startup failed\n";
        return 2;
    }

    socket_handle sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == BAD_SOCKET) {
        cleanup_sockets();
        return 2;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, argv[1], &address.sin_addr) != 1 ||
        ::connect(sock, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        std::cerr << "Connection failed: " << socket_last_error() << "\n";
        close_socket(sock);
        cleanup_sockets();
        return 2;
    }

    if (!chat::send_frame(sock, std::string("JOIN|") + argv[3])) {
        close_socket(sock);
        cleanup_sockets();
        return 2;
    }

    std::string pending;
    bool welcomed = false;
    char buffer[4096];
    while (!welcomed) {
        const int count = ::recv(sock, buffer, sizeof(buffer), 0);
        if (count <= 0) break;

        std::vector<std::string> frames;
        if (!chat::extract_frames(pending, buffer,
                                  static_cast<std::size_t>(count), frames)) {
            break;
        }
        for (const auto& frame : frames) {
            display_frame(frame);
            if (frame.rfind("WELCOME|", 0) == 0) welcomed = true;
            if (frame.rfind("ERR|", 0) == 0) {
                close_socket(sock);
                cleanup_sockets();
                return 3;
            }
        }
    }
    if (!welcomed) {
        show("[错误] 登录前服务器关闭了连接");
        close_socket(sock);
        cleanup_sockets();
        return 3;
    }

    show_help();
#ifdef _WIN32
    auto* args = new ReceiveArgs{sock, std::move(pending)};
    const HANDLE recv_thread = CreateThread(nullptr, 0, receiver_entry, args, 0, nullptr);
    if (!recv_thread) {
        delete args;
        close_socket(sock);
        cleanup_sockets();
        return 2;
    }
#else
    std::thread recv_thread(receiver, sock, std::move(pending));
#endif

    std::uint64_t sequence = 1;
    bool graceful_quit = false;
    std::string input;
    while (running.load() && read_input(input)) {
        if (!running.load()) break;
        if (!send_input(sock, input, sequence, graceful_quit)) break;
        if (graceful_quit) break;
    }

    if (graceful_quit) {
        shutdown_socket_send(sock);
    } else {
        shutdown_socket(sock);
    }
#ifdef _WIN32
    WaitForSingleObject(recv_thread, INFINITE);
    CloseHandle(recv_thread);
#else
    recv_thread.join();
#endif
    close_socket(sock);
    cleanup_sockets();
    return 0;
}
