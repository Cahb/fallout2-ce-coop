#ifndef FALLOUT_PLATFORM_NET_H_
#define FALLOUT_PLATFORM_NET_H_

// Thin platform shim for the BSD-sockets surface the multiplayer wire uses.
//
// WHY: the netcode was written against POSIX sockets, so the Windows client
// carried `#ifndef _WIN32` guards that compiled the transport OUT — a Windows
// build ran fine as single player but could never join a server. This header
// exists so that transport code can be written ONCE and work on both, rather
// than duplicated per platform.
//
// The Winsock deltas it hides are the classic ones:
//   - SOCKET is an unsigned handle, not an int, and the invalid value is
//     INVALID_SOCKET, not -1 (so `fd < 0` checks are wrong on Windows).
//   - close() -> closesocket().
//   - errno -> WSAGetLastError(), with its own WSAE* constants.
//   - WSAStartup() must run before any socket call.
//   - MSG_DONTWAIT does not exist: non-blocking is a SOCKET MODE set once via
//     ioctlsocket(FIONBIO), not a per-call flag.
//   - MSG_NOSIGNAL is meaningless (no SIGPIPE); the POSIX side still needs it.
//
// Header-only and inline: no new translation unit, so no build-system churn.
// Covers both sides of the wire: the CLIENT connect path and the SERVER
// listen/accept path (server_net.cc), so f2_server cross-builds too.

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
// <winsock2.h> must precede <windows.h>; nothing here includes the latter.
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include <cstddef>
#include <cstring>

namespace fallout {

#ifdef _WIN32
typedef SOCKET NetSocket;
constexpr NetSocket kNetInvalidSocket = INVALID_SOCKET;
#else
typedef int NetSocket;
constexpr NetSocket kNetInvalidSocket = -1;
#endif

// A socket handle is unsigned on Windows, so "is it valid" must be a comparison
// against the sentinel, never `< 0`.
inline bool netSocketValid(NetSocket s)
{
    return s != kNetInvalidSocket;
}

// Process-wide transport init. Idempotent; safe to call before every connect.
// No-op on POSIX apart from suppressing SIGPIPE, which is the equivalent
// "a dead peer must not kill the process" measure.
inline bool netPlatformInit()
{
#ifdef _WIN32
    static bool started = false;
    if (started) {
        return true;
    }
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        return false;
    }
    started = true;
    return true;
#else
    signal(SIGPIPE, SIG_IGN);
    return true;
#endif
}

inline void netCloseSocket(NetSocket s)
{
    if (!netSocketValid(s)) {
        return;
    }
#ifdef _WIN32
    closesocket(s);
#else
    ::close(s);
#endif
}

inline int netLastError()
{
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

// "No data right now" on a non-blocking socket — the normal end of a drain loop.
inline bool netErrorWouldBlock(int err)
{
#ifdef _WIN32
    return err == WSAEWOULDBLOCK;
#else
    return err == EAGAIN || err == EWOULDBLOCK;
#endif
}

// Interrupted by a signal — retry. Cannot happen on Windows, so it is always
// false there rather than a bogus mapping.
inline bool netErrorInterrupted(int err)
{
#ifdef _WIN32
    (void)err;
    return false;
#else
    return err == EINTR;
#endif
}

inline const char* netErrorString(int err)
{
#ifdef _WIN32
    // FormatMessage would need LocalFree bookkeeping for a diagnostic string;
    // the numeric WSA code is enough to identify a fault and cannot leak.
    static thread_local char buf[32];
    snprintf(buf, sizeof(buf), "WSA error %d", err);
    return buf;
#else
    return strerror(err);
#endif
}

// Non-blocking is a socket MODE, not a per-recv flag (Windows has no
// MSG_DONTWAIT). Set it once after connect and use plain recv afterwards.
inline bool netSetNonBlocking(NetSocket s, bool nonBlocking)
{
#ifdef _WIN32
    u_long mode = nonBlocking ? 1 : 0;
    return ioctlsocket(s, FIONBIO, &mode) == 0;
#else
    int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    flags = nonBlocking ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return fcntl(s, F_SETFL, flags) == 0;
#endif
}

// recv/send wrappers returning a signed count (>0 bytes, 0 = peer closed,
// <0 = error, inspect netLastError()). The socket is expected to be in
// non-blocking mode already (netSetNonBlocking), which is what replaces
// MSG_DONTWAIT portably.
inline long netRecv(NetSocket s, void* buf, size_t len)
{
#ifdef _WIN32
    return (long)::recv(s, (char*)buf, (int)len, 0);
#else
    return (long)::recv(s, buf, len, 0);
#endif
}

inline long netSend(NetSocket s, const void* buf, size_t len)
{
#ifdef _WIN32
    return (long)::send(s, (const char*)buf, (int)len, 0);
#else
    // MSG_NOSIGNAL is the per-call belt to the signal(SIGPIPE) braces above.
    return (long)::send(s, buf, len, MSG_NOSIGNAL);
#endif
}

inline void netSetNoDelay(NetSocket s)
{
    int one = 1;
#ifdef _WIN32
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
#else
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#endif
}

// Let a listener rebind its port immediately after a previous serve exited
// (TIME_WAIT). POSIX-only semantics: on Windows SO_REUSEADDR means "steal the
// port even while another socket is actively bound" — a footgun, and a plain
// re-bind after close already succeeds there, so it is a no-op.
inline void netSetReuseAddr(NetSocket s)
{
#ifdef _WIN32
    (void)s;
#else
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#endif
}

// Bound a BLOCKING send: past `seconds` the send fails (POSIX: EAGAIN;
// Windows: WSAETIMEDOUT, after which the socket should be dropped) instead of
// stalling the caller forever on a peer that stopped draining.
inline void netSetSendTimeout(NetSocket s, int seconds)
{
#ifdef _WIN32
    DWORD ms = (DWORD)seconds * 1000;
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&ms, sizeof(ms));
#else
    struct timeval tv;
    tv.tv_sec = seconds;
    tv.tv_usec = 0;
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}

// Non-blocking recv on a socket left in BLOCKING mode (the server keeps its
// wire clients blocking so bounded sends work — see netSetSendTimeout). POSIX
// has the per-call MSG_DONTWAIT flag; Windows does not, so readiness is probed
// with a zero-timeout select first — recv is only called when it cannot block,
// which also preserves the "recv==0 means orderly close" signal. Same return
// contract as netRecv; "nothing pending" reads as netErrorWouldBlock.
inline long netRecvNoWait(NetSocket s, void* buf, size_t len)
{
#ifdef _WIN32
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(s, &readable);
    TIMEVAL tv = { 0, 0 };
    // Winsock ignores select's first parameter.
    int ready = select(0, &readable, nullptr, nullptr, &tv);
    if (ready == 0) {
        WSASetLastError(WSAEWOULDBLOCK);
        return -1;
    }
    if (ready < 0) {
        return -1;
    }
    return (long)::recv(s, (char*)buf, (int)len, 0);
#else
    return (long)::recv(s, buf, len, MSG_DONTWAIT);
#endif
}

} // namespace fallout

#endif /* FALLOUT_PLATFORM_NET_H_ */
