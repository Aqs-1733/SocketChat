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
#include <vector>

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
constexpr int HEADER = 32;
constexpr int BLOCK = 1200;
constexpr int MAX_TRIES = 30;
constexpr uint32_t MAGIC = 0x52545032;
enum Type : uint32_t { SYN = 1, SYN_ACK, DATA, ACK, FIN, FIN_ACK, BAD, OPEN_ACK, CLOSE_ACK, RESET };

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

//16位反码求和，奇数字节补零
uint16_t checksum(const char* p, size_t n) {
    uint32_t sum = 0;
    for (size_t i = 0; i < n; i += 2) {
        sum += uint32_t(uint8_t(p[i])) << 8;
        if (i + 1 < n) sum += uint8_t(p[i + 1]);
        sum = (sum & 0xffff) + (sum >> 16);
    }
    return uint16_t(~sum);
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
    put32(s.data() + 20, checksum(s.data(), s.size()));
    return s;
}
bool decode(const char* buf, int n, Packet& p) {
    if (n < HEADER || get32(buf) != MAGIC || get32(buf + 16) > BLOCK ||
        n != HEADER + int(get32(buf + 16))) return false;
    if (get32(buf + 20) > 0xffff || checksum(buf, n) != 0) return false;
    uint32_t type = get32(buf + 8), length = get32(buf + 16);
    if (type < SYN || type > RESET) return false;
    if (type == SYN ? length != 8 : type == DATA ? length == 0 : length != 0) return false;
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
//直接使用数据报套接字
Socket createSocket(int port = 0) {
    Socket fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd == INVALID_SOCKET) throw std::runtime_error("socket failed");
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(uint16_t(port));
#ifdef _WIN32
    BOOL exclusive = TRUE;
    setsockopt(fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (char*)&exclusive, sizeof(exclusive));
#endif
    if (bind(fd, (sockaddr*)&a, sizeof(a)) == SOCKET_ERROR) {
        closeSock(fd);
        throw std::runtime_error("bind failed");
    }
    int buffer = 4 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, (char*)&buffer, sizeof(buffer));
    return fd;
}
void sendPacket(Socket fd, const Packet& p, const sockaddr_in& to) {
    auto s = encode(p);
    if (sendto(fd, s.data(), int(s.size()), 0, (const sockaddr*)&to, sizeof(to)) == SOCKET_ERROR)
        throw std::runtime_error("sendto failed");
}
bool receivePacket(Socket fd, Packet& p, sockaddr_in& from, int waitMs) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    timeval tv{waitMs / 1000, (waitMs % 1000) * 1000};
#ifdef _WIN32
    int ready = select(0, &rfds, nullptr, nullptr, &tv);
    int len = sizeof(from);
#else
    int ready = select(fd + 1, &rfds, nullptr, nullptr, &tv);
    socklen_t len = sizeof(from);
#endif
    if (ready < 0) throw std::runtime_error("select failed");
    if (!ready) return false;
    char buf[65536];
    int n = recvfrom(fd, buf, int(sizeof(buf)), 0, (sockaddr*)&from, &len);
    if (n < 0) {
#ifdef _WIN32
        if (WSAGetLastError() == WSAECONNRESET) return false;
#endif
        throw std::runtime_error("recvfrom failed");
    }
    if (!decode(buf, n, p)) {
        std::cout << "[CHECKSUM_ERROR] invalid packet discarded\n";
        return false;
    }
    return true;
}
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
              << "[RESULT] bytes=" << bytes << " time=" << std::setprecision(6) << duration << "s"
              << " throughput=" << std::setprecision(3) << bytes * 8.0 / duration / 1e6 << "Mbps\n";
}

void sendFile(Socket fd, const std::string& path, const sockaddr_in& peer, int window, int timeout) {
    std::ifstream in(std::filesystem::u8path(path), std::ios::binary | std::ios::ate);
    if (!in) throw std::runtime_error("cannot open source file");
    if (in.tellg() < 0) throw std::runtime_error("source size failed");
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
    if (in.bad()) throw std::runtime_error("source read failed");
    in.clear();
    in.seekg(0);
    std::mt19937 rng(std::random_device{}());
    uint32_t session = rng();
    try {
        auto start = Clock::now();
        std::string options(8, '\0');
        put32(options.data(), window);
        put32(options.data() + 4, timeout);
        Packet hello{session, SYN, fileHash, size, options};
        int attempts = 0;
        auto lastHello = start - std::chrono::milliseconds(timeout);
        sockaddr_in from{};
        Packet p;
        std::cout << "[START] size=" << size << " packets=" << total << " window=" << window << '\n';
        while (true) {
            if (Clock::now() - lastHello >= std::chrono::milliseconds(timeout)) {
                if (++attempts > MAX_TRIES) throw std::runtime_error("connection timeout");
                sendPacket(fd, hello, peer);
                lastHello = Clock::now();
                std::cout << "[SYN] session=" << session << '\n';
            }
            if (receivePacket(fd, p, from, 30) && samePeer(from, peer) && p.session == session) {
                if (p.type == RESET) throw std::runtime_error("connection parameters rejected");
                if (p.type == SYN_ACK && p.info == uint64_t(window)) {
                    sendPacket(fd, {session, OPEN_ACK, 0, 0, {}}, peer);
                    std::cout << "[OPEN_ACK] window=" << window << '\n';
                    break;
                }
            }
        }
        std::cout << "[CONNECTED]\n";

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
                sendPacket(fd, {session, DATA, next, 0, data}, peer);
                pending[next] = {std::move(data), Clock::now(), 1};
                std::cout << "[SEND] seq=" << next << " window=[" << base << ','
                          << std::min<uint64_t>(total, uint64_t(base) + window) << ")\n";
                ++next;
            }
            if (receivePacket(fd, p, from, 15) && samePeer(from, peer) && p.session == session) {
                if (p.type == RESET) throw std::runtime_error("peer aborted");
                if (p.type == ACK && p.data.empty() && pending.count(p.seq)) {
                    auto& item = pending.at(p.seq);
                    if (item.tries == 1) {
                        double rtt = std::chrono::duration<double, std::milli>(Clock::now() - item.sent).count();
                        rttSum += rtt;
                        ++rttCount;
                        std::cout << "[ACK] seq=" << p.seq << " RTT=" << std::fixed
                                  << std::setprecision(1) << rtt << "ms\n";
                    } else std::cout << "[ACK] seq=" << p.seq << " (retransmitted)\n";
                    pending.erase(p.seq);
                    uint32_t old = base;
                    while (base < next && !pending.count(base)) ++base;
                    if (base != old)
                        std::cout << "[WINDOW] base " << old << " -> " << base << '\n';
                }
            }
            auto now = Clock::now();
            for (auto& item : pending) {
                if (now - item.second.sent >= std::chrono::milliseconds(timeout)) {
                    if (item.second.tries >= MAX_TRIES) {
                        throw std::runtime_error("data retransmission limit");
                    }
                    sendPacket(fd, {session, DATA, item.first, 0, item.second.bytes}, peer);
                    item.second.sent = Clock::now();
                    ++item.second.tries;
                    ++retries;
                    std::cout << "[RETX] seq=" << item.first << " attempt=" << item.second.tries << '\n';
                }
            }
        }
        Packet fin{session, FIN, fileHash, size, {}};
        auto lastFin = Clock::now() - std::chrono::milliseconds(timeout);
        attempts = 0;
        while (true) {
            if (Clock::now() - lastFin >= std::chrono::milliseconds(timeout)) {
                if (++attempts > MAX_TRIES) throw std::runtime_error("close timeout");
                sendPacket(fd, fin, peer);
                lastFin = Clock::now();
                std::cout << "[FIN] waiting for verification\n";
            }
            if (receivePacket(fd, p, from, 30) && samePeer(from, peer) && p.session == session) {
                if (p.type == FIN_ACK) {
                    //最终确认发送三份，降低单个确认丢失的影响
                    for (int i = 0; i < 3; ++i) sendPacket(fd, {session, CLOSE_ACK, 0, 0, {}}, peer);
                    std::cout << "[CLOSE_ACK] connection closed\n";
                    break;
                }
                if (p.type == BAD || p.type == RESET) throw std::runtime_error("receiver checksum mismatch");
            }
        }
        std::cout << "[SUCCESS] retransmissions=" << retries << " avg_RTT=";
        if (rttCount) std::cout << std::fixed << std::setprecision(1) << rttSum / rttCount << "ms";
        else std::cout << "N/A";
        std::cout << '\n';
        result(size, start);
    } catch (...) {
        //通知对端异常结束
        try { sendPacket(fd, {session, RESET, 0, 0, {}}, peer); } catch (...) {}
        throw;
    }
}

void receiveFile(Socket fd, const std::string& path, int port, int window, int timeout) {
    std::cout << "[LISTEN] UDP port=" << port << " window=" << window << '\n';
    Packet p;
    sockaddr_in from{}, peer{};
    uint32_t session = 0, expectedHash = 0;
    uint64_t size = 0, written = 0;
    uint32_t base = 0, runningHash = 2166136261u;
    std::map<uint32_t, std::string> buffered;
    auto target = std::filesystem::u8path(path);
    auto temp = target;
#ifdef _WIN32
    temp += ".part." + std::to_string(GetCurrentProcessId());
#else
    temp += ".part." + std::to_string(getpid());
#endif
    //异常退出清理本次临时文件
    struct TempGuard {
        std::filesystem::path path;
        bool active = false;
        ~TempGuard() { if (active) { std::error_code ec; std::filesystem::remove(path, ec); } }
    } cleanup{temp};
    std::ofstream out;
    Clock::time_point start{};
    bool connected = false, complete = false, established = false;
    auto lastSeen = Clock::now();
    auto listenLimit = std::chrono::milliseconds(std::max(60000, (MAX_TRIES + 2) * timeout));
    auto idle = std::chrono::milliseconds(std::max(10000, (MAX_TRIES + 2) * timeout));
    Clock::time_point completedAt{};
    try {
        while (true) {
            if (complete && Clock::now() - completedAt > idle) {
                std::cout << "[CLOSED] final ACK wait expired, file verified\n";
                break;
            }
            if (!complete && Clock::now() - lastSeen > (connected ? idle : listenLimit)) throw std::runtime_error("receiver idle timeout");
            if (!receivePacket(fd, p, from, 50)) continue;
            if (!connected) {
                if (p.type != SYN || p.info > uint64_t(UINT32_MAX) * BLOCK) continue;
                if (p.data.size() != 8 || get32(p.data.data()) != uint32_t(window) ||
                    get32(p.data.data() + 4) < 100 || get32(p.data.data() + 4) > 60000) {
                    sendPacket(fd, {p.session, RESET, 0, 0, {}}, from);
                    std::cout << "[REJECT] window or timeout mismatch\n";
                    continue;
                }
                idle = std::chrono::milliseconds(std::max(10000u, (MAX_TRIES + 2) * get32(p.data.data() + 4)));
                peer = from;
                session = p.session;
                size = p.info;
                expectedHash = p.seq;
                if (std::filesystem::exists(temp)) throw std::runtime_error("temporary file already exists");
                out.open(temp, std::ios::binary | std::ios::trunc);
                if (!out) throw std::runtime_error("cannot open output file");
                cleanup.active = true;
                std::cout << "[TEMP] created\n";
                start = Clock::now();
                connected = true;
                std::cout << "[SYN] size=" << size << " session=" << session << '\n';
            }
            if (!samePeer(peer, from) || session != p.session) continue;
            lastSeen = Clock::now();
            if (p.type == RESET) throw std::runtime_error("peer aborted");
            if (p.type == CLOSE_ACK && complete) {
                std::cout << "[CLOSED] final confirmation received\n";
                break;
            }
            if (p.type == SYN) {
                sendPacket(fd, {session, SYN_ACK, 0, uint64_t(window), {}}, peer);
                std::cout << "[SYN_ACK] window=" << window << '\n';
                continue;
            }
            //首个DATA或FIN也可确认连接，容忍第三次握手丢失
            if (!established && (p.type == OPEN_ACK || p.type == DATA || p.type == FIN)) {
                established = true;
                std::cout << "[CONNECTED]\n";
            }
            if (p.type == DATA && !complete) {
                uint64_t total = (size + BLOCK - 1) / BLOCK;
                uint64_t offset = uint64_t(p.seq) * BLOCK;
                if (p.seq >= total || p.data.size() != std::min<uint64_t>(BLOCK, size - offset)) continue;
                if (p.seq < base) {
                    std::cout << "[DUPLICATE] seq=" << p.seq << '\n';
                    sendPacket(fd, {session, ACK, p.seq, 0, {}}, peer);
                    continue;
                }
                if (uint64_t(p.seq) >= uint64_t(base) + window) continue;
                bool fresh = buffered.emplace(p.seq, p.data).second;
                sendPacket(fd, {session, ACK, p.seq, 0, {}}, peer);
                if (fresh) std::cout << "[RECV] seq=" << p.seq << " bytes=" << p.data.size() << '\n';
                else std::cout << "[DUPLICATE] seq=" << p.seq << '\n';
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
                std::cout << "[RECV_WINDOW] base=" << base << " end=" << std::min<uint64_t>(total, uint64_t(base) + window) << " buffered=" << buffered.size() << '\n';
            }
            if (p.type == FIN) {
                if (written == size && runningHash == expectedHash && p.info == size && p.seq == expectedHash) {
                    if (!complete) {
                        out.close();
                        if (!out) throw std::runtime_error("output close failed");
    #ifdef _WIN32
                        if (!MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING))
                            throw std::runtime_error("output rename failed");
    #else
                        std::filesystem::rename(temp, target);
    #endif
                        cleanup.active = false;
                        complete = true;
                        completedAt = Clock::now();
                        std::cout << "[SUCCESS] checksum verified\n";
                        result(written, start);
                    } else completedAt = Clock::now();
                    sendPacket(fd, {session, FIN_ACK, 0, 0, {}}, peer);
                    std::cout << "[FIN_ACK] file verified\n";
                } else if (written == size) {
                    sendPacket(fd, {session, BAD, 0, 0, {}}, peer);
                    throw std::runtime_error("file checksum mismatch");
                }
            }
        }
    } catch (...) {
        if (connected && !complete) {
            try { sendPacket(fd, {session, RESET, 0, 0, {}}, peer); } catch (...) {}
        }
        throw;
    }
}

struct Delayed {
    Clock::time_point due;
    sockaddr_in to;
    std::string bytes;
};
void route(Socket fd, int listenPort, const sockaddr_in& receiver, double loss, int delay, int corrupt, const std::string& fault) {
    sockaddr_in sender{};
    bool known = false;
    std::mt19937 rng(std::random_device{}());
    std::uniform_real_distribution<double> chance(0, 100);
    std::deque<Delayed> queue;
    uint64_t dropped = 0;
    bool injected = false;
    const std::string allowed = " SYN SYN_ACK DATA ACK FIN FIN_ACK BAD OPEN_ACK CLOSE_ACK RESET CORRUPT ";
    if (!fault.empty() && (fault.find(' ') != std::string::npos || allowed.find(" " + fault + " ") == std::string::npos))
        throw std::runtime_error("invalid fault type");
    std::cout << "[ROUTER] port=" << listenPort << " loss=" << loss << "% delay=" << delay << "ms corrupt=" << corrupt << "%\n";
    while (true) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        long waitUs = 10000;
        if (!queue.empty()) {
            auto us = std::chrono::duration_cast<std::chrono::microseconds>(queue.front().due - Clock::now()).count();
            waitUs = long(std::max<int64_t>(0, std::min<int64_t>(10000, us)));
        }
        timeval tv{0, waitUs};
#ifdef _WIN32
        int ready = select(0, &rfds, nullptr, nullptr, &tv);
#else
        int ready = select(fd + 1, &rfds, nullptr, nullptr, &tv);
#endif
        if (ready < 0) throw std::runtime_error("select failed");
        if (ready) {
            char buf[65536];
            sockaddr_in from{};
#ifdef _WIN32
            int len = sizeof(from);
#else
            socklen_t len = sizeof(from);
#endif
            int n = recvfrom(fd, buf, int(sizeof(buf)), 0, (sockaddr*)&from, &len);
            if (n < 0) {
#ifdef _WIN32
                if (WSAGetLastError() == WSAECONNRESET) continue;
#endif
                throw std::runtime_error("router recvfrom failed");
            }
            if (n > 0) {
                Packet p;
                if (!decode(buf, n, p)) continue;
                bool fromReceiver = samePeer(from, receiver);
                if (!fromReceiver && ((!known && p.type == SYN) || (known && samePeer(from, sender)))) {
                    sender = from;
                    known = true;
                } else if (!fromReceiver) continue;
                if (!known) continue;
                const char* names[] = {"", "SYN", "SYN_ACK", "DATA", "ACK", "FIN", "FIN_ACK", "BAD", "OPEN_ACK", "CLOSE_ACK", "RESET"};
                bool once = !injected && fault == names[p.type];
                if (once) injected = true;
                if (once || chance(rng) < loss) {
                    ++dropped;
                    std::cout << "[DROP] type=" << p.type << " seq=" << p.seq << " total=" << dropped << '\n';
                } else {
                    bool flip = !injected && fault == "CORRUPT" && p.type == DATA;
                    if (flip) injected = true;
                    if (p.type == DATA && n > HEADER && (flip || chance(rng) < corrupt)) {
                        buf[HEADER] ^= 1;
                        std::cout << "[CORRUPT] seq=" << p.seq << '\n';
                    }
                    queue.push_back({Clock::now() + std::chrono::milliseconds(delay),
                                     fromReceiver ? sender : receiver, std::string(buf, n)});
                }
            }
        }
        auto now = Clock::now();
        while (!queue.empty() && queue.front().due <= now) {
            const auto& d = queue.front();
            if (sendto(fd, d.bytes.data(), int(d.bytes.size()), 0, (const sockaddr*)&d.to, sizeof(d.to)) == SOCKET_ERROR)
                throw std::runtime_error("router sendto failed");
            queue.pop_front();
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
              << "  reliable_udp route LISTEN_PORT DEST_IP DEST_PORT LOSS_PERCENT DELAY_MS [CORRUPT_PERCENT] [FAULT]\n"
              << "Example: recv received.bin 9001 16 800\n";
}
int run(int argc, char** argv) {
    std::cout << std::unitbuf;
    Socket fd = INVALID_SOCKET;
    int status = 0;
#ifdef _WIN32
    bool started = false;
#endif
    try {
#ifdef _WIN32
        WSADATA w{};
        if (WSAStartup(MAKEWORD(2, 2), &w)) throw std::runtime_error("WSAStartup failed");
        started = true;
#endif
        std::string mode = argc > 1 ? argv[1] : "";
        if (mode == "recv" && (argc == 5 || argc == 6)) {
            int port = number(argv[3], 1, 65535);
            int window = number(argv[4], 1, 4096);
            int timeout = argc == 6 ? number(argv[5], 100, 60000) : 800;
            fd = createSocket(port);
            receiveFile(fd, argv[2], port, window, timeout);
        } else if (mode == "send" && (argc == 6 || argc == 7)) {
            auto peer = address(argv[3], number(argv[4], 1, 65535));
            int window = number(argv[5], 1, 4096);
            int timeout = argc == 7 ? number(argv[6], 100, 60000) : 800;
            fd = createSocket();
            sendFile(fd, argv[2], peer, window, timeout);
        } else if (mode == "route" && argc >= 7 && argc <= 9) {
            int port = number(argv[2], 1, 65535);
            auto peer = address(argv[3], number(argv[4], 1, 65535));
            int loss = number(argv[5], 0, 99), delay = number(argv[6], 0, 10000);
            int corrupt = argc >= 8 ? number(argv[7], 0, 99) : 0;
            fd = createSocket(port);
            route(fd, port, peer, loss, delay, corrupt, argc == 9 ? argv[8] : "");
        } else { usage(); status = 1; }
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] " << e.what() << '\n';
        status = 1;
    }
    if (fd != INVALID_SOCKET) closeSock(fd);
#ifdef _WIN32
    if (started) WSACleanup();
#endif
    return status;
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
