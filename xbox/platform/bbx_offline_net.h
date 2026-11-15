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
// This header is the console's answer: after `<winsock2.h>` (which the Store SDK
// does have, and which supplies the types and constants), it renames the eight
// entry points these sources call, plus the two Winsock error helpers, onto
// inline functions that fail the way an unreachable network does. Nothing here
// imports `WS2_32.dll`, nothing allocates, and every call site keeps its shape -
// the guest's own error paths already handle "no network" and are what a player
// without a peer sees anyway.
//
// What remains honest: `getaddrinfo`/`freeaddrinfo` are fenced the same way (they
// live in ws2_32 too), and `WSAPoll` reports "no descriptor ready", which is the
// same answer as a poll on a socket that will never receive.
#pragma once

#if !defined(_WIN32)
#error "bbx_offline_net.h is the console's Winsock stand-in; only a Windows build includes it"
#endif

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstdint>

#if defined(BBX_UWP)

namespace bbx::offline_net {

inline SOCKET Socket(int, int, int) { return INVALID_SOCKET; }
inline int Bind(SOCKET, const sockaddr*, int) { return SOCKET_ERROR; }
inline int Connect(SOCKET, const sockaddr*, int) { return SOCKET_ERROR; }
inline int Getsockname(SOCKET, sockaddr*, int*) { return SOCKET_ERROR; }
inline int Setsockopt(SOCKET, int, int, const char*, int) { return SOCKET_ERROR; }
inline int Send(SOCKET, const char*, int, int) { return SOCKET_ERROR; }
inline int Sendto(SOCKET, const char*, int, int, const sockaddr*, int) { return SOCKET_ERROR; }
inline int Recv(SOCKET, char*, int, int) { return SOCKET_ERROR; }
inline int Recvfrom(SOCKET, char*, int, int, sockaddr*, int*) { return SOCKET_ERROR; }
inline int Closesocket(SOCKET) { return 0; }
inline int Ioctlsocket(SOCKET, long, u_long*) { return SOCKET_ERROR; }
inline int Select(int, fd_set*, fd_set*, fd_set*, const timeval*) { return 0; }
inline int WsaPoll(LPWSAPOLLFD, ULONG, INT) { return 0; }
inline int WsaStartup(WORD, LPWSADATA) { return 0; }
inline int WsaCleanup() { return 0; }
// The error the guest's code sees: "network is down", not "would block", so a
// retry loop gives up instead of spinning.
inline int WsaGetLastError() { return WSAENETDOWN; }
inline int Getaddrinfo(const char*, const char*, const ADDRINFOA*, ADDRINFOA**) { return WSAHOST_NOT_FOUND; }
inline void Freeaddrinfo(ADDRINFOA*) {}

}  // namespace bbx::offline_net

// The names, renamed. `htons`/`ntohl` and the `FD_*` macros are Winsock's own and
// expand to arithmetic, so they stay as they are.
#define socket bbx::offline_net::Socket
#define bind bbx::offline_net::Bind
#define connect bbx::offline_net::Connect
#define getsockname bbx::offline_net::Getsockname
#define setsockopt bbx::offline_net::Setsockopt
#define send bbx::offline_net::Send
#define sendto bbx::offline_net::Sendto
#define recv bbx::offline_net::Recv
#define recvfrom bbx::offline_net::Recvfrom
#define closesocket bbx::offline_net::Closesocket
#define ioctlsocket bbx::offline_net::Ioctlsocket
#define select bbx::offline_net::Select
#define WSAPoll bbx::offline_net::WsaPoll
#define WSAStartup bbx::offline_net::WsaStartup
#define WSACleanup bbx::offline_net::WsaCleanup
#define WSAGetLastError bbx::offline_net::WsaGetLastError
#define getaddrinfo bbx::offline_net::Getaddrinfo
#define freeaddrinfo bbx::offline_net::Freeaddrinfo

#endif  // BBX_UWP
