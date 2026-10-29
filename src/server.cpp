#include "protocol.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int MAX_CONNECTIONS = 64;
constexpr std::size_t MAX_USERS = 32;
constexpr std::size_t MAX_GROUPS = 32;

struct Session {
    explicit Session(socket_handle socket_value) : sock(socket_value) {}

    socket_handle sock;
    std::string name;
    std::mutex send_lock;
    std::atomic<bool> closed{false};
    bool joined = false;
    std::uint64_t next_seq = 1;
};

using SessionPtr = std::shared_ptr<Session>;

std::mutex state_lock;
std::map<std::string, SessionPtr> online_users;
std::map<std::string, std::set<std::string>> chat_groups;
std::mutex output_lock;
std::atomic<int> active_connections{0};

// 输出服务器事件。
void log_line(const std::string& text) {
    std::lock_guard<std::mutex> guard(output_lock);
    std::cout << "[" << chat::local_timestamp() << "] " << text << std::endl;
}

// 关闭一个客户端连接。
void disconnect_session(const SessionPtr& session) {
    std::lock_guard<std::mutex> guard(session->send_lock);
    if (!session->closed.exchange(true)) {
        shutdown_socket(session->sock);
    }
    close_socket(session->sock);
}

// 向单个客户端发送完整帧。
bool send_session(const SessionPtr& session, const std::string& message) {
    std::lock_guard<std::mutex> guard(session->send_lock);
    if (session->closed.load()) {
        return false;
    }
    if (chat::send_frame(session->sock, message)) {
        return true;
    }
    session->closed.store(true);
    shutdown_socket(session->sock);
    return false;
}

// 复制当前在线用户。
std::vector<SessionPtr> online_snapshot() {
    std::lock_guard<std::mutex> guard(state_lock);
    std::vector<SessionPtr> users;
    for (const auto& item : online_users) {
        users.push_back(item.second);
    }
    return users;
}

// 复制指定群的在线成员。
std::vector<SessionPtr> group_snapshot(const std::string& group_name) {
    std::lock_guard<std::mutex> guard(state_lock);
    std::vector<SessionPtr> users;
    const auto group = chat_groups.find(group_name);
    if (group == chat_groups.end()) {
        return users;
    }
    for (const std::string& user_name : group->second) {
        const auto user = online_users.find(user_name);
        if (user != online_users.end()) {
            users.push_back(user->second);
        }
    }
    return users;
}

// 向全部在线用户广播。
void broadcast(const std::string& message) {
    for (const SessionPtr& user : online_snapshot()) {
        send_session(user, message);
    }
}

// 向指定群成员广播。
void broadcast_group(const std::string& group_name, const std::string& message) {
    for (const SessionPtr& user : group_snapshot(group_name)) {
        send_session(user, message);
    }
}

// 返回协议错误帧。
void reply_error(const SessionPtr& session, const char* code, const char* reason) {
    send_session(session, std::string("ERR|") + code + "|" + reason);
}

// 校验客户端消息序号。
bool accept_next_sequence(const SessionPtr& session, const std::string& text,
                          std::uint64_t& sequence) {
    if (!chat::parse_sequence(text, sequence) || sequence != session->next_seq) {
        reply_error(session, "BAD_SEQ", "Sequence must increase by 1 starting at 1");
        return false;
    }
    return true;
}

// 登记登录用户并发送上线通知。
bool handle_join(const SessionPtr& session, const std::string& name) {
    if (session->joined) {
        reply_error(session, "ALREADY_JOINED", "Already signed in");
        return true;
    }
    if (!chat::is_valid_name(name)) {
        reply_error(session, "BAD_NAME", "Use 1-16 ASCII letters, digits or underscore");
        return true;
    }

    std::string error;
    {
        std::lock_guard<std::mutex> guard(state_lock);
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
    log_line(name + " joined; online=" + std::to_string(online_snapshot().size()));
    broadcast("NOTICE|" + chat::local_timestamp() + "|" + name + " joined the room");
    return true;
}

// 处理公共聊天室消息。
void handle_text(const SessionPtr& session, const std::string& line) {
    const auto fields = chat::split_fields(line, 2);
    if (fields.size() != 3) {
        reply_error(session, "BAD_FORMAT", "Use TEXT|seq|content");
        return;
    }

    std::uint64_t sequence = 0;
    if (!accept_next_sequence(session, fields[1], sequence)) {
        return;
    }
    ++session->next_seq;
    if (!chat::is_valid_body(fields[2])) {
        reply_error(session, fields[2].size() > chat::MAX_BODY ? "TOO_LONG" : "BAD_TEXT",
                    "Content must be valid UTF-8 and at most 4096 bytes");
        return;
    }

    const std::string stamp = chat::local_timestamp();
    broadcast("MSG|" + stamp + "|" + session->name + "|" + fields[1] + "|" + fields[2]);
    log_line("MSG user=" + session->name + " seq=" + fields[1]);
}

// 处理指定用户的私聊消息。
void handle_private_message(const SessionPtr& session, const std::string& line) {
    const auto fields = chat::split_fields(line, 3);
    if (fields.size() != 4) {
        reply_error(session, "BAD_FORMAT", "Use PM|seq|user|content");
        return;
    }

    std::uint64_t sequence = 0;
    if (!accept_next_sequence(session, fields[1], sequence)) {
        return;
    }
    ++session->next_seq;
    if (!chat::is_valid_name(fields[2])) {
        reply_error(session, "BAD_NAME", "Invalid private-message target");
        return;
    }
    if (!chat::is_valid_body(fields[3])) {
        reply_error(session, fields[3].size() > chat::MAX_BODY ? "TOO_LONG" : "BAD_TEXT",
                    "Content must be valid UTF-8 and at most 4096 bytes");
        return;
    }

    SessionPtr target;
    {
        std::lock_guard<std::mutex> guard(state_lock);
        const auto user = online_users.find(fields[2]);
        if (user != online_users.end()) {
            target = user->second;
        }
    }
    if (!target) {
        reply_error(session, "NO_USER", "Private-message target is offline");
        return;
    }

    const std::string message = "PM|" + chat::local_timestamp() + "|" + session->name +
                                "|" + fields[2] + "|" + fields[1] + "|" + fields[3];
    send_session(session, message);
    if (target != session) {
        send_session(target, message);
    }
    log_line("PM from=" + session->name + " to=" + fields[2] + " seq=" + fields[1]);
}

// 处理建群、入群、退群和群列表请求。
void handle_group_control(const SessionPtr& session, const std::string& line) {
    const auto fields = chat::split_fields(line, 3);
    if (fields.size() < 2) {
        reply_error(session, "BAD_FORMAT", "Use GROUP|CREATE|name, GROUP|ADD|name|user, GROUP|JOIN|name, GROUP|LEAVE|name or GROUP|LIST");
        return;
    }

    const std::string& action = fields[1];
    if (action == "LIST") {
        if (fields.size() != 2) {
            reply_error(session, "BAD_FORMAT", "Use GROUP|LIST");
            return;
        }
        std::string reply = "GROUPS|";
        {
            std::lock_guard<std::mutex> guard(state_lock);
            bool first = true;
            for (const auto& item : chat_groups) {
                if (!first) reply += ',';
                reply += item.first;
                first = false;
            }
        }
        send_session(session, reply);
        return;
    }

    if (action == "ADD") {
        if (fields.size() != 4 || !chat::is_valid_name(fields[2]) ||
            !chat::is_valid_name(fields[3])) {
            reply_error(session, "BAD_FORMAT", "Use GROUP|ADD|name|user");
            return;
        }

        const std::string& group_name = fields[2];
        const std::string& target_name = fields[3];
        std::string error;
        {
            std::lock_guard<std::mutex> guard(state_lock);
            const auto group = chat_groups.find(group_name);
            if (group == chat_groups.end()) {
                error = "NO_GROUP";
            } else if (group->second.count(session->name) == 0) {
                error = "NOT_MEMBER";
            } else if (online_users.count(target_name) == 0) {
                error = "NO_USER";
            } else if (!group->second.insert(target_name).second) {
                error = "ALREADY_MEMBER";
            }
        }
        if (error == "NO_GROUP") {
            reply_error(session, "NO_GROUP", "Group does not exist");
            return;
        }
        if (error == "NOT_MEMBER") {
            reply_error(session, "NOT_MEMBER", "Join the group before adding members");
            return;
        }
        if (error == "NO_USER") {
            reply_error(session, "NO_USER", "Selected user is offline");
            return;
        }
        if (error == "ALREADY_MEMBER") {
            reply_error(session, "ALREADY_MEMBER", "Selected user is already in this group");
            return;
        }

        send_session(session, "GROUP_OK|ADD|" + group_name + "|" + target_name);
        broadcast_group(group_name, "GNOTICE|" + chat::local_timestamp() + "|" +
                        group_name + "|" + session->name + " added " + target_name);
        log_line("GROUP add=" + group_name + " user=" + target_name +
                 " by=" + session->name);
        return;
    }

    if (fields.size() != 3 || !chat::is_valid_name(fields[2])) {
        reply_error(session, "BAD_GROUP", "Group name uses 1-16 letters, digits or underscore");
        return;
    }

    const std::string& group_name = fields[2];
    if (action == "CREATE") {
        std::string error;
        {
            std::lock_guard<std::mutex> guard(state_lock);
            if (chat_groups.count(group_name)) {
                error = "GROUP_EXISTS";
            } else if (chat_groups.size() >= MAX_GROUPS) {
                error = "GROUP_FULL";
            } else {
                chat_groups[group_name].insert(session->name);
            }
        }
        if (error == "GROUP_EXISTS") {
            reply_error(session, "GROUP_EXISTS", "Group name is already in use");
            return;
        }
        if (error == "GROUP_FULL") {
            reply_error(session, "GROUP_FULL", "Group limit reached");
            return;
        }
        send_session(session, "GROUP_OK|CREATE|" + group_name);
        broadcast_group(group_name, "GNOTICE|" + chat::local_timestamp() + "|" +
                        group_name + "|" + session->name + " created and joined the group");
        log_line("GROUP create=" + group_name + " owner=" + session->name);
        return;
    }

    if (action == "JOIN") {
        std::string error;
        {
            std::lock_guard<std::mutex> guard(state_lock);
            const auto group = chat_groups.find(group_name);
            if (group == chat_groups.end()) {
                error = "NO_GROUP";
            } else if (!group->second.insert(session->name).second) {
                error = "ALREADY_MEMBER";
            }
        }
        if (error == "NO_GROUP") {
            reply_error(session, "NO_GROUP", "Group does not exist");
            return;
        }
        if (error == "ALREADY_MEMBER") {
            reply_error(session, "ALREADY_MEMBER", "Already in this group");
            return;
        }
        send_session(session, "GROUP_OK|JOIN|" + group_name);
        broadcast_group(group_name, "GNOTICE|" + chat::local_timestamp() + "|" +
                        group_name + "|" + session->name + " joined the group");
        log_line("GROUP join=" + group_name + " user=" + session->name);
        return;
    }

    if (action == "LEAVE") {
        std::string error;
        bool group_removed = false;
        {
            std::lock_guard<std::mutex> guard(state_lock);
            const auto group = chat_groups.find(group_name);
            if (group == chat_groups.end()) {
                error = "NO_GROUP";
            } else if (group->second.erase(session->name) == 0) {
                error = "NOT_MEMBER";
            } else if (group->second.empty()) {
                chat_groups.erase(group);
                group_removed = true;
            }
        }
        if (error == "NO_GROUP") {
            reply_error(session, "NO_GROUP", "Group does not exist");
            return;
        }
        if (error == "NOT_MEMBER") {
            reply_error(session, "NOT_MEMBER", "Join the group before leaving it");
            return;
        }
        send_session(session, "GROUP_OK|LEAVE|" + group_name);
        if (!group_removed) {
            broadcast_group(group_name, "GNOTICE|" + chat::local_timestamp() + "|" +
                            group_name + "|" + session->name + " left the group");
        }
        log_line("GROUP leave=" + group_name + " user=" + session->name);
        return;
    }

    reply_error(session, "BAD_FORMAT", "Unknown GROUP action");
}
// 处理群聊消息。
void handle_group_text(const SessionPtr& session, const std::string& line) {
    const auto fields = chat::split_fields(line, 3);
    if (fields.size() != 4) {
        reply_error(session, "BAD_FORMAT", "Use GTEXT|seq|group|content");
        return;
    }

    std::uint64_t sequence = 0;
    if (!accept_next_sequence(session, fields[1], sequence)) {
        return;
    }
    ++session->next_seq;
    if (!chat::is_valid_name(fields[2])) {
        reply_error(session, "BAD_GROUP", "Invalid group name");
        return;
    }
    if (!chat::is_valid_body(fields[3])) {
        reply_error(session, fields[3].size() > chat::MAX_BODY ? "TOO_LONG" : "BAD_TEXT",
                    "Content must be valid UTF-8 and at most 4096 bytes");
        return;
    }

    bool member = false;
    {
        std::lock_guard<std::mutex> guard(state_lock);
        const auto group = chat_groups.find(fields[2]);
        member = group != chat_groups.end() && group->second.count(session->name) != 0;
    }
    if (!member) {
        reply_error(session, "NOT_MEMBER", "Join the group before sending a group message");
        return;
    }

    const std::string message = "GMSG|" + chat::local_timestamp() + "|" + fields[2] +
                                "|" + session->name + "|" + fields[1] + "|" + fields[3];
    broadcast_group(fields[2], message);
    log_line("GMSG group=" + fields[2] + " user=" + session->name + " seq=" + fields[1]);
}

// 返回在线用户列表。
void handle_who(const SessionPtr& session) {
    std::string reply = "USERS|";
    const auto users = online_snapshot();
    for (std::size_t index = 0; index < users.size(); ++index) {
        if (index != 0) {
            reply += ',';
        }
        reply += users[index]->name;
    }
    send_session(session, reply);
}

// 分派一条完整协议帧。
bool handle_frame(const SessionPtr& session, const std::string& frame) {
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
    if (frame.rfind("PM|", 0) == 0) {
        handle_private_message(session, frame);
        return true;
    }
    if (frame.rfind("GROUP|", 0) == 0) {
        handle_group_control(session, frame);
        return true;
    }
    if (frame.rfind("GTEXT|", 0) == 0) {
        handle_group_text(session, frame);
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

// 循环接收一个客户端的 TCP 字节流。
void client_worker(const SessionPtr& session) {
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
        if (!chat::extract_frames(pending, buffer, static_cast<std::size_t>(count), frames)) {
            reply_error(session, "TOO_LONG", "Protocol frame exceeds 8192 bytes");
            break;
        }
        for (const std::string& frame : frames) {
            if (!handle_frame(session, frame)) {
                running = false;
                break;
            }
        }
    }

    std::string left_name;
    std::vector<std::string> left_groups;
    {
        std::lock_guard<std::mutex> guard(state_lock);
        if (session->joined) {
            const auto user = online_users.find(session->name);
            if (user != online_users.end() && user->second == session) {
                left_name = session->name;
                online_users.erase(user);
            }
            for (auto group = chat_groups.begin(); group != chat_groups.end();) {
                if (group->second.erase(session->name) != 0 && !group->second.empty()) {
                    left_groups.push_back(group->first);
                }
                if (group->second.empty()) {
                    group = chat_groups.erase(group);
                } else {
                    ++group;
                }
            }
        }
    }

    disconnect_session(session);
    for (const std::string& group_name : left_groups) {
        broadcast_group(group_name, "GNOTICE|" + chat::local_timestamp() + "|" +
                        group_name + "|" + left_name + " left the group");
    }
    if (!left_name.empty()) {
        log_line(left_name + " left; online=" + std::to_string(online_snapshot().size()));
        broadcast("NOTICE|" + chat::local_timestamp() + "|" + left_name + " left the room");
    }
    --active_connections;
}

struct WorkerArgs {
    SessionPtr session;
};

#ifdef _WIN32
// CreateThread 的工作线程入口。
DWORD WINAPI worker_entry(LPVOID parameter) {
    std::unique_ptr<WorkerArgs> args(static_cast<WorkerArgs*>(parameter));
    client_worker(args->session);
    return 0;
}
#else
void worker_entry(WorkerArgs* parameter) {
    std::unique_ptr<WorkerArgs> args(parameter);
    client_worker(args->session);
}
#endif

// 解析服务器监听端口。
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

// 初始化 Winsock 并监听客户端连接。
int main(int argc, char** argv) {
    unsigned short port = 8888;
    if (argc > 3 || (argc >= 2 && !parse_port(argv[1], port)) ||
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
    if (::bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
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
        socket_handle client = ::accept(listener, reinterpret_cast<sockaddr*>(&remote),
                                        &address_length);
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

        const SessionPtr session = std::make_shared<Session>(client);
        auto* args = new WorkerArgs{session};
#ifdef _WIN32
        const HANDLE thread = CreateThread(nullptr, 0, worker_entry, args, 0, nullptr);
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
