// SPDX-License-Identifier: GPL-3.0-or-later
// The network, offline: Winsock for a title that is not allowed to have one.
//
// `hle/net.cpp` and the `src/net` stack call Winsock for the guest's P2P and
// matchmaking. `xbox/AppxManifest.xml` grants no `internetClient` capability, and
// `xbox/scripts/check-imports.ps1` refuses `WS2_32.dll` in the import table - so
// the moment those translation units are linked into `bbxbox.exe`, a single
// `socket()` call fails the packaging audit even if it is never reached (the
// import is in the table whether or not the branch runs).
//
// This header is the console's answer, and it *replaces* `<winsock2.h>` rather
// than shadowing it: the Store SDK's header marks every entry point
// `__declspec(dllimport)`, which cannot be given a definition, and renaming the
// call sites with macros collides with member functions that happen to be called
// `send` (`src/net/nat_probe.cpp` has one). So the small surface these sources
// touch is spelled out here - the types, the thirteen constants, the byte-order
// helpers - and the entry points are inline functions that fail the way an
// unreachable network does. Nothing imports `WS2_32.dll`, nothing allocates, and
// every call site keeps its shape: the guest's own error paths already handle
// "no network", and that is what a player without a peer sees anyway.
#pragma once

#if !defined(_WIN32)
#error "bbx_offline_net.h is the console's Winsock stand-in; only a Windows build includes it"
#endif

#if !defined(BBX_UWP)
#include <winsock2.h>
#include <ws2tcpip.h>
#else

#include <cstddef>
#include <cstdint>

// --- the types -------------------------------------------------------------
using SOCKET = std::uintptr_t;
using UINT_PTR = std::uintptr_t;
using u_long = unsigned long;
using u_short = unsigned short;
using WORD = unsigned short;
using ULONG = unsigned long;
using INT = int;

inline constexpr SOCKET INVALID_SOCKET = static_cast<SOCKET>(~0ull);
inline constexpr int SOCKET_ERROR = -1;

inline constexpr int AF_INET = 2;
inline constexpr int SOCK_DGRAM = 2;
inline constexpr int IPPROTO_UDP = 17;
inline constexpr int SOL_SOCKET = 0xffff;
inline constexpr int SO_REUSEADDR = 0x0004;
inline constexpr int FIONBIO = 0x8004667e;
inline constexpr int MSG_DONTWAIT = 0x0001;
// The Winsock error codes and the poll events are macros in the Windows headers
// (definitions come in through <windows.h>'s error tables), so they are taken as
// they are rather than redeclared.
#ifndef WSAEWOULDBLOCK
#define WSAEWOULDBLOCK 10035
#endif
#ifndef WSAENETDOWN
#define WSAENETDOWN 10050
#endif
#ifndef WSAHOST_NOT_FOUND
#define WSAHOST_NOT_FOUND 11001
#endif
#ifndef POLLIN
#define POLLIN 0x0001
#endif
#ifndef POLLOUT
#define POLLOUT 0x0004
#endif
#ifndef POLLERR
#define POLLERR 0x0008
#endif

#if !defined(_SOCKLEN_T_DEFINED)
#define _SOCKLEN_T_DEFINED
using socklen_t = int;
#endif

struct sockaddr {
    u_short sa_family;
    char sa_data[14];
};

struct in_addr {
    unsigned long s_addr;
};

struct sockaddr_in {
    short sin_family;
    u_short sin_port;
    in_addr sin_addr;
    char sin_zero[8];
};

struct timeval {
    long tv_sec;
    long tv_usec;
};

struct fd_set {
    unsigned int fd_count;
    SOCKET fd_array[64];
};

struct WSADATA {
    WORD wVersion;
    char reserved[400];
};

struct pollfd {
    SOCKET fd;
    short events;
    short revents;
};
using WSAPOLLFD = pollfd;
using LPWSAPOLLFD = pollfd*;

struct addrinfo {
    int ai_flags;
    int ai_family;
    int ai_socktype;
    int ai_protocol;
    std::size_t ai_addrlen;
    char* ai_canonname;
    sockaddr* ai_addr;
    addrinfo* ai_next;
};
using ADDRINFOA = addrinfo;
using PADDRINFOA = addrinfo*;

// The `FD_*` macros are the set operations the sources use, over the array
// above. No socket is ever valid, so in practice the set is always empty.
#define FD_ZERO(set) ((set)->fd_count = 0)
#define FD_SET(fd, set)                                                                 \
    do {                                                                                \
        if ((set)->fd_count < 64) (set)->fd_array[(set)->fd_count++] = (fd);             \
    } while (0)
#define FD_ISSET(fd, set) 0
#define FD_CLR(fd, set) ((void)0)

// --- the byte-order helpers (arithmetic, never a call) ---------------------
inline u_short htons(u_short v) { return static_cast<u_short>((v << 8) | (v >> 8)); }
inline u_short ntohs(u_short v) { return htons(v); }
inline u_long htonl(u_long v) {
    return ((v & 0x000000ffu) << 24) | ((v & 0x0000ff00u) << 8) | ((v & 0x00ff0000u) >> 8) | ((v & 0xff000000u) >> 24);
}
inline u_long ntohl(u_long v) { return htonl(v); }

// --- the entry points ------------------------------------------------------
namespace bbx::offline_net {

inline SOCKET socket(int, int, int) { return INVALID_SOCKET; }
inline int bind(SOCKET, const sockaddr*, socklen_t) { return SOCKET_ERROR; }
inline int connect(SOCKET, const sockaddr*, socklen_t) { return SOCKET_ERROR; }
inline int getsockname(SOCKET, sockaddr*, socklen_t*) { return SOCKET_ERROR; }
inline int setsockopt(SOCKET, int, int, const char*, socklen_t) { return SOCKET_ERROR; }
inline int send(SOCKET, const char*, int, int) { return SOCKET_ERROR; }
inline int sendto(SOCKET, const char*, int, int, const sockaddr*, socklen_t) { return SOCKET_ERROR; }
inline int recv(SOCKET, char*, int, int) { return SOCKET_ERROR; }
inline int recvfrom(SOCKET, char*, int, int, sockaddr*, socklen_t*) { return SOCKET_ERROR; }
inline int closesocket(SOCKET) { return 0; }
inline int ioctlsocket(SOCKET, long, u_long*) { return SOCKET_ERROR; }
inline int select(int, fd_set*, fd_set*, fd_set*, const timeval*) { return 0; }
inline int WSAPoll(LPWSAPOLLFD, ULONG, INT) { return 0; }
inline int WSAStartup(WORD, WSADATA*) { return 0; }
inline int WSACleanup() { return 0; }
// "Network is down", not "would block": a retry loop gives up instead of spinning.
inline int WSAGetLastError() { return WSAENETDOWN; }
inline int getaddrinfo(const char*, const char*, const ADDRINFOA*, ADDRINFOA**) { return WSAHOST_NOT_FOUND; }
inline void freeaddrinfo(ADDRINFOA*) {}

}  // namespace bbx::offline_net

using bbx::offline_net::bind;
using bbx::offline_net::closesocket;
using bbx::offline_net::connect;
using bbx::offline_net::freeaddrinfo;
using bbx::offline_net::getaddrinfo;
using bbx::offline_net::getsockname;
using bbx::offline_net::ioctlsocket;
using bbx::offline_net::recv;
using bbx::offline_net::recvfrom;
using bbx::offline_net::select;
using bbx::offline_net::send;
using bbx::offline_net::sendto;
using bbx::offline_net::setsockopt;
using bbx::offline_net::socket;
using bbx::offline_net::WSACleanup;
using bbx::offline_net::WSAGetLastError;
using bbx::offline_net::WSAPoll;
using bbx::offline_net::WSAStartup;

#endif  // BBX_UWP
