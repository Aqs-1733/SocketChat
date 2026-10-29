// 多人聊天室自动验收程序。
// 使用 TCP Socket 连接 server.exe。
// 用法：integration_tests.exe 127.0.0.1 8889
#include "../src/protocol.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

struct Peer {
    socket_handle sock = BAD_SOCKET;
    std::string pending;
    std::vector<std::string> queued;
};

static int passes = 0;
static int fails = 0;

// 输出测试结果。
void check(bool value, const std::string& title) {
    std::cout << (value ? "[PASS] " : "[FAIL] ") << title << '\n';
    if (value) ++passes; else ++fails;
}

// 初始化 TCP 客户端。
bool connect_peer(Peer& peer, const char* host, unsigned short port) {
    peer.sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (peer.sock == BAD_SOCKET) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1 ||
        ::connect(peer.sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) return false;
#ifdef _WIN32
    DWORD ms = 4000;
    setsockopt(peer.sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
#else
    timeval t{}; t.tv_sec = 4;
    setsockopt(peer.sock, SOL_SOCKET, SO_RCVTIMEO, &t, sizeof(t));
#endif
    return true;
}

// 从 TCP 字节流读取协议行。
bool next_frame(Peer& peer, std::string& out) {
    if (!peer.queued.empty()) {
        out = peer.queued.front(); peer.queued.erase(peer.queued.begin()); return true;
    }
    char buf[4096];
    for (;;) {
        const int n = ::recv(peer.sock, buf, sizeof(buf), 0);
        if (n <= 0) return false;
        std::vector<std::string> frames;
        if (!chat::extract_frames(peer.pending, buf, static_cast<std::size_t>(n), frames)) return false;
        for (auto& frame : frames) peer.queued.push_back(std::move(frame));
        if (!peer.queued.empty()) {
            out = peer.queued.front(); peer.queued.erase(peer.queued.begin()); return true;
        }
    }
}

// 查找指定类型的协议响应。
bool expect_prefix(Peer& peer, const std::string& prefix, std::string* matched = nullptr) {
    for (int i = 0; i < 400; ++i) {
        std::string line;
        if (!next_frame(peer, line)) return false;
        if (line.compare(0, prefix.size(), prefix) == 0) {
            if (matched) *matched = line;
            return true;
        }
    }
    return false;
}

// 发送 JOIN 并接收 WELCOME。
bool login(Peer& peer, const char* host, unsigned short port, const std::string& user) {
    return connect_peer(peer, host, port) && chat::send_frame(peer.sock, "JOIN|" + user) &&
           expect_prefix(peer, "WELCOME|" + user + "|");
}

// 关闭测试客户端连接。
void close_peer(Peer& peer) {
    if (peer.sock != BAD_SOCKET) {
        shutdown_socket(peer.sock);
        close_socket(peer.sock);
        peer.sock = BAD_SOCKET;
    }
}

// 检查指定发送者的广播。
bool get_msg(Peer& peer, const std::string& who, const std::string& seq, const std::string& body) {
    for (int tries = 0; tries < 100; ++tries) {
        std::string line;
        if (!next_frame(peer, line)) return false;
        if (line.rfind("MSG|", 0) != 0) continue;
        const auto f = chat::split_fields(line, 4);
        if (f.size() == 5 && f[2] == who && f[3] == seq && f[4] == body && f[1].size() == 19)
            return true;
    }
    return false;
}

// 检查私聊帧是否包含预期双方和正文。
bool get_private_message(Peer& peer, const std::string& sender,
                         const std::string& receiver, const std::string& seq,
                         const std::string& body) {
    for (int tries = 0; tries < 100; ++tries) {
        std::string line;
        if (!next_frame(peer, line)) return false;
        if (line.rfind("PM|", 0) != 0) continue;
        const auto fields = chat::split_fields(line, 5);
        if (fields.size() == 6 && fields[2] == sender && fields[3] == receiver &&
            fields[4] == seq && fields[5] == body && fields[1].size() == 19) {
            return true;
        }
    }
    return false;
}

// 检查群消息帧是否包含预期群名和正文。
bool get_group_message(Peer& peer, const std::string& group,
                       const std::string& sender, const std::string& seq,
                       const std::string& body) {
    for (int tries = 0; tries < 100; ++tries) {
        std::string line;
        if (!next_frame(peer, line)) return false;
        if (line.rfind("GMSG|", 0) != 0) continue;
        const auto fields = chat::split_fields(line, 5);
        if (fields.size() == 6 && fields[2] == group && fields[3] == sender &&
            fields[4] == seq && fields[5] == body && fields[1].size() == 19) {
            return true;
        }
    }
    return false;
}
int main(int argc, char** argv) {
    if (argc != 3) { std::cerr << "Usage: integration_tests <IPv4> <port>\n"; return 2; }
    const char* host = argv[1];
    const unsigned short port = static_cast<unsigned short>(std::atoi(argv[2]));
    if (!port || !init_sockets()) return 2;
    Peer a, b, c, duplicate, illegal, not_joined;
    check(login(a, host, port, "Alice"), "T01 Alice TCP connection and JOIN/WELCOME");
    check(login(b, host, port, "Bob"), "T02 Bob online concurrently");
    check(login(c, host, port, "Charlie"), "T03 Charlie online concurrently");
    check(connect_peer(duplicate, host, port) && chat::send_frame(duplicate.sock, "JOIN|Alice") &&
        expect_prefix(duplicate, "ERR|DUP_NAME|"), "T04 duplicate nickname rejected");
    close_peer(duplicate);
    check(connect_peer(illegal, host, port) && chat::send_frame(illegal.sock, "JOIN|bad-name") &&
        expect_prefix(illegal, "ERR|BAD_NAME|"), "T05 illegal nickname rejected");
    close_peer(illegal);
    check(connect_peer(not_joined, host, port) && chat::send_frame(not_joined.sock, "TEXT|1|hi") &&
        expect_prefix(not_joined, "ERR|NOT_JOINED|"), "T06 chat before login rejected");
    close_peer(not_joined);
    const std::string mixed = "你好 Bob! Hello|世界";
    check(chat::send_frame(a.sock, "TEXT|1|" + mixed) &&
          get_msg(a, "Alice", "1", mixed) && get_msg(b, "Alice", "1", mixed) &&
          get_msg(c, "Alice", "1", mixed), "T07 UTF-8, timestamp, | in body, three-way broadcast");
    const std::string half = "TEXT|2|中文拆包测试\n";
    bool frag = true;
    for (char ch : half) frag = frag && chat::send_all(a.sock, std::string(1, ch));
    check(frag && get_msg(a, "Alice", "2", "中文拆包测试") &&
          get_msg(b, "Alice", "2", "中文拆包测试") &&
          get_msg(c, "Alice", "2", "中文拆包测试"), "T08 fragmented TCP sends / UTF-8 recombination");
    check(chat::send_all(b.sock, "TEXT|1|first line\nTEXT|2|second line\n") &&
          get_msg(a, "Bob", "1", "first line") && get_msg(a, "Bob", "2", "second line") &&
          get_msg(b, "Bob", "1", "first line") && get_msg(b, "Bob", "2", "second line") &&
          get_msg(c, "Bob", "1", "first line") && get_msg(c, "Bob", "2", "second line"),
          "T09 coalesced TCP sends correctly separated");
    std::string online;
    check(chat::send_frame(a.sock, "WHO") && expect_prefix(a, "USERS|", &online) &&
          online.find("Alice") != std::string::npos && online.find("Bob") != std::string::npos &&
          online.find("Charlie") != std::string::npos, "T10 WHO online user list");
    check(chat::send_frame(a.sock, "TEXT|999|bad sequence") && expect_prefix(a, "ERR|BAD_SEQ|"),
          "T11 erroneous sequence rejected");
    check(chat::send_frame(a.sock, "TEXT|3|valid after rejected seq") &&
          get_msg(a, "Alice", "3", "valid after rejected seq") &&
          get_msg(b, "Alice", "3", "valid after rejected seq") &&
          get_msg(c, "Alice", "3", "valid after rejected seq"), "T12 recovery after bad sequence");
    const std::string maxbody(4096, 'A');
    check(chat::send_frame(c.sock, "TEXT|1|" + maxbody) &&
          get_msg(a, "Charlie", "1", maxbody) && get_msg(b, "Charlie", "1", maxbody) &&
          get_msg(c, "Charlie", "1", maxbody), "T13 legal 4096-byte message fully delivered");
    check(chat::send_frame(c.sock, "TEXT|2|" + std::string(4097, 'B')) &&
          expect_prefix(c, "ERR|TOO_LONG|"), "T14 illegal 4097-byte message rejected");
    check(chat::send_frame(c.sock, "QUIT") && expect_prefix(c, "BYE|") &&
          expect_prefix(a, "NOTICE|", nullptr), "T15 QUIT and response");
    close_peer(c); close_peer(a); close_peer(b);

    // 并发会话各发送 100 条消息。
    Peer x, y, z;
    const bool load_login = login(x, host, port, "LoadA") && login(y, host, port, "LoadB") &&
                            login(z, host, port, "LoadC");
    check(load_login, "T16 fresh sessions for concurrent load");
    if (load_login) {
        std::atomic<bool> sent_ok{true};
        auto send_100 = [&sent_ok](socket_handle sock, const char* name) {
            for (int i = 1; i <= 100; ++i) {
                if (!chat::send_frame(sock, "TEXT|" + std::to_string(i) + "|load_" + name + "_" + std::to_string(i))) {
                    sent_ok.store(false); return;
                }
            }
        };
        std::thread tx(send_100, x.sock, "LoadA");
        std::thread ty(send_100, y.sock, "LoadB");
        std::thread tz(send_100, z.sock, "LoadC");
        tx.join(); ty.join(); tz.join();
        check(sent_ok.load(), "T17 three senders each submitted 100 messages");
        auto receive_300 = [](Peer& p) {
            std::set<std::string> unique;
            std::size_t duplicates = 0;
            std::size_t invalid = 0;
            for (int attempts = 0; attempts < 450; ++attempts) {
                if (unique.size() == 300) break;
                std::string line;
                if (!next_frame(p, line)) break;
                if (line.rfind("MSG|", 0) != 0) continue;
                auto f = chat::split_fields(line, 4);
                if (f.size() != 5) {
                    continue;
                }
                if (f[2] != "LoadA" && f[2] != "LoadB" && f[2] != "LoadC") continue;
                std::uint64_t seq = 0;
                if (!chat::parse_sequence(f[3], seq) || seq > 100 ||
                    f[4] != "load_" + f[2] + "_" + f[3] || f[1].size() != 19) {
                    ++invalid;
                    continue;
                }
                if (!unique.insert(f[2] + "|" + f[3]).second) ++duplicates;
            }
            std::cout << "[DELIVERY] expected=300 received=" << unique.size()
                      << " missing=" << 300 - unique.size() << " duplicate=" << duplicates
                      << " invalid=" << invalid << '\n';
            if (duplicates || invalid) return std::size_t{0};
            return unique.size();
        };
        const auto cx = receive_300(x), cy = receive_300(y), cz = receive_300(z);
        std::cout << "[COUNT] expected=300 each, LoadA=" << cx << " LoadB=" << cy << " LoadC=" << cz << '\n';
        check(cx == 300 && cy == 300 && cz == 300, "T18 900 total deliveries checked without missing sequences");
    }
    close_peer(x); close_peer(y); close_peer(z);

    // 私聊和群聊功能测试。
    Peer dawn, evan, faye;
    const bool feature_login = login(dawn, host, port, "Dawn") &&
                               login(evan, host, port, "Evan") &&
                               login(faye, host, port, "Faye");
    check(feature_login, "T19 three users ready for private and group chat");
    if (feature_login) {
        const std::string private_text = "private 中文 English|message";
        check(chat::send_frame(dawn.sock, "PM|1|Evan|" + private_text) &&
              get_private_message(dawn, "Dawn", "Evan", "1", private_text) &&
              get_private_message(evan, "Dawn", "Evan", "1", private_text),
              "T20 private message delivered to sender and target");

        const bool created = chat::send_frame(dawn.sock, "GROUP|CREATE|Study") &&
                             expect_prefix(dawn, "GROUP_OK|CREATE|Study");
        const bool joined = chat::send_frame(evan.sock, "GROUP|JOIN|Study") &&
                            expect_prefix(evan, "GROUP_OK|JOIN|Study");
        const bool added = chat::send_frame(dawn.sock, "GROUP|ADD|Study|Faye") &&
                           expect_prefix(dawn, "GROUP_OK|ADD|Study|Faye");
        std::string groups;
        const bool listed = chat::send_frame(dawn.sock, "GROUP|LIST") &&
                            expect_prefix(dawn, "GROUPS|", &groups) &&
                            groups.find("Study") != std::string::npos;
        check(created && joined && added && listed,
              "T21 group create, member selection and list");

        const std::string group_text = "Study group message 中文";
        check(chat::send_frame(dawn.sock, "GTEXT|2|Study|" + group_text) &&
              get_group_message(dawn, "Study", "Dawn", "2", group_text) &&
              get_group_message(evan, "Study", "Dawn", "2", group_text),
              "T22 group message delivered to selected members");

        check(get_group_message(faye, "Study", "Dawn", "2", group_text),
              "T23 selected member received group message");

        const bool left = chat::send_frame(evan.sock, "GROUP|LEAVE|Study") &&
                          expect_prefix(evan, "GROUP_OK|LEAVE|Study");
        const bool missing_user = chat::send_frame(faye.sock, "PM|1|Ghost|offline target") &&
                                  expect_prefix(faye, "ERR|NO_USER|");
        const bool rejected = chat::send_frame(evan.sock, "GTEXT|1|Study|not a member") &&
                              expect_prefix(evan, "ERR|NOT_MEMBER|");
        check(left && missing_user && rejected, "T24 group leave and message sequence check");
    }
    close_peer(dawn); close_peer(evan); close_peer(faye);
    std::cout << "TOTAL PASS=" << passes << " FAIL=" << fails << '\n';
    cleanup_sockets();
    return fails ? 1 : 0;
}
