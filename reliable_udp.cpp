#define NOMINMAX
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")
using Socket = SOCKET;
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
using Socket = int;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#endif
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
constexpr int HEADER = 32;
constexpr int BLOCK = 1200;
constexpr uint32_t MAGIC = 0x52545031;
enum Type : uint32_t { SYN = 1, SYN_ACK, DATA, ACK, FIN, FIN_ACK, BAD };

void put32(char* p, uint32_t v) {
    v = htonl(v);
    std::memcpy(p, &v, 4);
}
uint32_t get32(const char* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return ntohl(v);
}
void put64(char* p, uint64_t v) {
    put32(p, uint32_t(v >> 32));
    put32(p + 4, uint32_t(v));
}
uint64_t get64(const char* p) {
    return (uint64_t(get32(p)) << 32) | get32(p + 4);
}
uint32_t hashBytes(const char* p, size_t n, uint32_t hash = 2166136261u) {
    for (size_t i = 0; i < n; ++i) hash = (hash ^ uint8_t(p[i])) * 16777619u;
    return hash;
}

struct Packet {
    uint32_t session = 0, type = 0, seq = 0;
    uint64_t info = 0;
    std::string data;
};
std::string encode(const Packet& p) {
    std::string s(HEADER + p.data.size(), '\0');
    put32(s.data(), MAGIC);
    put32(s.data() + 4, p.session);
    put32(s.data() + 8, p.type);
    put32(s.data() + 12, p.seq);
    put32(s.data() + 16, uint32_t(p.data.size()));
    put64(s.data() + 24, p.info);
    std::copy(p.data.begin(), p.data.end(), s.begin() + HEADER);
    put32(s.data() + 20, hashBytes(s.data(), s.size()));
    return s;
}
bool decode(const char* buf, int n, Packet& p) {
    if (n < HEADER || get32(buf) != MAGIC || get32(buf + 16) > BLOCK ||
        n != HEADER + int(get32(buf + 16))) return false;
    std::string s(buf, n);
    uint32_t expected = get32(s.data() + 20);
    put32(s.data() + 20, 0);
    if (hashBytes(s.data(), s.size()) != expected) return false;
    p.session = get32(buf + 4);
    p.type = get32(buf + 8);
    p.seq = get32(buf + 12);
    p.info = get64(buf + 24);
    p.data.assign(buf + HEADER, buf + n);
    return true;
}

void closeSock(Socket s) {
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
}
struct Network {
    Network() {
#ifdef _WIN32
        WSADATA w{};
        if (WSAStartup(MAKEWORD(2, 2), &w)) throw std::runtime_error("WSAStartup failed");
#endif
    }
    ~Network() {
#ifdef _WIN32
        WSACleanup();
#endif
    }
};
struct Udp {
    Socket fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    Udp(int port = 0) {
        if (fd == INVALID_SOCKET) throw std::runtime_error("socket failed");
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        a.sin_port = htons(uint16_t(port));
        if (bind(fd, (sockaddr*)&a, sizeof(a)) == SOCKET_ERROR) throw std::runtime_error("bind failed");
    }
    ~Udp() { if (fd != INVALID_SOCKET) closeSock(fd); }
    Udp(const Udp&) = delete;
    Udp& operator=(const Udp&) = delete;
    void send(const Packet& p, const sockaddr_in& to) {
        std::string s = encode(p);
        if (sendto(fd, s.data(), int(s.size()), 0, (const sockaddr*)&to, sizeof(to)) == SOCKET_ERROR)
            throw std::runtime_error("sendto failed");
    }
    //等待一个有效报文，超时返回false
    bool receive(Packet& p, sockaddr_in& from, int waitMs) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        timeval tv{waitMs / 1000, (waitMs % 1000) * 1000};
        int ready = select(int(fd) + 1, &rfds, nullptr, nullptr, &tv);
        if (ready < 0) throw std::runtime_error("select failed");
        if (!ready) return false;
        std::array<char, HEADER + BLOCK> buf{};
#ifdef _WIN32
        int len = sizeof(from);
#else
        socklen_t len = sizeof(from);
#endif
        int n = recvfrom(fd, buf.data(), int(buf.size()), 0, (sockaddr*)&from, &len);
        return n > 0 && decode(buf.data(), n, p);
    }
};
sockaddr_in address(const std::string& host, int port) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(uint16_t(port));
    if (inet_pton(AF_INET, host.c_str(), &a.sin_addr) != 1) throw std::runtime_error("invalid IPv4 address");
    return a;
}
bool samePeer(const sockaddr_in& a, const sockaddr_in& b) {
    return a.sin_port == b.sin_port && a.sin_addr.s_addr == b.sin_addr.s_addr;
}
double seconds(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}
void result(uint64_t bytes, Clock::time_point start) {
    double duration = std::max(0.000001, seconds(start));
    std::cout << std::fixed << std::setprecision(3)
              << "[RESULT] bytes=" << bytes << " time=" << duration << "s"
              << " throughput=" << bytes * 8.0 / duration / 1e6 << "Mbps\n";
}

void sendFile(const std::string& path, const sockaddr_in& peer, int window, int timeout) {
    std::ifstream in(std::filesystem::u8path(path), std::ios::binary | std::ios::ate);
    if (!in) throw std::runtime_error("cannot open source file");
    uint64_t size = uint64_t(in.tellg());
    uint64_t count64 = (size + BLOCK - 1) / BLOCK;
    if (count64 > UINT32_MAX) throw std::runtime_error("file too large");
    uint32_t total = uint32_t(count64);
    in.seekg(0);
    uint32_t fileHash = 2166136261u;
    std::array<char, 65536> buf{};
    while (in) {
        in.read(buf.data(), buf.size());
        fileHash = hashBytes(buf.data(), size_t(in.gcount()), fileHash);
    }
    in.clear();
    in.seekg(0);
    Udp udp;
    std::mt19937 rng(std::random_device{}());
    uint32_t session = rng();
    auto start = Clock::now();
    Packet hello{session, SYN, fileHash, size, {}};
    auto lastHello = start - std::chrono::milliseconds(timeout);
    sockaddr_in from{};
    Packet p;
    std::cout << "[START] size=" << size << " packets=" << total << " window=" << window << '\n';
    while (true) {
        if (Clock::now() - lastHello >= std::chrono::milliseconds(timeout)) {
            udp.send(hello, peer);
            lastHello = Clock::now();
            std::cout << "[SYN] session=" << session << '\n';
        }
        if (udp.receive(p, from, 30) && samePeer(from, peer) && p.session == session && p.type == SYN_ACK) break;
    }
    std::cout << "[CONNECTED]\n";
    std::vector<uint8_t> acked(total, 0);
    struct Pending { std::string bytes; Clock::time_point sent; int tries = 0; };
    std::map<uint32_t, Pending> pending;
    uint32_t base = 0, next = 0;
    uint64_t retries = 0;
    double rttSum = 0;
    uint64_t rttCount = 0;
    while (base < total) {
        //只允许窗口内的报文进入网络
        while (next < total && uint64_t(next) < uint64_t(base) + window) {
            size_t n = size_t(std::min<uint64_t>(BLOCK, size - uint64_t(next) * BLOCK));
            std::string data(n, '\0');
            in.read(data.data(), std::streamsize(n));
            if (size_t(in.gcount()) != n) throw std::runtime_error("source read failed");
            udp.send({session, DATA, next, 0, data}, peer);
            pending[next] = {std::move(data), Clock::now(), 1};
            std::cout << "[SEND] seq=" << next << " window=[" << base << ','
                      << std::min<uint64_t>(total, uint64_t(base) + window) << ")\n";
            ++next;
        }
        if (udp.receive(p, from, 15) && samePeer(from, peer) && p.session == session && p.type == ACK && p.seq < total) {
            if (!acked[p.seq] && pending.count(p.seq)) {
                auto& item = pending.at(p.seq);
                if (item.tries == 1) {
                    double rtt = std::chrono::duration<double, std::milli>(Clock::now() - item.sent).count();
                    rttSum += rtt;
                    ++rttCount;
                    std::cout << "[ACK] seq=" << p.seq << " RTT=" << std::fixed
                              << std::setprecision(1) << rtt << "ms\n";
                } else std::cout << "[ACK] seq=" << p.seq << " (retransmitted)\n";
                pending.erase(p.seq);
                acked[p.seq] = 1;
                uint32_t old = base;
                while (base < total && acked[base]) ++base;
                if (base != old)
                    std::cout << "[WINDOW] base " << old << " -> " << base << '\n';
            }
        }
        auto now = Clock::now();
        for (auto& item : pending) {
            if (now - item.second.sent >= std::chrono::milliseconds(timeout)) {
                udp.send({session, DATA, item.first, 0, item.second.bytes}, peer);
                item.second.sent = Clock::now();
                ++item.second.tries;
                ++retries;
                std::cout << "[RETX] seq=" << item.first << " attempt=" << item.second.tries << '\n';
            }
        }
    }
    Packet fin{session, FIN, fileHash, size, {}};
    auto lastFin = Clock::now() - std::chrono::milliseconds(timeout);
    while (true) {
        if (Clock::now() - lastFin >= std::chrono::milliseconds(timeout)) {
            udp.send(fin, peer);
            lastFin = Clock::now();
            std::cout << "[FIN] waiting for verification\n";
        }
        if (udp.receive(p, from, 30) && samePeer(from, peer) && p.session == session) {
            if (p.type == FIN_ACK) break;
            if (p.type == BAD) throw std::runtime_error("receiver checksum mismatch");
        }
    }
    std::cout << "[SUCCESS] retransmissions=" << retries << " avg_RTT="
              << std::fixed << std::setprecision(1) << (rttCount ? rttSum / rttCount : 0) << "ms\n";
    result(size, start);
}

void receiveFile(const std::string& path, int port, int window, int timeout) {
    Udp udp(port);
    std::cout << "[LISTEN] UDP port=" << port << " window=" << window << '\n';
    Packet p;
    sockaddr_in from{}, peer{};
    uint32_t session = 0, expectedHash = 0;
    uint64_t size = 0, written = 0;
    uint32_t base = 0, runningHash = 2166136261u;
    std::map<uint32_t, std::string> buffered;
    std::ofstream out;
    Clock::time_point start{};
    bool connected = false, complete = false;
    Clock::time_point completedAt{};
    while (true) {
        if (complete && Clock::now() - completedAt > std::chrono::milliseconds(std::max(3000, 4 * timeout))) break;
        if (!udp.receive(p, from, 50)) continue;
        if (!connected) {
            if (p.type != SYN || (p.info + BLOCK - 1) / BLOCK > UINT32_MAX) continue;
            peer = from;
            session = p.session;
            size = p.info;
            expectedHash = p.seq;
            out.open(std::filesystem::u8path(path), std::ios::binary | std::ios::trunc);
            if (!out) throw std::runtime_error("cannot open output file");
            start = Clock::now();
            connected = true;
            std::cout << "[SYN] size=" << size << " session=" << session << '\n';
        }
        if (!samePeer(peer, from) || session != p.session) continue;
        if (p.type == SYN) {
            udp.send({session, SYN_ACK, 0, 0, {}}, peer);
            continue;
        }
        if (p.type == DATA && !complete) {
            uint64_t total = (size + BLOCK - 1) / BLOCK;
            uint64_t offset = uint64_t(p.seq) * BLOCK;
            if (p.seq >= total || p.data.size() != std::min<uint64_t>(BLOCK, size - offset)) continue;
            if (p.seq < base) {
                udp.send({session, ACK, p.seq, 0, {}}, peer);
                continue;
            }
            if (uint64_t(p.seq) >= uint64_t(base) + window) continue;
            bool fresh = buffered.emplace(p.seq, p.data).second;
            udp.send({session, ACK, p.seq, 0, {}}, peer);
            if (fresh) std::cout << "[RECV] seq=" << p.seq << " bytes=" << p.data.size() << '\n';
            uint32_t old = base;
            while (true) {
                auto it = buffered.find(base);
                if (it == buffered.end()) break;
                out.write(it->second.data(), std::streamsize(it->second.size()));
                if (!out) throw std::runtime_error("output write failed");
                runningHash = hashBytes(it->second.data(), it->second.size(), runningHash);
                written += it->second.size();
                buffered.erase(it);
                ++base;
            }
            if (base != old) std::cout << "[WINDOW] base " << old << " -> " << base << '\n';
        }
        if (p.type == FIN) {
            if (written == size && runningHash == expectedHash && p.info == size && p.seq == expectedHash) {
                if (!complete) {
                    out.close();
                    complete = true;
                    completedAt = Clock::now();
                    std::cout << "[SUCCESS] checksum verified\n";
                    result(written, start);
                } else completedAt = Clock::now();
                udp.send({session, FIN_ACK, 0, 0, {}}, peer);
            } else if (written == size) {
                udp.send({session, BAD, 0, 0, {}}, peer);
                throw std::runtime_error("file checksum mismatch");
            }
        }
    }
}

struct Delayed {
    Clock::time_point due;
    sockaddr_in to;
    std::string bytes;
};
void route(int listenPort, const sockaddr_in& receiver, double loss, int delay) {
    Udp udp(listenPort);
    sockaddr_in sender{};
    bool known = false;
    std::mt19937 rng(std::random_device{}());
    std::uniform_real_distribution<double> chance(0, 100);
    std::vector<Delayed> queue;
    uint64_t dropped = 0, forwarded = 0;
    std::cout << "[ROUTER] port=" << listenPort << " loss=" << loss << "% delay=" << delay << "ms\n";
    while (true) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(udp.fd, &rfds);
        timeval tv{0, 10000};
        int ready = select(int(udp.fd) + 1, &rfds, nullptr, nullptr, &tv);
        if (ready < 0) throw std::runtime_error("select failed");
        if (ready) {
            char buf[HEADER + BLOCK];
            sockaddr_in from{};
#ifdef _WIN32
            int len = sizeof(from);
#else
            socklen_t len = sizeof(from);
#endif
            int n = recvfrom(udp.fd, buf, sizeof(buf), 0, (sockaddr*)&from, &len);
            if (n > 0) {
                Packet p;
                if (!decode(buf, n, p)) continue;
                bool fromReceiver = samePeer(from, receiver);
                if (!fromReceiver && (!known || samePeer(from, sender))) {
                    sender = from;
                    known = true;
                } else if (!fromReceiver) continue;
                if (!known) continue;
                if (chance(rng) < loss) {
                    ++dropped;
                    std::cout << "[DROP] type=" << p.type << " seq=" << p.seq << " total=" << dropped << '\n';
                } else {
                    queue.push_back({Clock::now() + std::chrono::milliseconds(delay),
                                     fromReceiver ? sender : receiver, std::string(buf, n)});
                }
            }
        }
        auto now = Clock::now();
        for (size_t i = 0; i < queue.size();) {
            if (queue[i].due <= now) {
                const auto& d = queue[i];
                sendto(udp.fd, d.bytes.data(), int(d.bytes.size()), 0, (sockaddr*)&d.to, sizeof(d.to));
                ++forwarded;
                queue[i] = std::move(queue.back());
                queue.pop_back();
            } else ++i;
        }
    }
}

int number(const char* s, int lo, int hi) {
    std::string text(s);
    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos) throw std::runtime_error("invalid integer");
    long long n = std::stoll(text);
    if (n < lo || n > hi) throw std::runtime_error("parameter out of range");
    return int(n);
}
void usage() {
    std::cout << "Usage:\n"
              << "  reliable_udp recv OUTPUT PORT WINDOW [TIMEOUT_MS]\n"
              << "  reliable_udp send INPUT IP PORT WINDOW [TIMEOUT_MS]\n"
              << "  reliable_udp route LISTEN_PORT DEST_IP DEST_PORT LOSS_PERCENT DELAY_MS\n"
              << "Example: recv received.bin 9001 16 800\n";
}
int run(int argc, char** argv) {
    std::cout << std::unitbuf;
    try {
        Network net;
        if (argc < 2) { usage(); return 1; }
        std::string mode = argv[1];
        if (mode == "recv" && (argc == 5 || argc == 6))
            receiveFile(argv[2], number(argv[3], 1, 65535), number(argv[4], 1, 4096),
                        argc == 6 ? number(argv[5], 100, 60000) : 800);
        else if (mode == "send" && (argc == 6 || argc == 7))
            sendFile(argv[2], address(argv[3], number(argv[4], 1, 65535)),
                     number(argv[5], 1, 4096), argc == 7 ? number(argv[6], 100, 60000) : 800);
        else if (mode == "route" && argc == 7)
            route(number(argv[2], 1, 65535), address(argv[3], number(argv[4], 1, 65535)),
                  number(argv[5], 0, 99), number(argv[6], 0, 10000));
        else { usage(); return 1; }
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] " << e.what() << '\n';
        return 1;
    }
    return 0;
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i) {
        int n = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, nullptr, 0, nullptr, nullptr);
        if (!n) return 1;
        std::string s(n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, s.data(), n, nullptr, nullptr);
        s.pop_back();
        args.push_back(std::move(s));
    }
    std::vector<char*> ptrs;
    for (auto& s : args) ptrs.push_back(s.data());
    return run(argc, ptrs.data());
}
#else
int main(int argc, char** argv) { return run(argc, argv); }
#endif
