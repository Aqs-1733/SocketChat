// TCP 聊天服务器。
// 初始化监听 Socket 并接受连接。
// 客户端线程处理协议并广播聊天消息。

#include "protocol.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int MAX_CONNECTIONS = 64;
constexpr std::size_t MAX_USERS = 32;

// 保存客户端会话状态。
struct Session {
    explicit Session(socket_handle socket_value) : sock(socket_value) {}

    socket_handle sock;
    std::string name;
    std::mutex send_lock;
    std::atomic<bool> closed{false};

    // 当前会话线程更新登录状态和序号。
    bool joined = false;
    std::uint64_t next_seq = 1;
};

std::mutex users_lock;
std::map<std::string, std::shared_ptr<Session>> online_users;
std::mutex output_lock;
std::atomic<int> active_connections{0};

// 输出带时间的服务器日志。
void log_line(const std::string& content) {
    std::lock_guard<std::mutex> guard(output_lock);
    std::cout << "[" << chat::local_timestamp() << "] "
              << content << std::endl;
}

// 关闭客户端 Socket。
void disconnect_session(const std::shared_ptr<Session>& session) {
    std::lock_guard<std::mutex> guard(session->send_lock);

    if (!session->closed.exchange(true)) {
        shutdown_socket(session->sock);
    }
    close_socket(session->sock);
}

// 发送一条完整协议帧。
bool send_session(const std::shared_ptr<Session>& session,
                  const std::string& message) {
    std::lock_guard<std::mutex> guard(session->send_lock);

    if (session->closed.load()) {
        return false;
    }
    if (chat::send_frame(session->sock, message)) {
        return true;
    }

    // 关闭发送失败的连接。
    session->closed.store(true);
    shutdown_socket(session->sock);
    return false;
}

// 复制在线用户会话。
std::vector<std::shared_ptr<Session>> snapshot_users() {
    std::lock_guard<std::mutex> guard(users_lock);
    std::vector<std::shared_ptr<Session>> snapshot;

    for (const auto& entry : online_users) {
        snapshot.push_back(entry.second);
    }
    return snapshot;
}

// 广播协议消息。
void broadcast(const std::string& message) {
    for (const auto& client : snapshot_users()) {
        send_session(client, message);
    }
}

// 发送错误协议消息。
void reply_error(const std::shared_ptr<Session>& session,
                 const char* code, const char* reason) {
    send_session(session, std::string("ERR|") + code + "|" + reason);
}

// 处理 JOIN 登录请求。
bool handle_join(const std::shared_ptr<Session>& session,
                 const std::string& name) {
    if (session->joined) {
        reply_error(session, "ALREADY_JOINED", "Already signed in");
        return true;
    }
    if (!chat::is_valid_name(name)) {
        reply_error(session, "BAD_NAME", "Use 1-16 ASCII letters, digits or underscore");
        return true;
    }

    // 在用户表锁外发送协议消息。
    std::string error;
    {
        std::lock_guard<std::mutex> guard(users_lock);
        if (online_users.count(name)) {
            error = "DUP_NAME";
        } else if (online_users.size() >= MAX_USERS) {
            error = "FULL";
        } else {
            session->name = name;
            session->joined = true;
            online_users.emplace(name, session);
        }
    }

    if (error == "DUP_NAME") {
        reply_error(session, "DUP_NAME", "Username already in use");
        return true;
    }
    if (error == "FULL") {
        reply_error(session, "FULL", "Chat room is full");
        return true;
    }

    if (!send_session(session, "WELCOME|" + name + "|" + chat::local_timestamp())) {
        return false;
    }

    log_line(name + " joined; online=" + std::to_string(snapshot_users().size()));
    broadcast("NOTICE|" + chat::local_timestamp() + "|" + name + " joined the room");
    return true;
}

// 检查并广播聊天消息。
void handle_text(const std::shared_ptr<Session>& session,
                 const std::string& line) {
    const auto fields = chat::split_fields(line, 2);
    if (fields.size() != 3) {
        reply_error(session, "BAD_FORMAT", "Use TEXT|seq|content");
        return;
    }

    std::uint64_t seq = 0;
    if (!chat::parse_sequence(fields[1], seq) || seq != session->next_seq) {
        reply_error(session, "BAD_SEQ", "Sequence must increase by 1 starting at 1");
        return;
    }

    if (!chat::is_valid_body(fields[2])) {
        const char* code = fields[2].size() > chat::MAX_BODY
                               ? "TOO_LONG" : "BAD_TEXT";
        reply_error(session, code,
                    "Content must be nonempty valid UTF-8, at most 4096 bytes");
        return;
    }

    const std::string stamp = chat::local_timestamp();
    const std::string message = "MSG|" + stamp + "|" + session->name +
                                "|" + fields[1] + "|" + fields[2];
    broadcast(message);

    log_line("MSG user=" + session->name + " seq=" + fields[1] +
             " bytes=" + std::to_string(fields[2].size()));
    ++session->next_seq;
}

// 生成 USERS 在线用户列表。
void handle_who(const std::shared_ptr<Session>& session) {
    const auto online = snapshot_users();
    std::string reply = "USERS|";

    for (std::size_t index = 0; index < online.size(); ++index) {
        if (index != 0) {
            reply += ",";
        }
        reply += online[index]->name;
    }
    send_session(session, reply);
}

// 分发完整协议消息。
bool handle_frame(const std::shared_ptr<Session>& session,
                  const std::string& frame) {
    if (!chat::is_valid_utf8(frame)) {
        reply_error(session, "BAD_UTF8", "Invalid UTF-8 text");
        return true;
    }

    if (frame.rfind("JOIN|", 0) == 0) {
        return handle_join(session, frame.substr(5));
    }

    if (!session->joined) {
        reply_error(session, "NOT_JOINED", "Please send JOIN|name first");
        return true;
    }

    if (frame.rfind("TEXT|", 0) == 0) {
        handle_text(session, frame);
        return true;
    }

    if (frame == "WHO") {
        handle_who(session);
        return true;
    }

    if (frame == "QUIT") {
        send_session(session, "BYE|" + chat::local_timestamp());
        return false;
    }

    reply_error(session, "BAD_FORMAT", "Unknown command or invalid fields");
    return true;
}

// 客户端线程接收并处理协议帧。
void client_worker(const std::shared_ptr<Session>& session) {
    std::string pending;
    char buffer[4096];
    bool running = true;

    while (running && !session->closed.load()) {
        const int count = ::recv(session->sock, buffer, sizeof(buffer), 0);
        if (count == 0) {
            log_line("Peer closed connection");
            break;
        }
        if (count < 0) {
            log_line("recv failed: " + std::to_string(socket_last_error()));
            break;
        }

        std::vector<std::string> frames;
        if (!chat::extract_frames(
                pending, buffer, static_cast<std::size_t>(count), frames)) {
            reply_error(session, "TOO_LONG", "Protocol frame exceeds 8192 bytes");
            break;
        }

        for (const auto& frame : frames) {
            if (!handle_frame(session, frame)) {
                running = false;
                break;
            }
        }
    }

    // 移除断开用户。
    std::string left_name;
    if (session->joined) {
        std::lock_guard<std::mutex> guard(users_lock);
        const auto it = online_users.find(session->name);
        if (it != online_users.end() && it->second == session) {
            left_name = session->name;
            online_users.erase(it);
        }
    }

    disconnect_session(session);

    if (!left_name.empty()) {
        log_line(left_name + " left; online=" +
                 std::to_string(snapshot_users().size()));
        broadcast("NOTICE|" + chat::local_timestamp() +
                  "|" + left_name + " left the room");
    }
    --active_connections;
}

// 传递工作线程参数。
struct WorkerArgs {
    std::shared_ptr<Session> session;
};

#ifdef _WIN32

// Windows 工作线程入口。
DWORD WINAPI worker_entry(LPVOID parameter) {
    std::unique_ptr<WorkerArgs> args(static_cast<WorkerArgs*>(parameter));
    client_worker(args->session);
    return 0;
}

#else

// 平台兼容工作线程入口。
void worker_entry(WorkerArgs* parameter) {
    std::unique_ptr<WorkerArgs> args(parameter);
    client_worker(args->session);
}

#endif

// 解析端口参数。
bool parse_port(const char* input, unsigned short& port) {
    if (input == nullptr || *input == '\0') {
        return false;
    }

    char* end = nullptr;
    const unsigned long number = std::strtoul(input, &end, 10);
    if (end == nullptr || *end != '\0' || number < 1 || number > 65535) {
        return false;
    }

    port = static_cast<unsigned short>(number);
    return true;
}

}

// 创建监听 Socket 并接受客户端。
int main(int argc, char** argv) {
    unsigned short port = 8888;
    if (argc > 3 ||
        (argc >= 2 && !parse_port(argv[1], port)) ||
        (argc == 3 && std::string(argv[2]) != "--lan")) {
        std::cerr << "Usage: server [port] [--lan]\n";
        return 1;
    }

    if (!init_sockets()) {
        std::cerr << "Winsock initialization failed\n";
        return 2;
    }

    socket_handle listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == BAD_SOCKET) {
        std::cerr << "socket() failed\n";
        cleanup_sockets();
        return 2;
    }

    const int reuse = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(argc == 3 ? INADDR_ANY : INADDR_LOOPBACK);

    if (::bind(listener, reinterpret_cast<const sockaddr*>(&address),
               sizeof(address)) != 0 ||
        ::listen(listener, SOMAXCONN) != 0) {
        std::cerr << "bind()/listen() failed: " << socket_last_error() << '\n';
        close_socket(listener);
        cleanup_sockets();
        return 2;
    }

    const std::string ip = argc == 3 ? "0.0.0.0" : "127.0.0.1";
    log_line("Listening on " + ip + ":" + std::to_string(port));

    for (;;) {
        sockaddr_in remote{};
#ifdef _WIN32
        int address_length = sizeof(remote);
#else
        socklen_t address_length = sizeof(remote);
#endif

        socket_handle client = ::accept(
            listener, reinterpret_cast<sockaddr*>(&remote), &address_length);

        if (client == BAD_SOCKET) {
            log_line("accept() failed: " + std::to_string(socket_last_error()));
            break;
        }

        if (active_connections.fetch_add(1) >= MAX_CONNECTIONS) {
            --active_connections;
            chat::send_frame(client, "ERR|BUSY|Too many connections");
            close_socket(client);
            continue;
        }

        const auto session = std::make_shared<Session>(client);
        auto* args = new WorkerArgs{session};

#ifdef _WIN32
        const HANDLE thread = CreateThread(
            nullptr, 0, worker_entry, args, 0, nullptr);

        if (thread == nullptr) {
            delete args;
            disconnect_session(session);
            --active_connections;
            log_line("CreateThread() failed");
            continue;
        }
        CloseHandle(thread);
#else
        std::thread(worker_entry, args).detach();
#endif
    }

    close_socket(listener);
    cleanup_sockets();
    return 0;
}
