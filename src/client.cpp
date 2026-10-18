#include "protocol.h"
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {
std::mutex output_lock;
std::atomic<bool> running{true};

#ifdef _WIN32
// 将 UTF-8 消息转换为 UTF-16 文本。
std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return {};
    const int chars = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (chars <= 0) return L"[Invalid UTF-8]";
    std::wstring wide(static_cast<std::size_t>(chars), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), wide.data(), chars);
    return wide;
}
// 将键盘输入转换为 UTF-8 文本。
std::string wide_to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return {};
    std::string out(static_cast<std::size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w.data(), static_cast<int>(w.size()), out.data(), bytes, nullptr, nullptr);
    return out;
}
#endif

// 输出客户端接收的文本。
void show(const std::string& message) {
    std::lock_guard<std::mutex> guard(output_lock);
#ifdef _WIN32
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (out != INVALID_HANDLE_VALUE && GetConsoleMode(out, &mode)) {
        const auto wide = utf8_to_wide(message + "\n");
        DWORD written = 0;
        WriteConsoleW(out, wide.data(), static_cast<DWORD>(wide.size()), &written, nullptr);
        return;
    }
#endif
    std::cout << message << std::endl;
}

// 读取一行控制台输入。
bool read_input(std::string& result) {
#ifdef _WIN32
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (in != INVALID_HANDLE_VALUE && GetConsoleMode(in, &mode)) {
        std::wstring wide;
        // 检查接收线程状态。
        while (running.load() && WaitForSingleObject(in, 200) == WAIT_TIMEOUT) { }
        if (!running.load()) return false;
        wchar_t buffer[2048];
        for (;;) {
            DWORD count = 0;
            if (!ReadConsoleW(in, buffer, 2048, &count, nullptr) || count == 0) return false;
            wide.append(buffer, buffer + count);
            if (wide.find(L'\n') != std::wstring::npos) break;
            if (wide.size() > 10000) return false;
        }
        while (!wide.empty() && (wide.back() == L'\n' || wide.back() == L'\r')) wide.pop_back();
        result = wide_to_utf8(wide);
        return true;
    }
#endif
    return static_cast<bool>(std::getline(std::cin, result));
}

// 显示服务器协议消息。
void display_frame(const std::string& frame) {
    if (frame.compare(0, 4, "MSG|") == 0) {
        const auto fields = chat::split_fields(frame, 4);
        if (fields.size() == 5) {
            show("[" + fields[1] + "] " + fields[2] + " (#" + fields[3] + "): " + fields[4]);
            return;
        }
    }
    if (frame.compare(0, 7, "NOTICE|") == 0) {
        const auto fields = chat::split_fields(frame, 2);
        if (fields.size() == 3) { show("[" + fields[1] + "] [SYSTEM] " + fields[2]); return; }
    }
    if (frame.compare(0, 6, "USERS|") == 0) { show("[ONLINE] " + frame.substr(6)); return; }
    if (frame.compare(0, 4, "ERR|") == 0) { show("[ERROR] " + frame.substr(4)); return; }
    if (frame.compare(0, 4, "BYE|") == 0) { show("[BYE] " + frame.substr(4)); running = false; return; }
    if (frame.compare(0, 8, "WELCOME|") == 0) { show("[WELCOME] " + frame.substr(8)); return; }
    show("[RAW] " + frame);
}

// 将登录后的接收缓存交给接收线程。
// 接收线程提取并显示完整协议帧。
void receiver(socket_handle sock, std::string pending) {
    char buffer[4096];
    while (running.load()) {
        const int count = ::recv(sock, buffer, sizeof(buffer), 0);
        if (count <= 0) break;
        std::vector<std::string> frames;
        if (!chat::extract_frames(pending, buffer, static_cast<std::size_t>(count), frames)) {
            show("[ERROR] Incoming frame too long");
            break;
        }
        for (const auto& frame : frames) display_frame(frame);
    }
    if (running.exchange(false)) show("[SYSTEM] Server connection closed; press Enter to exit.");
}

#ifdef _WIN32
struct ReceiveArgs { socket_handle sock; std::string pending; };
// CreateThread 接收线程入口。
DWORD WINAPI receiver_entry(LPVOID ptr) {
    std::unique_ptr<ReceiveArgs> args(static_cast<ReceiveArgs*>(ptr));
    receiver(args->sock, std::move(args->pending));
    return 0;
}
#endif

// 解析命令行端口。
bool parse_port(const std::string& text, unsigned short& port) {
    if (text.empty()) return false;
    char* end = nullptr;
    const auto n = std::strtoul(text.c_str(), &end, 10);
    if (!end || *end || n == 0 || n > 65535) return false;
    port = static_cast<unsigned short>(n);
    return true;
}
}

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
#endif
    if (argc != 4 || !chat::is_valid_name(argv[3])) {
        std::cerr << "Usage: client <IPv4> <port> <name> (ASCII name: 1-16 letters/digits/_)\n";
        return 1;
    }
    unsigned short port = 0;
    if (!parse_port(argv[2], port)) { std::cerr << "Invalid port\n"; return 1; }
    if (!init_sockets()) { std::cerr << "Winsock startup failed\n"; return 2; }
    socket_handle sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == BAD_SOCKET) { cleanup_sockets(); return 2; }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, argv[1], &addr.sin_addr) != 1 ||
        ::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::cerr << "Connection failed: " << socket_last_error() << "\n";
        close_socket(sock);
        cleanup_sockets();
        return 2;
    }
    if (!chat::send_frame(sock, std::string("JOIN|") + argv[3])) {
        close_socket(sock); cleanup_sockets(); return 2;
    }
    std::string pending;
    bool welcomed = false;
    char buffer[4096];
    while (!welcomed) {
        const int count = ::recv(sock, buffer, sizeof(buffer), 0);
        if (count <= 0) break;
        std::vector<std::string> frames;
        if (!chat::extract_frames(pending, buffer, static_cast<std::size_t>(count), frames)) break;
        for (auto& frame : frames) {
            display_frame(frame);
            if (frame.compare(0, 8, "WELCOME|") == 0) welcomed = true;
            if (frame.compare(0, 4, "ERR|") == 0) { close_socket(sock); cleanup_sockets(); return 3; }
        }
    }
    if (!welcomed) { show("[ERROR] Server closed before login"); close_socket(sock); cleanup_sockets(); return 3; }
    show("Commands: /list = online users, /quit = exit; all other lines are chat messages.");
#ifdef _WIN32
    auto* args = new ReceiveArgs{sock, std::move(pending)};
    const HANDLE recv_thread = CreateThread(nullptr, 0, receiver_entry, args, 0, nullptr);
    if (!recv_thread) { delete args; close_socket(sock); cleanup_sockets(); return 2; }
#else
    std::thread recv_thread(receiver, sock, std::move(pending));
#endif
    std::uint64_t seq = 1;
    bool graceful_quit = false;
    std::string input;
    while (running.load() && read_input(input)) {
        if (!running.load()) break;
        if (input == "/quit") {
            graceful_quit = chat::send_frame(sock, "QUIT");
            break;
        }
        if (input == "/list") {
            if (!chat::send_frame(sock, "WHO")) break;
            continue;
        }
        if (!chat::is_valid_body(input)) {
            show("[ERROR] Empty/invalid message or more than 4096 UTF-8 bytes");
            continue;
        }
        if (!chat::send_frame(sock, "TEXT|" + std::to_string(seq) + "|" + input)) break;
        ++seq;
    }
    // 关闭写方向并接收服务器响应。
    if (graceful_quit) shutdown_socket_send(sock);
    else shutdown_socket(sock);
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
