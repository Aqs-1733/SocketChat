#pragma once
// 定义网络平台类型。
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma comment(lib, "Ws2_32.lib")
using socket_handle = SOCKET;
constexpr socket_handle BAD_SOCKET = INVALID_SOCKET;
inline bool init_sockets() { WSADATA w{}; return WSAStartup(MAKEWORD(2,2), &w) == 0; }
inline void cleanup_sockets() { WSACleanup(); }
inline void close_socket(socket_handle s) { closesocket(s); }
inline void shutdown_socket(socket_handle s) { shutdown(s, SD_BOTH); }
inline void shutdown_socket_send(socket_handle s) { shutdown(s, SD_SEND); }
inline int socket_last_error() { return WSAGetLastError(); }
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
using socket_handle = int;
constexpr socket_handle BAD_SOCKET = -1;
inline bool init_sockets() { return true; }
inline void cleanup_sockets() { }
inline void close_socket(socket_handle s) { close(s); }
inline void shutdown_socket(socket_handle s) { shutdown(s, SHUT_RDWR); }
inline void shutdown_socket_send(socket_handle s) { shutdown(s, SHUT_WR); }
inline int socket_last_error() { return errno; }
#endif
