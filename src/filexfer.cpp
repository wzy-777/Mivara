/*
 * FILEXFER.CPP - authorized, independent single-file transfer on TCP 8086.
 *
 * Variable-length payloads use explicit big-endian byte encoding.  The only
 * native wire structs are the fixed, packed structs shared with proto.h.
 */
#include "filexfer.h"
#include "proto.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdlib>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#  include <winternl.h>
#else
#  include <arpa/inet.h>
#  include <cstdio>
#  include <dirent.h>
#  include <fcntl.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <pwd.h>
#  include <sys/select.h>
#  include <sys/socket.h>
#  include <sys/stat.h>
#  include <sys/statvfs.h>
#  ifdef __linux__
#    include <sys/syscall.h>
#  endif
#  include <sys/time.h>
#  include <sys/types.h>
#  include <unistd.h>
#endif

#ifndef O_DIRECTORY
#  define O_DIRECTORY 0
#endif
#ifndef AT_SYMLINK_NOFOLLOW
#  define AT_SYMLINK_NOFOLLOW 0
#endif
#if defined(__linux__) && defined(SYS_renameat2)
#  ifndef RENAME_NOREPLACE
#    define RENAME_NOREPLACE 1
#  endif
#endif
#if defined(__APPLE__)
#  ifndef RENAME_EXCL
#    define RENAME_EXCL 0x00000004
#  endif
/* Declared by <stdio.h> only at full Darwin C level; repeat the prototype so
 * -std=c++17 with strict ANSI still compiles. */
extern "C" int renameatx_np(int, const char *, int, const char *,
                            unsigned int);
#endif

namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

#ifdef _WIN32
using Sock = SOCKET;
static const Sock kInvalidSock = INVALID_SOCKET;
static const int kShutdownSend = SD_SEND;
static const int kShutdownBoth = SD_BOTH;
#else
using Sock = int;
static const Sock kInvalidSock = -1;
static const int kShutdownSend = SHUT_WR;
static const int kShutdownBoth = SHUT_RDWR;
#endif

void closeSock(Sock s)
{
    if (s == kInvalidSock) return;
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
}

void shutdownSock(Sock s, int how = kShutdownBoth)
{
    if (s == kInvalidSock) return;
    shutdown(s, how);
}

class SockHolder {
public:
    SockHolder() = default;
    explicit SockHolder(Sock s) : _s(s) {}
    ~SockHolder() { reset(); }
    SockHolder(const SockHolder &) = delete;
    SockHolder &operator=(const SockHolder &) = delete;
    Sock get() const { return _s; }
    bool valid() const { return _s != kInvalidSock; }
    void reset(Sock s = kInvalidSock)
    {
        if (_s != kInvalidSock) closeSock(_s);
        _s = s;
    }
private:
    Sock _s = kInvalidSock;
};

bool ensureSockets(int *startup_error = nullptr)
{
#ifdef _WIN32
    static std::once_flag once;
    static bool ok = false;
    static int error = 0;
    std::call_once(once, [] {
        WSADATA data{};
        error = WSAStartup(MAKEWORD(2, 2), &data);
        ok = error == 0;
    });
    if (startup_error) *startup_error = error;
    return ok;
#else
    if (startup_error) *startup_error = 0;
    return true;
#endif
}

int sockError()
{
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

bool wouldBlock(int e)
{
#ifdef _WIN32
    return e == WSAEWOULDBLOCK || e == WSAEINTR;
#else
    return e == EAGAIN || e == EWOULDBLOCK || e == EINTR;
#endif
}

bool setNonblocking(Sock s)
{
#ifdef _WIN32
    u_long on = 1;
    return ioctlsocket(s, FIONBIO, &on) == 0;
#else
    int flags = fcntl(s, F_GETFL, 0);
    return flags >= 0 && fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

void tuneSock(Sock s)
{
    int on = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&on, sizeof(on));
    setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, (const char *)&on, sizeof(on));
}

enum class IoResult { Ok, Timeout, Stopped, Closed, Error, Protocol };

IoResult waitSock(Sock s, bool read, const std::atomic<bool> &running,
                  Clock::time_point deadline)
{
    for (;;) {
        if (!running.load(std::memory_order_acquire)) return IoResult::Stopped;
        const auto now = Clock::now();
        if (now >= deadline) return IoResult::Timeout;
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - now);
        long slice = (long)std::min<int64_t>(1000, std::max<int64_t>(1, left.count()));
        fd_set rd, wr;
        FD_ZERO(&rd);
        FD_ZERO(&wr);
        FD_SET(s, read ? &rd : &wr);
        timeval tv{};
        tv.tv_sec = slice / 1000;
        tv.tv_usec = (slice % 1000) * 1000;
#ifdef _WIN32
        int rc = select(0, read ? &rd : nullptr, read ? nullptr : &wr,
                        nullptr, &tv);
#else
        int rc = select(s + 1, read ? &rd : nullptr, read ? nullptr : &wr,
                        nullptr, &tv);
#endif
        if (rc > 0) return IoResult::Ok;
        if (rc == 0) continue;
        if (wouldBlock(sockError())) continue;
        return IoResult::Error;
    }
}

IoResult recvAll(Sock s, void *buf, size_t len,
                 const std::atomic<bool> &running, int timeout_ms)
{
    char *p = static_cast<char *>(buf);
    size_t off = 0;
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    while (off < len) {
        IoResult wr = waitSock(s, true, running, deadline);
        if (wr != IoResult::Ok) return wr;
        int amount = (int)std::min<size_t>(len - off,
                                           static_cast<size_t>(INT_MAX));
        int n = recv(s, p + off, amount, 0);
        if (n > 0) {
            off += static_cast<size_t>(n);
            continue;
        }
        if (n == 0) return IoResult::Closed;
        int e = sockError();
        if (wouldBlock(e)) continue;
        return IoResult::Error;
    }
    return IoResult::Ok;
}

IoResult sendAll(Sock s, const void *buf, size_t len,
                 const std::atomic<bool> &running, int timeout_ms)
{
    const char *p = static_cast<const char *>(buf);
    size_t off = 0;
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    while (off < len) {
        IoResult wr = waitSock(s, false, running, deadline);
        if (wr != IoResult::Ok) return wr;
        int amount = (int)std::min<size_t>(len - off,
                                           static_cast<size_t>(INT_MAX));
        int flags = 0;
#ifdef MSG_NOSIGNAL
        flags |= MSG_NOSIGNAL;
#endif
        int n = send(s, p + off, amount, flags);
        if (n > 0) {
            off += static_cast<size_t>(n);
            continue;
        }
        int e = sockError();
        if (n < 0 && wouldBlock(e)) continue;
        return IoResult::Error;
    }
    return IoResult::Ok;
}

/* The app client only ever connects to the numeric address recorded for a
 * recent connection.  Rejecting hostnames before the worker starts keeps a
 * synchronous getaddrinfo() lookup from blocking Stop's join on a stalled DNS
 * resolver.  An optional IPv6 zone id is allowed; the service itself is still
 * IPv4-only. */
bool numericHost(const std::string &host)
{
    if (host.empty()) return false;
    in_addr v4{};
    if (inet_pton(AF_INET, host.c_str(), &v4) == 1) return true;
    std::string text = host;
    std::string zone;
    const size_t scope = text.find('%');
    if (scope != std::string::npos) {
        zone = text.substr(scope + 1);
        text.resize(scope);
        if (zone.empty()) return false;
    }
    in6_addr v6{};
    if (inet_pton(AF_INET6, text.c_str(), &v6) != 1) return false;
    if (scope == std::string::npos) return true;
    for (size_t i = 0; i < zone.size(); ++i) {
        const char c = zone[i];
#ifdef _WIN32
        /* Winsock only accepts a numeric interface index in a scope id. */
        if (c < '0' || c > '9') return false;
#else
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
            return false;
#endif
    }
    return true;
}

Sock connectSock(const std::string &ip, uint16_t port,
                 const std::atomic<bool> &running, int timeout_ms)
{
    if (!ensureSockets()) return kInvalidSock;
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
    addrinfo *addresses = nullptr;
    const std::string service = std::to_string(port);
    if (getaddrinfo(ip.c_str(), service.c_str(), &hints, &addresses) != 0)
        return kInvalidSock;
    struct AddrGuard {
        addrinfo *p;
        ~AddrGuard() { if (p) freeaddrinfo(p); }
    } guard{addresses};

    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    for (addrinfo *a = addresses; a; a = a->ai_next) {
        if (!running.load(std::memory_order_acquire)) return kInvalidSock;
        Sock s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == kInvalidSock) continue;
        if (!setNonblocking(s)) {
            closeSock(s);
            continue;
        }
        int rc = connect(s, a->ai_addr, (int)a->ai_addrlen);
        if (rc != 0 && !wouldBlock(sockError())) {
#ifdef _WIN32
            if (sockError() != WSAEINPROGRESS) {
                closeSock(s);
                continue;
            }
#else
            if (sockError() != EINPROGRESS) {
                closeSock(s);
                continue;
            }
#endif
        }
        while (rc != 0) {
            IoResult wr = waitSock(s, false, running, deadline);
            if (wr != IoResult::Ok) {
                closeSock(s);
                s = kInvalidSock;
                break;
            }
            int error = 0;
#ifdef _WIN32
            int error_len = sizeof(error);
#else
            socklen_t error_len = sizeof(error);
#endif
            if (getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&error,
                           &error_len) != 0 || error != 0) {
                closeSock(s);
                s = kInvalidSock;
                break;
            }
            rc = 0;
        }
        if (s != kInvalidSock) {
            tuneSock(s);
            return s;
        }
    }
    return kInvalidSock;
}

/* ── fixed frames and scalar codecs ────────────────────────────────────── */

#pragma pack(push, 1)
struct FilexferHello {
    uint32_t magic;
    uint32_t version;
    uint32_t reserved[2];
};
struct FilexferReply {
    uint32_t magic;
    uint32_t version;
    uint32_t status;
    uint32_t reserved;
};
#pragma pack(pop)
static_assert(sizeof(FilexferHello) == 16, "FilexferHello must be 16 bytes");
static_assert(sizeof(FilexferReply) == 16, "FilexferReply must be 16 bytes");

enum FtFrameType : uint16_t {
    FTF_HOME = 1,
    FTF_ROOTS = 2,
    FTF_LIST = 3,
    FTF_PUT_BEGIN = 4,
    FTF_GET = 5,
    FTF_DATA = 6,
    FTF_END = 7,
    FTF_RESULT = 8,
    FTF_ABORT = 9,
    FTF_INFO = 10,
    FTF_MKDIR = 11
};

struct Frame {
    uint16_t type = 0;
    std::vector<uint8_t> payload;
};

void append32(std::vector<uint8_t> &v, uint32_t x)
{
    v.push_back((uint8_t)(x >> 24));
    v.push_back((uint8_t)(x >> 16));
    v.push_back((uint8_t)(x >> 8));
    v.push_back((uint8_t)x);
}

void append64(std::vector<uint8_t> &v, uint64_t x)
{
    append32(v, (uint32_t)(x >> 32));
    append32(v, (uint32_t)x);
}

void appendString(std::vector<uint8_t> &v, const std::string &s)
{
    append32(v, (uint32_t)s.size());
    v.insert(v.end(), s.begin(), s.end());
}

class Cursor {
public:
    Cursor(const uint8_t *data, size_t size) : _p(data), _n(size) {}
    bool u8(uint8_t &x)
    {
        if (_n < 1) return false;
        x = *_p++;
        --_n;
        return true;
    }
    bool u32(uint32_t &x)
    {
        if (_n < 4) return false;
        x = ((uint32_t)_p[0] << 24) | ((uint32_t)_p[1] << 16) |
            ((uint32_t)_p[2] << 8) | (uint32_t)_p[3];
        _p += 4;
        _n -= 4;
        return true;
    }
    bool u64(uint64_t &x)
    {
        uint32_t hi = 0, lo = 0;
        if (!u32(hi) || !u32(lo)) return false;
        x = ((uint64_t)hi << 32) | lo;
        return true;
    }
    bool bytes(void *out, size_t n)
    {
        if (_n < n) return false;
        if (n) std::memcpy(out, _p, n);
        _p += n;
        _n -= n;
        return true;
    }
    bool string(std::string &out, size_t cap)
    {
        uint32_t n = 0;
        if (!u32(n) || n > cap || _n < n) return false;
        out.assign((const char *)_p, n);
        _p += n;
        _n -= n;
        return true;
    }
    bool done() const { return _n == 0; }
private:
    const uint8_t *_p;
    size_t _n;
};

bool validStatus(uint32_t status)
{
    return status <= (uint32_t)FT_ERR_PROTOCOL;
}

IoResult sendFrame(Sock s, uint16_t type, const std::vector<uint8_t> &payload,
                   const std::atomic<bool> &running, int timeout_ms = 30000)
{
    if (payload.size() > FILEXFER_MAX_FRAME_BYTES) return IoResult::Protocol;
    uint8_t h[16];
    const uint32_t magic = FILEXFER_MAGIC;
    const uint32_t length = (uint32_t)payload.size();
    h[0] = (uint8_t)(magic >> 24); h[1] = (uint8_t)(magic >> 16);
    h[2] = (uint8_t)(magic >> 8); h[3] = (uint8_t)magic;
    h[4] = (uint8_t)(FILEXFER_VERSION >> 8); h[5] = (uint8_t)FILEXFER_VERSION;
    h[6] = (uint8_t)(type >> 8); h[7] = (uint8_t)type;
    h[8] = (uint8_t)(length >> 24); h[9] = (uint8_t)(length >> 16);
    h[10] = (uint8_t)(length >> 8); h[11] = (uint8_t)length;
    h[12] = h[13] = h[14] = h[15] = 0;
    IoResult rc = sendAll(s, h, sizeof(h), running, timeout_ms);
    if (rc != IoResult::Ok || payload.empty()) return rc;
    return sendAll(s, payload.data(), payload.size(), running, timeout_ms);
}

IoResult recvFrame(Sock s, Frame &frame, const std::atomic<bool> &running,
                   int timeout_ms)
{
    uint8_t h[16];
    frame.payload.clear();
    IoResult rc = recvAll(s, h, sizeof(h), running, timeout_ms);
    if (rc != IoResult::Ok) return rc;
    const uint32_t magic = ((uint32_t)h[0] << 24) | ((uint32_t)h[1] << 16) |
                           ((uint32_t)h[2] << 8) | h[3];
    const uint16_t version = (uint16_t)(((uint16_t)h[4] << 8) | h[5]);
    const uint16_t type = (uint16_t)(((uint16_t)h[6] << 8) | h[7]);
    const uint32_t length = ((uint32_t)h[8] << 24) | ((uint32_t)h[9] << 16) |
                            ((uint32_t)h[10] << 8) | h[11];
    if (magic != FILEXFER_MAGIC || version != FILEXFER_VERSION ||
        h[12] || h[13] || h[14] || h[15] ||
        length > FILEXFER_MAX_FRAME_BYTES)
        return IoResult::Protocol;
    frame.type = type;
    try {
        frame.payload.resize(length);
    } catch (...) {
        return IoResult::Error;
    }
    if (!length) return IoResult::Ok;
    return recvAll(s, frame.payload.data(), length, running, timeout_ms);
}

/* ── UTF-8 and lexical path validation ─────────────────────────────────── */

bool validUtf8(const std::string &s)
{
    size_t i = 0;
    while (i < s.size()) {
        const uint8_t c = (uint8_t)s[i++];
        if (c < 0x80) continue;
        unsigned need = 0;
        uint32_t cp = 0;
        if (c >= 0xC2 && c <= 0xDF) { need = 1; cp = c & 0x1F; }
        else if (c >= 0xE0 && c <= 0xEF) { need = 2; cp = c & 0x0F; }
        else if (c >= 0xF0 && c <= 0xF4) { need = 3; cp = c & 0x07; }
        else return false;
        if (i + need > s.size()) return false;
        for (unsigned j = 0; j < need; ++j) {
            const uint8_t d = (uint8_t)s[i++];
            if ((d & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (d & 0x3F);
        }
        if ((need == 2 && cp < 0x800) || (need == 3 && cp < 0x10000) ||
            (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF)
            return false;
    }
    return true;
}

#ifdef _WIN32
bool windowsReservedName(const std::string &name)
{
    std::string base = name.substr(0, name.find('.'));
    for (char &c : base)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    if (base == "con" || base == "prn" || base == "aux" || base == "nul")
        return true;
    return base.size() == 4 &&
           (base.compare(0, 3, "com") == 0 || base.compare(0, 3, "lpt") == 0) &&
           base[3] >= '1' && base[3] <= '9';
}
#endif

/* Wire names must be portable across every platform we build for.  They are
 * one UTF-8 path component and must never contain a separator on any OS, so a
 * name accepted here can be sent to a peer regardless of the sender's local
 * filesystem rules.  Local-only rules live in validLocalName below. */
bool validWireName(const std::string &name)
{
    if (name.empty() || name.size() > FILEXFER_MAX_NAME_BYTES ||
        !validUtf8(name) || name == "." || name == ".." ||
        name.find('\0') != std::string::npos ||
        name.find('/') != std::string::npos ||
        name.find('\\') != std::string::npos)
        return false;
    for (unsigned char c : name)
        if (c < 0x20 || c == 0x7f) return false;
    return true;
}

/* Additional rules for names this process may create in its local filesystem.
 * A POSIX peer may legitimately list "a:b" or "con"; a Windows client must be
 * able to display that listing and report a local-name error only when the
 * user tries to download it. */
bool validLocalName(const std::string &name)
{
    if (!validWireName(name)) return false;
#ifdef _WIN32
    if (name.back() == '.' || name.back() == ' ' ||
        name.find_first_of("<>:\"|?*") != std::string::npos ||
        windowsReservedName(name))
        return false;
#endif
    return true;
}

bool driveRootName(const std::string &name)
{
    return name.size() == 2 &&
           ((name[0] >= 'A' && name[0] <= 'Z') ||
            (name[0] >= 'a' && name[0] <= 'z')) && name[1] == ':';
}

bool wireAbsolute(const std::string &path)
{
    if (path.empty()) return false;
    if (path[0] == '/' || path[0] == '\\') {
        if (path[0] == '/') return true;
        return path.size() >= 2 && path[1] == '\\';
    }
    return path.size() >= 3 &&
           ((path[0] >= 'A' && path[0] <= 'Z') ||
            (path[0] >= 'a' && path[0] <= 'z')) && path[1] == ':' &&
           (path[2] == '/' || path[2] == '\\');
}

bool windowsStylePath(const std::string &path)
{
    return (path.size() >= 2 && path[0] == '\\' && path[1] == '\\') ||
           (path.size() >= 3 && driveRootName(path.substr(0, 2)) &&
            (path[2] == '/' || path[2] == '\\'));
}

bool validPathText(const std::string &path, bool allow_empty)
{
    if (path.empty()) return allow_empty;
    if (path.size() > FILEXFER_MAX_PATH_BYTES || !validUtf8(path) ||
        path.find('\0') != std::string::npos)
        return false;
    for (unsigned char c : path)
        if (c < 0x20 || c == 0x7f) return false;
    return true;
}

/* std::filesystem on MinGW treats a UNC path as relative
 * (root_name() is empty for "\\\\server\\share"), so it cannot be used as the
 * Windows absolute-path authority. */
bool absolutePathText(const fs::path &p)
{
#ifdef _WIN32
    const std::wstring w = p.native();
    if (w.size() >= 2 && (w[0] == L'\\' || w[0] == L'/') &&
        (w[1] == L'\\' || w[1] == L'/'))
        return true;
    return w.size() >= 3 &&
           ((w[0] >= L'A' && w[0] <= L'Z') ||
            (w[0] >= L'a' && w[0] <= L'z')) &&
           w[1] == L':' && (w[2] == L'\\' || w[2] == L'/');
#else
    return p.is_absolute();
#endif
}

#ifdef _WIN32
std::wstring apiPath(const fs::path &p)
{
    std::wstring w = p.native();
    /* The \\?\ prefix disables Win32 separator normalization, so a path with
     * forward slashes (e.g. from a POSIX peer) must be converted first. */
    for (wchar_t &c : w)
        if (c == L'/') c = L'\\';
    if (w.compare(0, 4, L"\\\\?\\") == 0) return w;
    /* \\.\ is the Win32 device namespace, not a UNC share. */
    if (w.compare(0, 4, L"\\\\.\\") == 0) return w;
    if (w.compare(0, 2, L"\\\\") == 0)
        return L"\\\\?\\UNC\\" + w.substr(2);
    return L"\\\\?\\" + w;
}
#endif

/* ── handle-relative directory access ──────────────────────────────────────
 * Every file read, temporary creation and commit is performed relative to a
 * directory handle that was walked component by component without following
 * links.  The final component is opened with O_NOFOLLOW / OPEN_REPARSE_POINT.
 * Replacing an ancestor after the walk therefore cannot redirect the
 * operation to a different object. */

#ifdef _WIN32
typedef NTSTATUS (NTAPI *NtCreateFileFn)(
    PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK,
    PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);

NtCreateFileFn ntCreateFile()
{
    static std::once_flag once;
    static NtCreateFileFn fn = nullptr;
    std::call_once(once, [] {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (!ntdll) ntdll = LoadLibraryW(L"ntdll.dll");
        if (ntdll) {
            FARPROC proc = GetProcAddress(ntdll, "NtCreateFile");
            if (proc) std::memcpy(&fn, &proc, sizeof(fn));
        }
    });
    return fn;
}

typedef NTSTATUS (NTAPI *NtSetInformationFileFn)(HANDLE, PIO_STATUS_BLOCK,
                                                 PVOID, ULONG, ULONG);

/* SetFileInformationByHandle rejects FILE_RENAME_INFO.RootDirectory with
 * ERROR_INVALID_PARAMETER; the native call supports the relative rename. */
NtSetInformationFileFn ntSetInformationFile()
{
    static std::once_flag once;
    static NtSetInformationFileFn fn = nullptr;
    std::call_once(once, [] {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (!ntdll) ntdll = LoadLibraryW(L"ntdll.dll");
        if (ntdll) {
            FARPROC proc = GetProcAddress(ntdll, "NtSetInformationFile");
            if (proc) std::memcpy(&fn, &proc, sizeof(fn));
        }
    });
    return fn;
}

FtStatus statusFromNt(NTSTATUS status)
{
    switch ((ULONG)status) {
    case 0xC0000034UL: /* STATUS_OBJECT_NAME_NOT_FOUND */
    case 0xC000003AUL: /* STATUS_OBJECT_PATH_NOT_FOUND */
        return FT_ERR_NOT_FOUND;
    case 0xC0000103UL: /* STATUS_NOT_A_DIRECTORY */
    case 0xC00000BAUL: /* STATUS_FILE_IS_A_DIRECTORY */
        return FT_ERR_PATH;
    case 0xC0000035UL: /* STATUS_OBJECT_NAME_COLLISION */
        return FT_ERR_EXISTS;
    case 0xC0000106UL: /* STATUS_NAME_TOO_LONG */
        return FT_ERR_LIMIT;
    case 0xC0000033UL: /* STATUS_OBJECT_NAME_INVALID */
        return FT_ERR_PATH;
    case 0xC000007FUL: /* STATUS_DISK_FULL */
    case 0xC0000044UL: /* STATUS_QUOTA_EXCEEDED */
        return FT_ERR_DISK;
    case 0xC0000022UL: /* STATUS_ACCESS_DENIED */
    case 0xC0000056UL: /* STATUS_DELETE_PENDING */
    default:
        return FT_ERR_IO;
    }
}

FtStatus ntOpenRelative(HANDLE parent, const std::wstring &component,
                        bool directory, ACCESS_MASK access, ULONG share,
                        ULONG disposition, HANDLE &out)
{
    out = INVALID_HANDLE_VALUE;
    NtCreateFileFn create = ntCreateFile();
    if (!create || !parent || parent == INVALID_HANDLE_VALUE ||
        component.empty() || component.size() >= 0x7FFF)
        return FT_ERR_IO;
    UNICODE_STRING name{};
    name.Buffer = const_cast<PWSTR>(component.c_str());
    name.Length = (USHORT)(component.size() * sizeof(wchar_t));
    name.MaximumLength = (USHORT)((component.size() + 1) * sizeof(wchar_t));
    OBJECT_ATTRIBUTES attributes{};
    attributes.Length = sizeof(attributes);
    attributes.RootDirectory = parent;
    attributes.ObjectName = &name;
    attributes.Attributes = OBJ_CASE_INSENSITIVE;
    IO_STATUS_BLOCK iosb{};
    /* FILE_SYNCHRONOUS_IO_NONALERT is deliberately omitted.  Directory handles
     * are only used for traversal, and one relative-open experiment returned
     * STATUS_INVALID_PARAMETER with it; it is not needed here. */
    const ULONG options = FILE_OPEN_REPARSE_POINT |
                          (directory ? FILE_DIRECTORY_FILE
                                     : FILE_NON_DIRECTORY_FILE);
    const NTSTATUS status =
        create(&out, access, &attributes, &iosb, nullptr, FILE_ATTRIBUTE_NORMAL,
               share, disposition, options, nullptr, 0);
    if (status < 0) {
        out = INVALID_HANDLE_VALUE;
        return statusFromNt(status);
    }
    return FT_OK;
}

bool ntHandleIsPlain(HANDLE h, bool directory)
{
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(h, &info)) return false;
    if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) return false;
    const bool is_directory =
        (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    return is_directory == directory;
}

std::wstring wideFromUtf8(const std::string &text)
{
    if (text.empty()) return std::wstring();
    const int chars = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                          (int)text.size(), nullptr, 0);
    if (chars <= 0) return std::wstring();
    std::wstring out((size_t)chars, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), &out[0],
                        chars);
    return out;
}

std::string utf8FromWide(const std::wstring &wide)
{
    if (wide.empty()) return std::string();
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                                          (int)wide.size(), nullptr, 0,
                                          nullptr, nullptr);
    if (bytes <= 0) return std::string();
    std::string out((size_t)bytes, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int)wide.size(), &out[0],
                        bytes, nullptr, nullptr);
    return out;
}

/* GetFullPathNameW is the Windows authority for relative / ".." / mixed
 * separator normalization, including UNC and extended paths. */
bool winFullPath(const std::string &text, std::wstring &full)
{
    const std::wstring wide = wideFromUtf8(text);
    if (wide.empty()) return false;
    DWORD need = GetFullPathNameW(wide.c_str(), 0, nullptr, nullptr);
    if (!need) return false;
    std::wstring buffer((size_t)need, L'\0');
    const DWORD got = GetFullPathNameW(wide.c_str(), need, &buffer[0], nullptr);
    if (!got || got >= need) return false;
    buffer.resize(got);
    full = std::move(buffer);
    return true;
}

/* Split a normalized Windows path into the openable root (drive root, UNC
 * share root or \\?\ device root) and the remaining one-component names. */
bool winSplitRoot(const std::wstring &input, std::wstring &root,
                  std::vector<std::wstring> &parts)
{
    std::wstring w = input;
    for (wchar_t &c : w)
        if (c == L'/') c = L'\\';
    size_t root_end = 0;
    if (w.size() >= 2 && w[0] == L'\\' && w[1] == L'\\') {
        size_t i = 2;
        if (w.compare(0, 8, L"\\\\?\\UNC\\") == 0) {
            i = 8;
        } else if (w.compare(0, 4, L"\\\\?\\") == 0) {
            i = 4;
            const size_t slash = w.find(L'\\', i);
            if (slash == std::wstring::npos) return false;
            root_end = slash + 1;
        }
        if (!root_end) {
            const size_t server_end = w.find(L'\\', i);
            if (server_end == std::wstring::npos) return false;
            const size_t share_end = w.find(L'\\', server_end + 1);
            root_end = share_end == std::wstring::npos ? w.size()
                                                       : share_end + 1;
        }
    } else if (w.size() >= 3 &&
               ((w[0] >= L'A' && w[0] <= L'Z') ||
                (w[0] >= L'a' && w[0] <= L'z')) &&
               w[1] == L':' && w[2] == L'\\') {
        root_end = 3;
    } else {
        return false;
    }
    root = w.substr(0, root_end);
    if (root.empty() || root.back() != L'\\') root += L'\\';
    parts.clear();
    size_t pos = root_end;
    while (pos < w.size()) {
        while (pos < w.size() && w[pos] == L'\\') ++pos;
        if (pos >= w.size()) break;
        size_t next = w.find(L'\\', pos);
        if (next == std::wstring::npos) next = w.size();
        const std::wstring part = w.substr(pos, next - pos);
        if (part == L".") {
            pos = next;
            continue;
        }
        if (part == L"..") return false;
        parts.push_back(part);
        pos = next;
    }
    return true;
}

/* Canonical DOS path for a handle, without the \\?\ / \\?\UNC\ prefixes. */
bool ntHandleFinalPath(HANDLE h, std::wstring &out)
{
    DWORD need = GetFinalPathNameByHandleW(
        h, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!need) return false;
    std::wstring buffer((size_t)need, L'\0');
    DWORD got = GetFinalPathNameByHandleW(
        h, &buffer[0], need, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!got || got >= need) return false;
    buffer.resize(got);
    if (buffer.compare(0, 8, L"\\\\?\\UNC\\") == 0)
        buffer = L"\\\\" + buffer.substr(8);
    else if (buffer.compare(0, 4, L"\\\\?\\") == 0)
        buffer = buffer.substr(4);
    out = std::move(buffer);
    return true;
}

std::string handlePath(HANDLE h, const fs::path &fallback)
{
    std::wstring final_path;
    if (ntHandleFinalPath(h, final_path)) {
        std::string text = utf8FromWide(final_path);
        if (!text.empty()) return text;
    }
    return fallback.u8string();
}

bool ntSameObject(HANDLE a, HANDLE b)
{
    BY_HANDLE_FILE_INFORMATION ia{}, ib{};
    return GetFileInformationByHandle(a, &ia) &&
           GetFileInformationByHandle(b, &ib) &&
           ia.dwVolumeSerialNumber == ib.dwVolumeSerialNumber &&
           ia.nFileIndexHigh == ib.nFileIndexHigh &&
           ia.nFileIndexLow == ib.nFileIndexLow;
}

/* The final component was opened by path (CreateFileW has no relative form
 * usable with synchronous ReadFile/WriteFile), so confirm that the file's
 * live parent is the pinned directory object, not merely a path string.  A
 * junction/symlink swap or a case-sensitive-directory substitution is
 * therefore rejected instead of redirecting the transfer. */
bool ntHandleInDirectory(HANDLE h, HANDLE directory)
{
    std::wstring final_path;
    if (!ntHandleFinalPath(h, final_path)) return false;
    const size_t slash = final_path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return false;
    std::wstring parent = final_path.substr(0, slash);
    if (parent.empty()) return false;
    if (parent.back() != L'\\' && parent.back() != L'/') parent += L'\\';
    HANDLE parent_handle = CreateFileW(
        apiPath(fs::path(parent.c_str())).c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (parent_handle == INVALID_HANDLE_VALUE) return false;
    const bool same = ntSameObject(parent_handle, directory);
    CloseHandle(parent_handle);
    return same;
}
#endif /* _WIN32 */

class DirLock {
public:
    DirLock() = default;
    ~DirLock() { close(); }
    DirLock(const DirLock &) = delete;
    DirLock &operator=(const DirLock &) = delete;

    /* create_child adds FILE_ADD_SUBDIRECTORY to the final directory handle
     * (Windows); read-only callers keep the smaller access mask. */
    FtStatus open(const std::string &text, bool require_absolute,
                  bool create_child = false)
    {
        close();
        if (text.empty() || !validUtf8(text) ||
            text.find('\0') != std::string::npos)
            return FT_ERR_PATH;
        if (require_absolute && !validPathText(text, false))
            return FT_ERR_PATH;
        if (require_absolute && !absolutePathText(fs::u8path(text)))
            return FT_ERR_PATH;
#ifdef _WIN32
        std::wstring full;
        if (!winFullPath(text, full)) return FT_ERR_PATH;
        std::wstring root;
        std::vector<std::wstring> parts;
        if (!winSplitRoot(full, root, parts)) return FT_ERR_PATH;
        const DWORD access = FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES |
                             FILE_TRAVERSE | SYNCHRONIZE;
        /* Creating a child through a handle requires FILE_ADD_SUBDIRECTORY on
         * that directory object; read-only callers keep the least privilege. */
        const DWORD final_access =
            access | (create_child ? FILE_ADD_SUBDIRECTORY : 0);
        HANDLE h = CreateFileW(apiPath(fs::path(root.c_str())).c_str(),
                               parts.empty() ? final_access : access,
                               FILE_SHARE_READ | FILE_SHARE_WRITE |
                               FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING,
                               FILE_FLAG_BACKUP_SEMANTICS |
                               FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            const DWORD e = GetLastError();
            return (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND)
                       ? FT_ERR_NOT_FOUND : FT_ERR_IO;
        }
        if (!ntHandleIsPlain(h, true)) {
            CloseHandle(h);
            return FT_ERR_PATH;
        }
        for (size_t i = 0; i < parts.size(); ++i) {
            const std::wstring &name = parts[i];
            const DWORD child_access =
                (i + 1 == parts.size()) ? final_access : access;
            HANDLE child = INVALID_HANDLE_VALUE;
            FtStatus status = ntOpenRelative(
                h, name, true, child_access,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                FILE_OPEN, child);
            CloseHandle(h);
            if (status != FT_OK) return status;
            if (!ntHandleIsPlain(child, true)) {
                CloseHandle(child);
                return FT_ERR_PATH;
            }
            h = child;
        }
        _handle = h;
        _path = handlePath(h, fs::path(full.c_str()));
#else
        /* Resolve the directory itself once (this is what lets macOS'/var and
         * /tmp symlinks work), then walk the resolved components with
         * O_NOFOLLOW.  A component swapped to a link after the walk therefore
         * still fails with ELOOP instead of redirecting the operation. */
        char *resolved = ::realpath(text.c_str(), nullptr);
        if (!resolved) {
            const int e = errno;
            return (e == ENOENT || e == ENOTDIR) ? FT_ERR_NOT_FOUND :
                   (e == ELOOP) ? FT_ERR_PATH :
                   (e == ENAMETOOLONG) ? FT_ERR_LIMIT : FT_ERR_IO;
        }
        const std::string resolved_text(resolved);
        std::free(resolved);
        const fs::path resolved_path = fs::u8path(resolved_text);
        int base = O_RDONLY | O_DIRECTORY;
#ifdef O_CLOEXEC
        base |= O_CLOEXEC;
#endif
        int fd = ::open("/", base);
        if (fd < 0) return FT_ERR_IO;
        for (const fs::path &part : resolved_path.relative_path()) {
            const std::string name = part.native();
            if (name.empty() || name == ".") continue;
            if (name == "..") {
                ::close(fd);
                return FT_ERR_PATH;
            }
            int flags = base;
#ifdef O_NOFOLLOW
            flags |= O_NOFOLLOW;
#endif
            const int next = ::openat(fd, name.c_str(), flags);
            if (next < 0) {
                const int e = errno;
                ::close(fd);
                return e == ENOENT ? FT_ERR_NOT_FOUND :
                       (e == ENOTDIR || e == ELOOP) ? FT_ERR_PATH : FT_ERR_IO;
            }
            struct stat st{};
            if (fstat(next, &st) != 0 || !S_ISDIR(st.st_mode)) {
                ::close(next);
                ::close(fd);
                return FT_ERR_PATH;
            }
            ::close(fd);
            fd = next;
        }
        _fd = fd;
        _path = resolved_text;
#endif
        if (!validPathText(_path, false)) {
            close();
            return FT_ERR_PATH;
        }
        return FT_OK;
    }

    std::string path() const { return _path; }

    /* Windows file opens still need a path; ask the pinned handle so a
     * directory renamed after DirLock is still usable.  POSIX operations are
     * fd-relative and keep the path captured at lock time. */
    std::string currentPath() const
    {
#ifdef _WIN32
        return handlePath(_handle, fs::u8path(_path));
#else
#ifdef __linux__
        char buffer[4096];
        const std::string link = "/proc/self/fd/" + std::to_string(_fd);
        const ssize_t n = ::readlink(link.c_str(), buffer, sizeof(buffer) - 1);
        if (n > 0) {
            buffer[n] = '\0';
            std::string live(buffer);
            static const char deleted[] = " (deleted)";
            const size_t tail = sizeof(deleted) - 1;
            if (live.size() > tail &&
                live.compare(live.size() - tail, tail, deleted) == 0)
                return _path;
            return live;
        }
#elif defined(__APPLE__)
        char buffer[4096] = {};
        if (_fd >= 0 && ::fcntl(_fd, F_GETPATH, buffer) == 0)
            return std::string(buffer);
#endif
        return _path;
#endif
    }

    size_t nameLimit() const
    {
#ifdef _WIN32
        return 255; /* NTFS/FAT component limit, in UTF-16 units */
#else
        errno = 0;
        long limit = _fd >= 0 ? ::fpathconf(_fd, _PC_NAME_MAX) : -1;
        if (limit <= 0) limit = 255;
        if (limit > 4096) limit = 4096;
        return (size_t)limit;
#endif
    }

    bool availableSpace(uint64_t &bytes) const
    {
#ifdef _WIN32
        ULARGE_INTEGER free_bytes{};
        if (!GetDiskFreeSpaceExW(apiPath(fs::u8path(currentPath())).c_str(),
                                 &free_bytes, nullptr, nullptr))
            return false;
        bytes = free_bytes.QuadPart;
        return true;
#else
        struct statvfs info{};
        if (_fd < 0 || fstatvfs(_fd, &info) != 0) return false;
        const uint64_t block = info.f_frsize ? info.f_frsize : info.f_bsize;
        if (!block) return false;
        bytes = (uint64_t)info.f_bavail > UINT64_MAX / block
                    ? UINT64_MAX : (uint64_t)info.f_bavail * block;
        return true;
#endif
    }

#ifdef _WIN32
    HANDLE handle() const { return _handle; }
#else
    int fd() const { return _fd; }
#endif

private:
    void close()
    {
#ifdef _WIN32
        if (_handle != INVALID_HANDLE_VALUE) CloseHandle(_handle);
        _handle = INVALID_HANDLE_VALUE;
#else
        if (_fd >= 0) ::close(_fd);
        _fd = -1;
#endif
        _path.clear();
    }

#ifdef _WIN32
    HANDLE _handle = INVALID_HANDLE_VALUE;
#else
    int _fd = -1;
#endif
    std::string _path;
};

bool listLocal(const std::string &path, FtListing &out, std::string &error)
{
    out = FtListing{};
    error.clear();
    try {
        if (path.empty()) {
#ifdef _WIN32
            DWORD mask = GetLogicalDrives();
            if (!mask) {
                error = "无法枚举磁盘";
                return false;
            }
            for (int i = 0; i < 26; ++i) {
                if (!(mask & (1u << i))) continue;
                FtEntry e;
                e.name.push_back((char)('A' + i));
                e.name += ':';
                e.directory = true;
                out.entries.push_back(e);
            }
#else
            /* POSIX has only one root.  Return the real root directory listing
             * rather than manufacturing an entry named "/", which would not
             * be a valid single path component on the wire. */
            return listLocal("/", out, error);
#endif
            return true;
        }
#ifndef _WIN32
        if (!validPathText(path, false)) {
            error = "路径无效";
            return false;
        }
        DirLock dir;
        FtStatus ns = dir.open(path, true);
        if (ns != FT_OK) {
            error = ns == FT_ERR_NOT_FOUND ? "目录不存在" :
                    ns == FT_ERR_PATH ? "目录路径无效" : "无法读取目录";
            return false;
        }
        out.path = dir.path();
        if (!validPathText(out.path, false)) {
            error = "目录路径过长";
            out = FtListing{};
            return false;
        }
        const int dupfd = ::dup(dir.fd());
        if (dupfd < 0) {
            error = "无法读取目录";
            return false;
        }
        DIR *stream = ::fdopendir(dupfd);
        if (!stream) {
            ::close(dupfd);
            error = "无法读取目录";
            return false;
        }
        bool read_error = false;
        for (;;) {
            errno = 0;
            struct dirent *ent = ::readdir(stream);
            if (!ent) {
                if (errno) read_error = true;
                break;
            }
            const char *raw = ent->d_name;
            if (!raw || !*raw ||
                (raw[0] == '.' && (raw[1] == '\0' ||
                                   (raw[1] == '.' && raw[2] == '\0'))))
                continue;
            struct stat info{};
            if (fstatat(dir.fd(), raw, &info, AT_SYMLINK_NOFOLLOW) != 0)
                continue;
            if (S_ISLNK(info.st_mode)) continue;
            FtEntry e;
            e.name = raw;
            if (!validWireName(e.name)) continue;
            e.hidden = raw[0] == '.';
            e.mtime = info.st_mtime > 0
                          ? std::min<uint64_t>((uint64_t)info.st_mtime,
                                               FILEXFER_MAX_MTIME)
                          : 0;
            if (S_ISDIR(info.st_mode)) {
                e.directory = true;
                e.size = 0;
            } else if (S_ISREG(info.st_mode)) {
                if (info.st_size < 0) continue;
                e.directory = false;
                e.size = (uint64_t)info.st_size;
            } else {
                continue;
            }
            if (out.entries.size() >= FILEXFER_MAX_ENTRIES) {
                out.truncated = true;
                break;
            }
            out.entries.push_back(std::move(e));
        }
        ::closedir(stream);
        if (read_error) {
            error = "目录读取中断";
            out = FtListing{};
            return false;
        }
        std::sort(out.entries.begin(), out.entries.end(),
                  [](const FtEntry &a, const FtEntry &b) {
                      if (a.directory != b.directory) return a.directory;
                      return a.name < b.name;
                  });
        return true;
#else
        if (!validPathText(path, false)) {
            error = "路径无效";
            return false;
        }
        DirLock dir;
        FtStatus ns = dir.open(path, true);
        if (ns != FT_OK) {
            error = ns == FT_ERR_NOT_FOUND ? "目录不存在" :
                    ns == FT_ERR_PATH ? "目录路径无效" : "无法读取目录";
            return false;
        }
        out.path = dir.currentPath();
        if (!validPathText(out.path, false)) {
            error = "目录路径过长";
            out = FtListing{};
            return false;
        }
        /* FindFirstFileW with the \\?\ path reaches directories over MAX_PATH;
         * std::filesystem's directory_entry status calls do not. */
        std::wstring pattern = apiPath(fs::u8path(out.path));
        if (pattern.empty() || pattern.back() != L'\\') pattern += L'\\';
        pattern += L'*';
        WIN32_FIND_DATAW data{};
        HANDLE find = FindFirstFileW(pattern.c_str(), &data);
        if (find == INVALID_HANDLE_VALUE) {
            const DWORD e = GetLastError();
            if (e == ERROR_FILE_NOT_FOUND) return true; /* empty directory */
            error = "无法读取目录";
            return false;
        }
        DWORD find_error = ERROR_NO_MORE_FILES;
        for (;;) {
            const std::wstring wide_name = data.cFileName;
            if (wide_name != L"." && wide_name != L"..") {
                const std::string name = utf8FromWide(wide_name);
                if (!name.empty() && validWireName(name) &&
                    !(data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                    const bool is_directory =
                        (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                    if (is_directory ||
                        !(data.dwFileAttributes & FILE_ATTRIBUTE_DEVICE)) {
                        if (out.entries.size() >= FILEXFER_MAX_ENTRIES) {
                            out.truncated = true;
                            break;
                        }
                        FtEntry e;
                        e.name = name;
                        e.directory = is_directory;
                        e.size = is_directory
                                     ? 0
                                     : ((uint64_t)data.nFileSizeHigh << 32) |
                                           data.nFileSizeLow;
                        /* Explorer hides by attribute, not by a leading dot. */
                        e.hidden =
                            (data.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) != 0;
                        /* FILETIME is 100 ns ticks since 1601-01-01. */
                        const uint64_t ticks =
                            ((uint64_t)data.ftLastWriteTime.dwHighDateTime << 32) |
                            data.ftLastWriteTime.dwLowDateTime;
                        const uint64_t epoch = 116444736000000000ull;
                        e.mtime = ticks > epoch
                                      ? std::min<uint64_t>(
                                            (ticks - epoch) / 10000000ull,
                                            FILEXFER_MAX_MTIME)
                                      : 0;
                        out.entries.push_back(std::move(e));
                    }
                }
            }
            if (!FindNextFileW(find, &data)) {
                find_error = GetLastError();
                break;
            }
        }
        FindClose(find);
        if (find_error != ERROR_NO_MORE_FILES) {
            error = "目录读取中断";
            out = FtListing{};
            return false;
        }
        std::sort(out.entries.begin(), out.entries.end(),
                  [](const FtEntry &a, const FtEntry &b) {
                      if (a.directory != b.directory) return a.directory;
                      return a.name < b.name;
                  });
        return true;
#endif
    } catch (...) {
        out = FtListing{};
        error = "目录读取失败";
        return false;
    }
}

/* Create one directory component relative to a pinned parent. No path is
 * re-resolved, so a concurrently swapped ancestor cannot redirect the create.
 * Fails (never replaces) when the name already exists. */
FtStatus createDirectoryLocked(DirLock &dir, const std::string &name,
                               std::string &path)
{
    path.clear();
    if (!validLocalName(name)) return FT_ERR_PATH;
    /* Check before creating: a directory that exists on disk but whose path
     * cannot be represented on the wire would have no usable RESULT text. */
    if (dir.path().size() + 1 + name.size() > FILEXFER_MAX_PATH_BYTES)
        return FT_ERR_LIMIT;
#ifdef _WIN32
    const std::wstring wide = wideFromUtf8(name);
    if (wide.empty()) return FT_ERR_PATH;
    HANDLE child = INVALID_HANDLE_VALUE;
    FtStatus status = ntOpenRelative(
        dir.handle(), wide, true,
        FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        FILE_CREATE, child);
    if (status != FT_OK) return status;
    CloseHandle(child);
#else
    if (::mkdirat(dir.fd(), name.c_str(), 0777) != 0) {
        const int e = errno;
        return e == EEXIST ? FT_ERR_EXISTS :
               (e == ENOENT || e == ENOTDIR) ? FT_ERR_NOT_FOUND :
               e == ENAMETOOLONG ? FT_ERR_LIMIT :
               e == ENOSPC ? FT_ERR_DISK : FT_ERR_IO;
    }
#endif
    try {
        path = (fs::u8path(dir.path()) / fs::u8path(name)).u8string();
    } catch (...) {
        return FT_ERR_IO;
    }
    if (!validPathText(path, false)) {
        path.clear();
        return FT_ERR_LIMIT;
    }
    return FT_OK;
}

#ifdef _WIN32
std::string environmentUtf8(const wchar_t *name)
{
    const DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
    if (n <= 1) return std::string();
    std::wstring wide((size_t)n, L'\0');
    const DWORD got = GetEnvironmentVariableW(name, &wide[0], n);
    if (!got || got >= n) return std::string();
    wide.resize(got);
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int)wide.size(),
                                         nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return std::string();
    std::string out((size_t)bytes, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int)wide.size(), &out[0], bytes,
                        nullptr, nullptr);
    return out;
}
#endif

bool localHome(std::string &path, std::string &error)
{
    path.clear();
    error.clear();
    try {
        std::string home;
#ifdef _WIN32
        home = environmentUtf8(L"USERPROFILE");
        if (home.empty()) {
            const std::string drive = environmentUtf8(L"HOMEDRIVE");
            const std::string tail = environmentUtf8(L"HOMEPATH");
            if (!drive.empty() && !tail.empty()) home = drive + tail;
        }
#else
        const char *env = std::getenv("HOME");
        if (env) home = env;
        if (home.empty()) {
            passwd entry{};
            passwd *result = nullptr;
            std::vector<char> buffer(4096);
            if (getpwuid_r(getuid(), &entry, buffer.data(), buffer.size(),
                           &result) == 0 &&
                result && result->pw_dir)
                home = result->pw_dir;
        }
#endif
        if (home.empty() || !validUtf8(home)) {
            error = "无法确定用户主目录";
            return false;
        }
#ifdef _WIN32
        std::wstring full;
        if (!winFullPath(home, full)) {
            error = "用户主目录不可用";
            return false;
        }
        const DWORD attributes =
            GetFileAttributesW(apiPath(fs::path(full.c_str())).c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
            error = "用户主目录不可用";
            return false;
        }
        path = utf8FromWide(full);
#else
        std::error_code ec;
        fs::path p = fs::canonical(fs::u8path(home), ec);
        if (ec || !fs::is_directory(p, ec) || ec) {
            error = "用户主目录不可用";
            return false;
        }
        path = p.u8string();
#endif
        if (!validPathText(path, false)) {
            path.clear();
            error = "用户主目录路径过长";
            return false;
        }
        return true;
    } catch (...) {
        error = "无法确定用户主目录";
        path.clear();
        return false;
    }
}

/* ── read-only source and exclusive same-directory staging ─────────────── */

class FileReader {
public:
    FileReader() = default;
    ~FileReader() { close(); }
    FileReader(const FileReader &) = delete;
    FileReader &operator=(const FileReader &) = delete;

    FtStatus open(const std::string &text, bool require_absolute)
    {
        close();
        if (text.empty() || !validUtf8(text) ||
            text.find('\0') != std::string::npos)
            return FT_ERR_PATH;
        if (require_absolute && !validPathText(text, false)) return FT_ERR_PATH;
        if (require_absolute && !absolutePathText(fs::u8path(text)))
            return FT_ERR_PATH;
#ifdef _WIN32
        std::wstring full;
        if (!winFullPath(text, full)) return FT_ERR_PATH;
        while (full.size() > 1 && full.back() == L'\\') full.pop_back();
        const size_t slash = full.find_last_of(L'\\');
        if (slash == std::wstring::npos || slash + 1 >= full.size())
            return FT_ERR_PATH;
        std::wstring parent_w = full.substr(0, slash);
        if (!parent_w.empty() && parent_w.back() != L'\\') parent_w += L'\\';
        const std::string name = utf8FromWide(full.substr(slash + 1));
        if (name.empty() || name == "." || name == ".." ||
            !validLocalName(name))
            return FT_ERR_PATH;
        DirLock parent;
        FtStatus status = parent.open(utf8FromWide(parent_w), true);
        if (status != FT_OK) return status;
#else
        std::error_code ec;
        fs::path p = fs::u8path(text);
        if (!p.is_absolute()) p = fs::absolute(p, ec);
        if (ec || !p.is_absolute()) return FT_ERR_PATH;
        const std::string name = p.filename().u8string();
        if (name.empty() || name == "." || name == ".." ||
            !validLocalName(name))
            return FT_ERR_PATH;
        DirLock parent;
        FtStatus status = parent.open(p.parent_path().u8string(), true);
        if (status != FT_OK) return status;
#endif
#ifdef _WIN32
        const std::string full_path =
            (fs::u8path(parent.currentPath()) / fs::u8path(name)).u8string();
        _handle = CreateFileW(apiPath(fs::u8path(full_path)).c_str(), GENERIC_READ,
                              FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_FLAG_OPEN_REPARSE_POINT |
                                  FILE_FLAG_SEQUENTIAL_SCAN |
                                  FILE_FLAG_BACKUP_SEMANTICS,
                              nullptr);
        if (_handle == INVALID_HANDLE_VALUE) {
            const DWORD e = GetLastError();
            return (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND)
                       ? FT_ERR_NOT_FOUND : FT_ERR_IO;
        }
        if (!ntHandleInDirectory(_handle, parent.handle())) {
            close();
            return FT_ERR_PATH;
        }
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(_handle, &info) ||
            (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            close();
            return FT_ERR_PATH;
        }
        uint64_t n = ((uint64_t)info.nFileSizeHigh << 32) | info.nFileSizeLow;
#else
        int flags = O_RDONLY;
#ifdef O_NOFOLLOW
        flags |= O_NOFOLLOW;
#endif
#ifdef O_CLOEXEC
        flags |= O_CLOEXEC;
#endif
#ifdef O_NONBLOCK
        /* A FIFO at the requested path must not block the service thread
         * waiting for a writer; the S_ISREG check below rejects it. */
        flags |= O_NONBLOCK;
#endif
#ifdef O_NOCTTY
        flags |= O_NOCTTY;
#endif
        _fd = ::openat(parent.fd(), name.c_str(), flags);
        if (_fd < 0) {
            const int e = errno;
            return e == ENOENT ? FT_ERR_NOT_FOUND :
                   (e == ELOOP || e == ENOTDIR) ? FT_ERR_PATH : FT_ERR_IO;
        }
        struct stat info{};
        if (fstat(_fd, &info) != 0 || !S_ISREG(info.st_mode)) {
            close();
            return FT_ERR_PATH;
        }
        if (info.st_size < 0) {
            close();
            return FT_ERR_IO;
        }
        uint64_t n = (uint64_t)info.st_size;
#endif
        if (n > FILEXFER_MAX_FILE_BYTES) {
            close();
            return FT_ERR_LIMIT;
        }
        _path = fs::u8path(parent.currentPath()) / fs::u8path(name);
        _size = n;
        return FT_OK;
    }

    bool read(void *buf, size_t want, size_t &got)
    {
        got = 0;
        if (want > (size_t)INT_MAX) return false;
#ifdef _WIN32
        DWORD n = 0;
        if (_handle == INVALID_HANDLE_VALUE ||
            !ReadFile(_handle, buf, (DWORD)want, &n, nullptr))
            return false;
        got = (size_t)n;
        return true;
#else
        for (;;) {
            if (_fd < 0) return false;
            ssize_t n = ::read(_fd, buf, want);
            if (n < 0 && errno == EINTR) continue;
            if (n < 0) return false;
            got = (size_t)n;
            return true;
        }
#endif
    }

    bool sizeStill(uint64_t expected) const
    {
#ifdef _WIN32
        LARGE_INTEGER n{};
        return _handle != INVALID_HANDLE_VALUE &&
               GetFileSizeEx(_handle, &n) && n.QuadPart >= 0 &&
               (uint64_t)n.QuadPart == expected;
#else
        struct stat st{};
        return _fd >= 0 && fstat(_fd, &st) == 0 && st.st_size >= 0 &&
               (uint64_t)st.st_size == expected;
#endif
    }

    uint64_t size() const { return _size; }
    std::string path() const { return _path.u8string(); }
    std::string name() const { return _path.filename().u8string(); }

    void close()
    {
#ifdef _WIN32
        if (_handle != INVALID_HANDLE_VALUE) CloseHandle(_handle);
        _handle = INVALID_HANDLE_VALUE;
#else
        if (_fd >= 0) ::close(_fd);
        _fd = -1;
#endif
    }

private:
    fs::path _path;
    uint64_t _size = 0;
#ifdef _WIN32
    HANDLE _handle = INVALID_HANDLE_VALUE;
#else
    int _fd = -1;
#endif
};

/* ── UTF-8 component limits for no-overwrite collision names ────────────── */

size_t utf8SequenceLength(unsigned char lead)
{
    if (lead < 0x80) return 1;
    if (lead < 0xE0) return 2;
    if (lead < 0xF0) return 3;
    return 4;
}

/* A POSIX component limit is measured in bytes; Windows counts UTF-16 code
 * units, so a 4-byte UTF-8 sequence consumes two units there. */
size_t codepointUnits(const char *sequence, size_t length)
{
    (void)sequence;
#ifdef _WIN32
    return length == 4 ? 2u : 1u;
#else
    return length;
#endif
}

size_t nameUnits(const std::string &name)
{
    size_t units = 0;
    for (size_t i = 0; i < name.size();) {
        size_t n = utf8SequenceLength((unsigned char)name[i]);
        if (i + n > name.size()) n = 1;
        units += codepointUnits(name.data() + i, n);
        i += n;
    }
    return units;
}

std::string truncateName(const std::string &name, size_t max_units)
{
    std::string out;
    size_t units = 0;
    for (size_t i = 0; i < name.size();) {
        const size_t n = utf8SequenceLength((unsigned char)name[i]);
        if (i + n > name.size()) break;
        const size_t add = codepointUnits(name.data() + i, n);
        if (units + add > max_units) break;
        out.append(name, i, n);
        units += add;
        i += n;
    }
    return out;
}

/* Build the index-th no-overwrite candidate.  The suffix and extension are
 * preserved, and the stem is shortened on a full UTF-8 character boundary so
 * the result always fits the target filesystem's component limit.  An empty
 * return means even the suffix cannot fit. */
std::string uniqueName(const std::string &name, unsigned index,
                       size_t max_units)
{
    if (!index) return name;
    const std::string suffix = " (" + std::to_string(index) + ")";
    const size_t suffix_units = nameUnits(suffix);
    if (suffix_units > max_units) return std::string();
    size_t available = max_units - suffix_units;
    const size_t dot = name.rfind('.');
    std::string stem = (dot == std::string::npos || dot == 0)
                           ? name : name.substr(0, dot);
    std::string ext = (dot == std::string::npos || dot == 0)
                          ? std::string() : name.substr(dot);
    if (nameUnits(ext) > available) {
        ext = truncateName(ext, available);
        available -= nameUnits(ext);
        stem.clear();
    } else {
        available -= nameUnits(ext);
    }
    stem = truncateName(stem, available);
#ifdef _WIN32
    /* Truncation can leave a trailing dot/space, which Win32 does not treat as
     * part of the name.  The original name has already passed
     * validLocalName(); only the shortened form needs this cleanup. */
    while (!ext.empty() && (ext.back() == '.' || ext.back() == ' '))
        ext.pop_back();
    while (!stem.empty() && (stem.back() == '.' || stem.back() == ' '))
        stem.pop_back();
#endif
    return stem + suffix + ext;
}

class FileWriter {
public:
    FileWriter() = default;
    ~FileWriter()
    {
        /* Dispose of the temp file through the open handle first (Windows),
         * then release the file handle and the directory handle. */
        removeTemp();
        close();
    }
    FileWriter(const FileWriter &) = delete;
    FileWriter &operator=(const FileWriter &) = delete;

    FtStatus create(const std::string &directory, const std::string &name,
                    bool require_absolute, uint64_t expected_size)
    {
        removeTemp();
        close();
        _final.clear();
        _committed = false;
        _name.clear();
        _temp_id_valid = false;
        if (!validLocalName(name) || expected_size > FILEXFER_MAX_FILE_BYTES)
            return FT_ERR_PATH;
        FtStatus status = _dir_lock.open(directory, require_absolute);
        if (status != FT_OK) return status;
        _max_name_units = _dir_lock.nameLimit();
        if (nameUnits(name) > _max_name_units) return FT_ERR_LIMIT;
        uint64_t available = 0;
        if (!_dir_lock.availableSpace(available)) return FT_ERR_IO;
        if (expected_size > available) return FT_ERR_DISK;

        _name = name;
        for (unsigned attempt = 0; attempt < 16; ++attempt) {
            uint8_t random[12] = {};
            if (!mivaraRandomBytes(random, sizeof(random))) return FT_ERR_IO;
            const std::string temp_name = ".mivara-" +
                mivaraHexEncode(random, sizeof(random)) + ".part";
#ifdef _WIN32
            const std::string full =
                (fs::u8path(_dir_lock.currentPath()) /
                 fs::u8path(temp_name)).u8string();
            _handle = CreateFileW(apiPath(fs::u8path(full)).c_str(),
                                  GENERIC_WRITE | DELETE, 0, nullptr, CREATE_NEW,
                                  FILE_ATTRIBUTE_NORMAL |
                                      FILE_FLAG_OPEN_REPARSE_POINT |
                                      FILE_FLAG_SEQUENTIAL_SCAN,
                                  nullptr);
            if (_handle != INVALID_HANDLE_VALUE) {
                if (!ntHandleIsPlain(_handle, false) ||
                    !ntHandleInDirectory(_handle, _dir_lock.handle())) {
                    FILE_DISPOSITION_INFO info{};
                    info.DeleteFile = TRUE;
                    SetFileInformationByHandle(_handle, FileDispositionInfo,
                                               &info, sizeof(info));
                    close();
                    _temp_name.clear();
                    return FT_ERR_IO;
                }
                BY_HANDLE_FILE_INFORMATION file_info{};
                if (!GetFileInformationByHandle(_handle, &file_info)) {
                    FILE_DISPOSITION_INFO info{};
                    info.DeleteFile = TRUE;
                    SetFileInformationByHandle(_handle, FileDispositionInfo,
                                               &info, sizeof(info));
                    close();
                    _temp_name.clear();
                    return FT_ERR_IO;
                }
                _temp_dev = file_info.dwVolumeSerialNumber;
                _temp_ino = ((uint64_t)file_info.nFileIndexHigh << 32) |
                            file_info.nFileIndexLow;
                _temp_id_valid = true;
                _temp_name = temp_name;
                return FT_OK;
            }
            const DWORD e = GetLastError();
            if (e == ERROR_FILE_EXISTS || e == ERROR_ALREADY_EXISTS) continue;
            return (e == ERROR_DISK_FULL || e == ERROR_HANDLE_DISK_FULL)
                       ? FT_ERR_DISK : FT_ERR_IO;
#else
            int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_NOFOLLOW
            flags |= O_NOFOLLOW;
#endif
#ifdef O_CLOEXEC
            flags |= O_CLOEXEC;
#endif
            _fd = ::openat(_dir_lock.fd(), temp_name.c_str(), flags, 0600);
            if (_fd >= 0) {
                struct stat st{};
                if (fstat(_fd, &st) != 0 || !S_ISREG(st.st_mode)) {
                    ::close(_fd);
                    _fd = -1;
                    _temp_name.clear();
                    return FT_ERR_IO;
                }
                _temp_dev = (uint64_t)st.st_dev;
                _temp_ino = (uint64_t)st.st_ino;
                _temp_id_valid = true;
                _temp_name = temp_name;
                return FT_OK;
            }
            const int e = errno;
            if (e == EEXIST) continue;
            return (e == ENOSPC || e == EDQUOT) ? FT_ERR_DISK : FT_ERR_IO;
#endif
        }
        return FT_ERR_IO;
    }

    bool write(const void *data, size_t size)
    {
        if (!size) return true;
#ifdef _WIN32
        if (_handle == INVALID_HANDLE_VALUE || size > MAXDWORD) return false;
        DWORD done = 0;
        return WriteFile(_handle, data, (DWORD)size, &done, nullptr) &&
               done == size;
#else
        const uint8_t *p = static_cast<const uint8_t *>(data);
        size_t off = 0;
        while (off < size) {
            if (_fd < 0) return false;
            ssize_t n = ::write(_fd, p + off, size - off);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return false;
            off += (size_t)n;
        }
        return true;
#endif
    }

    FtStatus finish()
    {
        if (_temp_name.empty()) return FT_ERR_IO;
#ifdef _WIN32
        if (_handle == INVALID_HANDLE_VALUE || !FlushFileBuffers(_handle)) {
            close();
            return FT_ERR_IO;
        }
        /* Keep the handle open: commit renames the file relative to it. */
        return FT_OK;
#else
        if (_fd < 0) return FT_ERR_IO;
        if (fsync(_fd) != 0) {
            close();
            return FT_ERR_IO;
        }
        if (::close(_fd) != 0) {
            _fd = -1;
            return FT_ERR_IO;
        }
        _fd = -1;
        return FT_OK;
#endif
    }

    FtStatus commit()
    {
        if (_temp_name.empty() || _committed) return FT_ERR_IO;
#ifdef _WIN32
        if (_handle == INVALID_HANDLE_VALUE) return FT_ERR_IO;
        NtSetInformationFileFn set_information = ntSetInformationFile();
        if (!set_information) return FT_ERR_IO;
        struct RenameInfo {
            BOOLEAN ReplaceIfExists;
            HANDLE RootDirectory;
            ULONG FileNameLength;
            WCHAR FileName[1];
        };
#else
        if (_fd >= 0) return FT_ERR_IO;
#endif
        for (unsigned index = 0; index < 10000; ++index) {
            const std::string candidate_name =
                uniqueName(_name, index, _max_name_units);
            /* Once the suffix alone no longer fits, larger indices cannot fit
             * either; do not spin through the remaining attempts. */
            if (candidate_name.empty()) return FT_ERR_LIMIT;
            if (!validLocalName(candidate_name)) continue;
            fs::path candidate =
                fs::u8path(_dir_lock.currentPath()) / fs::u8path(candidate_name);
#ifdef _WIN32
            const std::wstring wide_name = wideFromUtf8(candidate_name);
            const size_t name_bytes = wide_name.size() * sizeof(wchar_t);
            std::vector<uint8_t> buffer(
                offsetof(RenameInfo, FileName) + name_bytes);
            RenameInfo *info = reinterpret_cast<RenameInfo *>(buffer.data());
            info->ReplaceIfExists = FALSE;
            info->RootDirectory = _dir_lock.handle();
            info->FileNameLength = (ULONG)name_bytes;
            std::memcpy(info->FileName, wide_name.c_str(), name_bytes);
            IO_STATUS_BLOCK iosb{};
            const NTSTATUS rename_status = set_information(
                _handle, &iosb, info, (ULONG)buffer.size(),
                10 /* FileRenameInformation */);
            if (rename_status >= 0) {
                close();
                _final = candidate;
                _temp_name.clear();
                _committed = true;
                return FT_OK;
            }
            const ULONG raw = (ULONG)rename_status;
            if (raw == 0xC0000035UL /* STATUS_OBJECT_NAME_COLLISION */ ||
                raw == 0xC00000BAUL /* STATUS_FILE_IS_A_DIRECTORY */ ||
                raw == 0xC0000103UL /* STATUS_NOT_A_DIRECTORY */)
                continue;
            const FtStatus mapped = statusFromNt(rename_status);
            return mapped == FT_ERR_NOT_FOUND ? FT_ERR_IO : mapped;
#else
            /* The temporary name lives in a user-writable directory; make sure
             * the entry still refers to the inode this writer created before
             * publishing it.  This narrows (not eliminates) a same-directory
             * name-swap race. */
            struct stat temp_st{};
            if (!_temp_id_valid ||
                fstatat(_dir_lock.fd(), _temp_name.c_str(), &temp_st,
                        AT_SYMLINK_NOFOLLOW) != 0 ||
                !S_ISREG(temp_st.st_mode) ||
                (uint64_t)temp_st.st_dev != _temp_dev ||
                (uint64_t)temp_st.st_ino != _temp_ino)
                return FT_ERR_IO;
            /* A local writer in the same directory can still replace the temp
             * name between the check above and the publish call.  Never report
             * success unless the published name denotes our inode. */
            auto published_is_ours = [&]() {
                struct stat st{};
                return fstatat(_dir_lock.fd(), candidate_name.c_str(), &st,
                               AT_SYMLINK_NOFOLLOW) == 0 &&
                       S_ISREG(st.st_mode) &&
                       (uint64_t)st.st_dev == _temp_dev &&
                       (uint64_t)st.st_ino == _temp_ino;
            };
#ifdef __linux__
#  ifdef SYS_renameat2
            if (::syscall(SYS_renameat2, _dir_lock.fd(),
                          _temp_name.c_str(), _dir_lock.fd(),
                          candidate_name.c_str(),
                          (unsigned)RENAME_NOREPLACE) == 0) {
                if (!published_is_ours()) {
                    ::unlinkat(_dir_lock.fd(), candidate_name.c_str(), 0);
                    _temp_name.clear();
                    _temp_id_valid = false;
                    return FT_ERR_IO;
                }
                _final = candidate;
                _temp_name.clear();
                _temp_id_valid = false;
                _committed = true;
                (void)::fsync(_dir_lock.fd());
                return FT_OK;
            }
            {
                const int e = errno;
                if (e == EEXIST || e == EISDIR || e == ENOTEMPTY) continue;
                if (e != ENOSYS && e != EINVAL && e != EOPNOTSUPP &&
                    e != ENOTSUP)
                    return (e == ENOSPC || e == EDQUOT) ? FT_ERR_DISK :
                           (e == ENAMETOOLONG) ? FT_ERR_LIMIT : FT_ERR_IO;
            }
#  endif
#elif defined(__APPLE__)
            /* RENAME_EXCL is atomic no-overwrite and works on filesystems
             * without hard links. */
            if (::renameatx_np(_dir_lock.fd(), _temp_name.c_str(),
                               _dir_lock.fd(), candidate_name.c_str(),
                               RENAME_EXCL) == 0) {
                if (!published_is_ours()) {
                    ::unlinkat(_dir_lock.fd(), candidate_name.c_str(), 0);
                    _temp_name.clear();
                    _temp_id_valid = false;
                    return FT_ERR_IO;
                }
                _final = candidate;
                _temp_name.clear();
                _temp_id_valid = false;
                _committed = true;
                (void)::fsync(_dir_lock.fd());
                return FT_OK;
            }
            {
                const int e = errno;
                if (e == EEXIST || e == EISDIR || e == ENOTEMPTY) continue;
                if (e != ENOTSUP && e != EOPNOTSUPP && e != EINVAL &&
                    e != ENOSYS)
                    return (e == ENOSPC || e == EDQUOT) ? FT_ERR_DISK :
                           (e == ENAMETOOLONG) ? FT_ERR_LIMIT : FT_ERR_IO;
            }
#endif
            /* Fallback for kernels/filesystems without a no-replace rename.
             * linkat() also fails without hard-link support; never replace it
             * with a check-then-rename overwrite race. */
            if (::linkat(_dir_lock.fd(), _temp_name.c_str(), _dir_lock.fd(),
                         candidate_name.c_str(), 0) == 0) {
                if (::unlinkat(_dir_lock.fd(), _temp_name.c_str(), 0) == 0) {
                    if (!published_is_ours()) {
                        ::unlinkat(_dir_lock.fd(), candidate_name.c_str(), 0);
                        _temp_name.clear();
                        _temp_id_valid = false;
                        return FT_ERR_IO;
                    }
                    _final = candidate;
                    _temp_name.clear();
                    _temp_id_valid = false;
                    _committed = true;
                    (void)::fsync(_dir_lock.fd());
                    return FT_OK;
                }
                /* The final name is the only copy; roll it back rather than
                 * report failure while leaving an apparently valid file. */
                ::unlinkat(_dir_lock.fd(), candidate_name.c_str(), 0);
                return FT_ERR_IO;
            }
            if (errno == EEXIST) continue;
            return (errno == ENOSPC || errno == EDQUOT) ? FT_ERR_DISK :
                   (errno == ENAMETOOLONG ? FT_ERR_LIMIT : FT_ERR_IO);
#endif
        }
        return FT_ERR_EXISTS;
    }

    std::string finalPath() const { return _final.u8string(); }

private:
    void close()
    {
#ifdef _WIN32
        if (_handle != INVALID_HANDLE_VALUE) CloseHandle(_handle);
        _handle = INVALID_HANDLE_VALUE;
#else
        if (_fd >= 0) ::close(_fd);
        _fd = -1;
#endif
    }

    void removeTemp()
    {
        if (_temp_name.empty() || _committed) {
            _temp_name.clear();
            return;
        }
#ifdef _WIN32
        /* Prefer deleting the inode this writer still holds open: a renamed or
         * swapped entry under the same name is then never removed. */
        if (_handle != INVALID_HANDLE_VALUE) {
            FILE_DISPOSITION_INFO info{};
            info.DeleteFile = TRUE;
            SetFileInformationByHandle(_handle, FileDispositionInfo, &info,
                                       sizeof(info));
            close();
        } else {
            HANDLE temp = INVALID_HANDLE_VALUE;
            if (_dir_lock.handle() != INVALID_HANDLE_VALUE &&
                ntOpenRelative(_dir_lock.handle(),
                               wideFromUtf8(_temp_name), false,
                               DELETE | SYNCHRONIZE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE |
                                   FILE_SHARE_DELETE,
                               FILE_OPEN, temp) == FT_OK) {
                BY_HANDLE_FILE_INFORMATION file_info{};
                const bool ours =
                    _temp_id_valid &&
                    GetFileInformationByHandle(temp, &file_info) &&
                    file_info.dwVolumeSerialNumber == (DWORD)_temp_dev &&
                    (((uint64_t)file_info.nFileIndexHigh << 32) |
                     file_info.nFileIndexLow) == _temp_ino;
                if (ours) {
                    FILE_DISPOSITION_INFO disposition{};
                    disposition.DeleteFile = TRUE;
                    SetFileInformationByHandle(temp, FileDispositionInfo,
                                               &disposition,
                                               sizeof(disposition));
                }
                CloseHandle(temp);
            }
        }
#else
        if (_dir_lock.fd() >= 0)
            ::unlinkat(_dir_lock.fd(), _temp_name.c_str(), 0);
#endif
        _temp_name.clear();
        _temp_id_valid = false;
    }

    DirLock _dir_lock;
    std::string _temp_name;
    fs::path _final;
    std::string _name;
    size_t _max_name_units = 255;
    bool _committed = false;
    uint64_t _temp_dev = 0;
    uint64_t _temp_ino = 0;
    bool _temp_id_valid = false;
#ifdef _WIN32
    HANDLE _handle = INVALID_HANDLE_VALUE;
#else
    int _fd = -1;
#endif
};

/* ── payload formats ──────────────────────────────────────────────────────
 * LIST reply: status, flags(bit0=truncated), path, count, entries.
 * Entry: flags(u8: bit0=directory, bit1=hidden), name, size(u64),
 *        mtime(u64, unix seconds, 0 = unknown).
 * PUT_BEGIN: target directory (empty in a GET reply), one-component name,
 *            file size(u64).  GET/LIST carry one length-prefixed path.
 * MKDIR: parent directory + one-component name; RESULT returns the created
 *        absolute path.
 * INFO: one length-prefixed device name, sent by the service immediately
 *       after admission and before the request loop.
 * DATA: raw bytes.  END: exactly 32 SHA-256 bytes.
 * RESULT: status, byte count(u64), text (final path on successful PUT/MKDIR).
 * ABORT: status, short text.  Every string has a u32 byte length.
 */

std::vector<uint8_t> encodePath(const std::string &path)
{
    std::vector<uint8_t> p;
    appendString(p, path);
    return p;
}

bool decodePath(const std::vector<uint8_t> &p, std::string &path,
                bool allow_empty)
{
    Cursor c(p.data(), p.size());
    return c.string(path, FILEXFER_MAX_PATH_BYTES) && c.done() &&
           validPathText(path, allow_empty);
}

std::vector<uint8_t> encodePut(const std::string &dir, const std::string &name,
                               uint64_t size)
{
    std::vector<uint8_t> p;
    p.reserve(16 + dir.size() + name.size());
    appendString(p, dir);
    appendString(p, name);
    append64(p, size);
    return p;
}

bool decodePut(const std::vector<uint8_t> &p, std::string &dir,
               std::string &name, uint64_t &size)
{
    Cursor c(p.data(), p.size());
    return c.string(dir, FILEXFER_MAX_PATH_BYTES) &&
           c.string(name, FILEXFER_MAX_NAME_BYTES) && c.u64(size) && c.done() &&
           validPathText(dir, true) && validWireName(name) &&
           size <= FILEXFER_MAX_FILE_BYTES;
}

std::vector<uint8_t> encodeResult(FtStatus status, uint64_t bytes,
                                  const std::string &text)
{
    std::vector<uint8_t> p;
    append32(p, (uint32_t)status);
    append64(p, bytes);
    appendString(p, text);
    return p;
}

struct WireResult {
    FtStatus status = FT_ERR_PROTOCOL;
    uint64_t bytes = 0;
    std::string text;
};

bool decodeResult(const std::vector<uint8_t> &p, WireResult &result)
{
    uint32_t status = 0;
    Cursor c(p.data(), p.size());
    if (!c.u32(status) || !validStatus(status) || !c.u64(result.bytes) ||
        !c.string(result.text, FILEXFER_MAX_PATH_BYTES) || !c.done() ||
        !validUtf8(result.text) || result.text.find('\0') != std::string::npos)
        return false;
    result.status = (FtStatus)status;
    return true;
}

std::vector<uint8_t> encodeListing(const FtListing &listing, FtStatus status)
{
    std::vector<uint8_t> p;
    append32(p, (uint32_t)status);
    append32(p, listing.truncated ? 1u : 0u);
    appendString(p, status == FT_OK ? listing.path : std::string());
    append32(p, status == FT_OK ? (uint32_t)listing.entries.size() : 0u);
    if (status == FT_OK) {
        for (const FtEntry &e : listing.entries) {
            p.push_back((uint8_t)((e.directory ? 1u : 0u) |
                                  (e.hidden ? 2u : 0u)));
            appendString(p, e.name);
            append64(p, e.size);
            append64(p, e.mtime);
        }
    }
    return p;
}

bool decodeListing(const std::vector<uint8_t> &p, FtListing &listing,
                   FtStatus &status, std::string &message)
{
    uint32_t raw_status = 0, flags = 0, count = 0;
    Cursor c(p.data(), p.size());
    if (!c.u32(raw_status) || !validStatus(raw_status) || !c.u32(flags) ||
        (flags & ~1u) || !c.string(listing.path, FILEXFER_MAX_PATH_BYTES) ||
        !validPathText(listing.path, true) || !c.u32(count) ||
        count > FILEXFER_MAX_ENTRIES)
        return false;
    status = (FtStatus)raw_status;
    listing.truncated = (flags & 1u) != 0;
    if (status != FT_OK) {
        if (count != 0 || !listing.path.empty() || listing.truncated || !c.done())
            return false;
        message = ftStatusText(status);
        return true;
    }
    try {
        listing.entries.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            uint8_t flags = 0;
            FtEntry e;
            if (!c.u8(flags) || (flags & ~3u) ||
                !c.string(e.name, FILEXFER_MAX_NAME_BYTES) || !c.u64(e.size) ||
                !c.u64(e.mtime))
                return false;
            e.directory = (flags & 1u) != 0;
            e.hidden = (flags & 2u) != 0;
            if (e.directory && e.size != 0) return false;
            /* Do not reject an oversized file here: listings may legitimately
             * contain files over the transfer cap, and the GET must show the
             * typed FT_ERR_LIMIT rather than failing the whole directory. */
            if (e.mtime > FILEXFER_MAX_MTIME) return false;
            const bool drive_root = listing.path.empty() && e.directory &&
                                    driveRootName(e.name);
            if (drive_root && (e.hidden || e.mtime != 0)) return false;
            if (!validWireName(e.name) && !drive_root) return false;
            listing.entries.push_back(std::move(e));
        }
    } catch (...) {
        return false;
    }
    if (!c.done()) return false;
    if (listing.path.empty()) {
        /* Empty is reserved for synthetic Windows drive roots.  Drive names
         * are "C:" (no separator); ftPathJoin expands them to "C:\\". */
        for (const FtEntry &e : listing.entries)
            if (!e.directory || !driveRootName(e.name)) return false;
    }
    return true;
}

IoResult sendResult(Sock s, const std::atomic<bool> &running, FtStatus status,
                    uint64_t bytes = 0, const std::string &text = std::string())
{
    return sendFrame(s, FTF_RESULT, encodeResult(status, bytes, text), running);
}

IoResult sendAbort(Sock s, const std::atomic<bool> &running, FtStatus status,
                   const std::string &text)
{
    std::vector<uint8_t> p;
    append32(p, (uint32_t)status);
    appendString(p, text);
    return sendFrame(s, FTF_ABORT, p, running, 3000);
}

bool decodeAbort(const std::vector<uint8_t> &p, FtStatus &status)
{
    uint32_t raw = 0;
    std::string text;
    Cursor c(p.data(), p.size());
    if (!c.u32(raw) || !validStatus(raw) ||
        !c.string(text, FILEXFER_MAX_PATH_BYTES) || !c.done() ||
        !validUtf8(text) || text.find('\0') != std::string::npos)
        return false;
    status = (FtStatus)raw;
    return true;
}

std::vector<uint8_t> encodeInfo(const std::string &device)
{
    std::vector<uint8_t> p;
    appendString(p, device);
    return p;
}

bool decodeInfo(const std::vector<uint8_t> &p, std::string &device)
{
    Cursor c(p.data(), p.size());
    if (!c.string(device, 255) || !c.done() || device.empty() ||
        !validUtf8(device) || device.find('\0') != std::string::npos)
        return false;
    return true;
}

std::vector<uint8_t> encodeMkdir(const std::string &dir,
                                 const std::string &name)
{
    std::vector<uint8_t> p;
    p.reserve(8 + dir.size() + name.size());
    appendString(p, dir);
    appendString(p, name);
    return p;
}

bool decodeMkdir(const std::vector<uint8_t> &p, std::string &dir,
                 std::string &name)
{
    Cursor c(p.data(), p.size());
    return c.string(dir, FILEXFER_MAX_PATH_BYTES) &&
           c.string(name, FILEXFER_MAX_NAME_BYTES) && c.done() &&
           validPathText(dir, false) && validWireName(name);
}

/* ── receiver service ──────────────────────────────────────────────────── */

void (*g_stat_sink)(const char *) = nullptr;
std::mutex g_stat_mutex;

void fileStat(const std::string &state)
{
    /* Every caller supplies fixed keys and numeric/enum values only.  In
     * particular no device-supplied path or message is ever copied to STAT. */
    std::lock_guard<std::mutex> lock(g_stat_mutex);
    void (*sink)(const char *) = g_stat_sink;
    if (!sink) return;
    const std::string line = "event=files|" + state;
    sink(line.c_str());
}

std::atomic<int> g_service_state{0};
std::atomic<int> g_service_stage{0};
std::atomic<int> g_service_error{0};

void reportStartup(int state, int stage, int error)
{
    g_service_error.store(error, std::memory_order_relaxed);
    g_service_stage.store(stage, std::memory_order_relaxed);
    g_service_state.store(state, std::memory_order_release);
}

bool serviceEnabled(const FtTargetAuth *auth)
{
    return auth && auth->enabled.load(std::memory_order_acquire) != 0;
}

IoResult sendFileReply(Sock s, const std::atomic<bool> &running, FtStatus status)
{
    FilexferReply reply{};
    reply.magic = htonl(FILEXFER_MAGIC);
    reply.version = htonl(FILEXFER_VERSION);
    reply.status = htonl((uint32_t)status);
    return sendAll(s, &reply, sizeof(reply), running, 5000);
}

IoResult sendHsResponse(Sock s, const std::atomic<bool> &running, uint32_t status)
{
    HsResponseV2 response{};
    response.magic = htonl(TVHS_MAGIC);
    response.version = htonl(TVHS_VERSION);
    response.status = htonl(status);
    return sendAll(s, &response, sizeof(response), running, 5000);
}

std::string sanitizeDevice(const std::string &input)
{
    std::string out;
    out.reserve(std::min<size_t>(input.size(), 31));
    for (unsigned char c : input) {
        if (out.size() >= 31) break;
        out.push_back((c < 0x20 || c == 0x7f || c == '|') ? '?' : (char)c);
    }
    if (!validUtf8(out)) return "Mivara";
    while (out.size() > 31) out.pop_back();
    while (!out.empty() && !validUtf8(out)) out.pop_back();
    return out.empty() ? "Mivara" : out;
}

std::string localDeviceName()
{
    std::string name;
#ifdef _WIN32
    wchar_t wide[256] = {};
    DWORD n = (DWORD)(sizeof(wide) / sizeof(wide[0]) - 1);
    if (GetComputerNameW(wide, &n) && n) {
        int bytes = WideCharToMultiByte(CP_UTF8, 0, wide, (int)n, nullptr, 0,
                                        nullptr, nullptr);
        if (bytes > 0) {
            name.resize((size_t)bytes);
            WideCharToMultiByte(CP_UTF8, 0, wide, (int)n, &name[0], bytes,
                                nullptr, nullptr);
        }
    }
#else
    char host[256] = {};
    if (gethostname(host, sizeof(host) - 1) == 0) name = host;
#endif
    return sanitizeDevice(name);
}

std::string deviceFromWire(const HsRequestExtV3 &ext)
{
    std::string device;
    for (size_t i = 0; i < sizeof(ext.device) && ext.device[i]; ++i)
        device.push_back(ext.device[i]);
    return sanitizeDevice(device);
}

class AuthMaterialGuard {
public:
    ~AuthMaterialGuard() { mivaraSecureZero(hash, sizeof(hash)); }
    uint8_t salt[16] = {};
    uint8_t hash[32] = {};
};

int remainingBudget(Clock::time_point start, int total_ms)
{
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - start).count();
    if (elapsed >= total_ms) return 0;
    return (int)(total_ms - elapsed);
}

IoResult recvHandshake(Sock s, void *data, size_t size,
                       const std::atomic<bool> &running,
                       Clock::time_point start, int stage_ms)
{
    int left = remainingBudget(start, 140000);
    if (!left) return IoResult::Timeout;
    return recvAll(s, data, size, running, std::min(stage_ms, left));
}

/* The single service slot is local to 8086.  Deliberately no
 * mivaraSessionClaim/Promote call: display/stitch leases are independent. */
bool authorizeServiceClient(Sock s, const std::string &peer,
                            const FilexferHello &hello,
                            const FtTargetAuth *auth,
                            const std::atomic<bool> &running)
{
    if (!serviceEnabled(auth)) {
        sendFileReply(s, running, FT_ERR_DISABLED);
        fileStat("state=disabled");
        return false;
    }
    if (!running.load(std::memory_order_acquire)) return false;

    MivaraAuthMode mode = MIVARA_AUTH_MANUAL;
    uint32_t iterations = 0;
    bool record_ok = false;
    AuthMaterialGuard material;
    if (auth && auth->policy) {
        if (!auth->policy(&mode, material.salt, material.hash, &iterations,
                          &record_ok, auth->ctx))
            mode = MIVARA_AUTH_MANUAL;
    }
    if (mode != MIVARA_AUTH_OPEN && mode != MIVARA_AUTH_PASSWORD &&
        mode != MIVARA_AUTH_MANUAL)
        mode = MIVARA_AUTH_MANUAL;

    if (mode == MIVARA_AUTH_PASSWORD &&
        (!record_ok ||
         !mivaraAuthRecordUsable(material.hash, material.salt, iterations,
                                 nullptr))) {
        sendHsResponse(s, running, HS_ERR_AUTH_CONFIG);
        fileStat("state=denied|reason=auth_config");
        return false;
    }

    if (mode == MIVARA_AUTH_OPEN) {
        if (!serviceEnabled(auth)) {
            sendFileReply(s, running, FT_ERR_DISABLED);
            return false;
        }
        if (sendFileReply(s, running, FT_OK) != IoResult::Ok) return false;
        fileStat("state=authorized|mode=open");
        return true;
    }

    const auto handshake_start = Clock::now();
    HsAuthStart challenge{};
    challenge.magic = htonl(TVHA_MAGIC);
    challenge.version = htonl(TVHA_VERSION);
    challenge.mode = htonl(mode == MIVARA_AUTH_PASSWORD ? AUTH_MODE_PASSWORD
                                                        : AUTH_MODE_MANUAL);
    challenge.kdf_iter = htonl(iterations);
    if (mode == MIVARA_AUTH_PASSWORD)
        std::memcpy(challenge.salt, material.salt, sizeof(challenge.salt));
    if (!mivaraRandomBytes(challenge.nonce, sizeof(challenge.nonce))) {
        sendHsResponse(s, running, HS_ERR_AUTH_CONFIG);
        return false;
    }
    if (sendAll(s, &challenge, sizeof(challenge), running, 5000) != IoResult::Ok)
        return false;

    HsRequestExtV3 ext{};
    if (recvHandshake(s, &ext, sizeof(ext), running, handshake_start, 10000) !=
        IoResult::Ok)
        return false;
    if (ntohl(ext.magic) != TVHX_MAGIC || ntohl(ext.version) != TVHA_VERSION ||
        ntohl(ext.flags) != 0 || ntohl(ext.reserved) != 0) {
        sendHsResponse(s, running, HS_ERR_VERSION);
        return false;
    }
    const std::string device = deviceFromWire(ext);

    if (mode == MIVARA_AUTH_PASSWORD) {
        if (mivaraAuthThrottleRetryAfter(peer) > 0) {
            sendHsResponse(s, running, HS_ERR_AUTH);
            fileStat("state=denied|reason=throttled");
            return false;
        }
        HsAuthProof proof{};
        if (recvHandshake(s, &proof, sizeof(proof), running, handshake_start,
                          125000) != IoResult::Ok)
            return false;
        if (ntohl(proof.magic) != TVHA_MAGIC ||
            ntohl(proof.version) != TVHA_VERSION) {
            mivaraAuthThrottleNoteAbuse(peer);
            sendHsResponse(s, running, HS_ERR_AUTH);
            return false;
        }
        uint8_t bound[sizeof(FilexferHello) + sizeof(HsRequestExtV3)];
        std::memcpy(bound, &hello, sizeof(hello));
        std::memcpy(bound + sizeof(hello), &ext, sizeof(ext));
        uint8_t expected[32] = {};
        mivaraAuthProof(material.hash, MIVARA_AUTH_PURPOSE_FILES,
                        challenge.nonce, bound, sizeof(bound), expected);
        const bool match = mivaraConstEq(expected, proof.proof, sizeof(expected));
        mivaraSecureZero(expected, sizeof(expected));
        if (!match) {
            mivaraAuthThrottleNoteFailure(peer);
            sendHsResponse(s, running, HS_ERR_AUTH);
            fileStat("state=denied|reason=password");
            return false;
        }
        mivaraAuthThrottleNoteSuccess(peer);
    } else {
        /* Missing approval machinery is not a native/open fallback. */
        FtApprovalResult verdict = FT_APPROVAL_FAILED;
        if (auth && auth->approve)
            verdict = auth->approve(peer.c_str(), device.c_str(), (intptr_t)s,
                                    auth->ctx);
        switch (verdict) {
        case FT_APPROVAL_ALLOW:
            break;
        case FT_APPROVAL_DENY:
            sendHsResponse(s, running, HS_ERR_DENIED);
            fileStat("state=denied|reason=manual");
            return false;
        case FT_APPROVAL_TIMEOUT:
            sendHsResponse(s, running, HS_ERR_AUTH_TIMEOUT);
            fileStat("state=denied|reason=timeout");
            return false;
        case FT_APPROVAL_CANCELLED:
            return false;
        case FT_APPROVAL_BUSY:
            sendHsResponse(s, running, HS_ERR_BUSY);
            fileStat("state=denied|reason=approval_busy");
            return false;
        case FT_APPROVAL_FAILED:
        default:
            sendHsResponse(s, running, HS_ERR_DENIED);
            fileStat("state=denied|reason=approval_failed");
            return false;
        }
    }

    if (!serviceEnabled(auth)) {
        sendHsResponse(s, running, HS_ERR_DENIED);
        return false;
    }
    /* The file reply is a separate frame even after HS_OK. */
    if (sendHsResponse(s, running, HS_OK) != IoResult::Ok) return false;
    if (!serviceEnabled(auth)) {
        sendFileReply(s, running, FT_ERR_DISABLED);
        return false;
    }
    if (sendFileReply(s, running, FT_OK) != IoResult::Ok) return false;
    fileStat(std::string("state=authorized|mode=") +
             (mode == MIVARA_AUTH_PASSWORD ? "password" : "manual"));
    return true;
}

FtStatus listFailureStatus(const std::string &path)
{
    DirLock dir;
    FtStatus status = dir.open(path, true);
    if (status != FT_OK) return status;
    /* The directory opened, so enumeration itself failed (I/O, not path). */
    return FT_ERR_IO;
}

bool sendListing(Sock s, const std::atomic<bool> &running,
                 const FtListing &listing, FtStatus status)
{
    std::vector<uint8_t> p = encodeListing(listing, status);
    if (p.size() > FILEXFER_MAX_FRAME_BYTES) {
        FtListing empty;
        p = encodeListing(empty, FT_ERR_LIMIT);
    }
    return sendFrame(s, FTF_LIST, p, running) == IoResult::Ok;
}

bool serviceList(Sock s, const FtTargetAuth *auth,
                 const std::atomic<bool> &running, uint16_t type,
                 const std::vector<uint8_t> &payload)
{
    if (!serviceEnabled(auth)) {
        sendResult(s, running, FT_ERR_DISABLED);
        return false;
    }
    std::string path;
    if (type == FTF_LIST) {
        if (!decodePath(payload, path, true)) {
            sendResult(s, running, FT_ERR_PROTOCOL);
            return false;
        }
    } else if (!payload.empty()) {
        sendResult(s, running, FT_ERR_PROTOCOL);
        return false;
    }
    if (type == FTF_HOME) {
        std::string error;
        if (!localHome(path, error)) {
            FtListing empty;
            return sendListing(s, running, empty, FT_ERR_NOT_FOUND);
        }
    }
    FtListing listing;
    std::string error;
    if (!listLocal(path, listing, error)) {
        FtStatus status = FT_ERR_IO;
        if (!path.empty()) {
            FtStatus candidate = listFailureStatus(path);
            if (candidate == FT_ERR_PATH || candidate == FT_ERR_NOT_FOUND)
                status = candidate;
        }
        FtListing empty;
        return sendListing(s, running, empty, status);
    }
    return sendListing(s, running, listing, FT_OK);
}

bool serviceMkdir(Sock s, const FtTargetAuth *auth,
                  const std::atomic<bool> &running,
                  const std::vector<uint8_t> &payload)
{
    if (!serviceEnabled(auth)) {
        sendResult(s, running, FT_ERR_DISABLED);
        return false;
    }
    std::string dir, name;
    if (!decodeMkdir(payload, dir, name)) {
        sendResult(s, running, FT_ERR_PROTOCOL);
        return false;
    }
    if (!absolutePathText(fs::u8path(dir))) {
        sendResult(s, running, FT_ERR_PATH);
        return true;
    }
    /* A wire-valid name this platform cannot create is a local path error,
     * not a protocol error: keep the connection reusable like PUT does. */
    if (!validLocalName(name)) {
        sendResult(s, running, FT_ERR_PATH);
        return true;
    }
    DirLock lock;
    FtStatus status = lock.open(dir, true, true);
    if (status != FT_OK) {
        sendResult(s, running, status);
        return true;
    }
    std::string created;
    status = createDirectoryLocked(lock, name, created);
    fileStat(status == FT_OK ? "state=mkdir|result=ok"
                             : "state=mkdir|result=failed");
    sendResult(s, running, status, 0,
               status == FT_OK ? created : std::string());
    return true;
}

void bestEffortTransferError(Sock s, const std::atomic<bool> &running,
                             FtStatus status)
{
    if (running.load(std::memory_order_acquire))
        sendResult(s, running, status);
}

/* Receive one PUT.  False means the byte stream can no longer be reused. */
bool servicePut(Sock s, const FtTargetAuth *auth,
                const std::atomic<bool> &running,
                const std::vector<uint8_t> &payload)
{
    std::string dir, name;
    uint64_t size = 0;
    if (!decodePut(payload, dir, name, size) || dir.empty()) {
        sendResult(s, running, FT_ERR_PROTOCOL);
        return false;
    }
    if (!absolutePathText(fs::u8path(dir))) {
        sendResult(s, running, FT_ERR_PATH);
        return true;
    }
    FileWriter writer;
    FtStatus status = writer.create(dir, name, true, size);
    if (status != FT_OK) {
        sendResult(s, running, status);
        return true;
    }
    fileStat("state=receiving");
    if (sendResult(s, running, FT_OK) != IoResult::Ok) return false;

    MivaraSha256Ctx hash;
    mivaraSha256Init(&hash);
    uint64_t received = 0;
    std::vector<uint8_t> digest(32);
    for (;;) {
        if (!running.load(std::memory_order_acquire)) return false;
        Frame frame;
        IoResult rc = recvFrame(s, frame, running, 30000);
        if (rc != IoResult::Ok) {
            if (rc == IoResult::Timeout || rc == IoResult::Protocol)
                bestEffortTransferError(s, running, FT_ERR_IO);
            return false;
        }
        if (frame.type == FTF_ABORT) {
            FtStatus ignored = FT_ERR_CANCELLED;
            if (!decodeAbort(frame.payload, ignored)) {
                bestEffortTransferError(s, running, FT_ERR_PROTOCOL);
                return false;
            }
            fileStat("state=cancelled");
            return true;
        }
        if (frame.type == FTF_DATA) {
            const size_t n = frame.payload.size();
            if (!n || n > FILEXFER_CHUNK_BYTES ||
                received + n > size || received + n < received) {
                bestEffortTransferError(s, running, FT_ERR_PROTOCOL);
                return false;
            }
            if (!writer.write(frame.payload.data(), n)) {
                bestEffortTransferError(s, running, FT_ERR_IO);
                return false;
            }
            mivaraSha256Update(&hash, frame.payload.data(), n);
            received += n;
            continue;
        }
        if (frame.type != FTF_END || frame.payload.size() != 32 ||
            received != size) {
            bestEffortTransferError(s, running, FT_ERR_PROTOCOL);
            return false;
        }
        std::copy(frame.payload.begin(), frame.payload.end(), digest.begin());
        break;
    }
    uint8_t actual[32] = {};
    mivaraSha256Final(&hash, actual);
    if (!mivaraConstEq(actual, digest.data(), 32)) {
        mivaraSecureZero(actual, sizeof(actual));
        bestEffortTransferError(s, running, FT_ERR_IO);
        return false;
    }
    mivaraSecureZero(actual, sizeof(actual));
    status = writer.finish();
    if (status == FT_OK) status = writer.commit();
    if (status != FT_OK) {
        bestEffortTransferError(s, running, status);
        return false;
    }
    std::string final = writer.finalPath();
    if (!validPathText(final, false)) final.clear();
    if (sendResult(s, running, FT_OK, size, final) != IoResult::Ok) return false;
    fileStat("state=complete|direction=receive|bytes=" + std::to_string(size));
    return true;
}

/* Send one GET.  The source is only ever opened read-only. */
bool serviceGet(Sock s, const FtTargetAuth *auth,
                const std::atomic<bool> &running,
                const std::vector<uint8_t> &payload)
{
    std::string path;
    if (!decodePath(payload, path, false)) {
        sendResult(s, running, FT_ERR_PROTOCOL);
        return false;
    }
    if (!absolutePathText(fs::u8path(path))) {
        sendResult(s, running, FT_ERR_PATH);
        return true;
    }
    FileReader reader;
    FtStatus status = reader.open(path, true);
    if (status != FT_OK) {
        sendResult(s, running, status);
        return true;
    }
    const std::string name = reader.name();
    if (!validWireName(name)) {
        sendResult(s, running, FT_ERR_PATH);
        return true;
    }
    const uint64_t size = reader.size();
    if (sendFrame(s, FTF_PUT_BEGIN, encodePut("", name, size), running) !=
        IoResult::Ok)
        return false;
    fileStat("state=sending");

    MivaraSha256Ctx hash;
    mivaraSha256Init(&hash);
    std::vector<uint8_t> data(FILEXFER_CHUNK_BYTES);
    uint64_t sent = 0;
    while (sent < size) {
        if (!running.load(std::memory_order_acquire)) return false;
        const size_t want = (size_t)std::min<uint64_t>(data.size(), size - sent);
        size_t got = 0;
        if (!reader.read(data.data(), want, got) || !got) {
            sendAbort(s, running, FT_ERR_IO, "source_changed_or_read_failed");
            return false;
        }
        mivaraSha256Update(&hash, data.data(), got);
        std::vector<uint8_t> body(data.begin(), data.begin() + got);
        if (sendFrame(s, FTF_DATA, body, running) != IoResult::Ok) return false;
        sent += got;
    }
    uint8_t extra = 0;
    size_t extra_read = 0;
    if (!reader.read(&extra, 1, extra_read) || extra_read ||
        !reader.sizeStill(size)) {
        sendAbort(s, running, FT_ERR_IO, "source_changed");
        fileStat("state=error|reason=source_changed");
        return false;
    }
    reader.close();
    uint8_t digest[32] = {};
    mivaraSha256Final(&hash, digest);
    std::vector<uint8_t> end(digest, digest + 32);
    mivaraSecureZero(digest, sizeof(digest));
    if (sendFrame(s, FTF_END, end, running) != IoResult::Ok) return false;
    if (sendResult(s, running, FT_OK, size) != IoResult::Ok) return false;
    fileStat("state=complete|direction=send|bytes=" + std::to_string(size));
    return true;
}

void serveServiceClient(Sock s, const std::string &peer,
                        const FtTargetAuth *auth,
                        const std::atomic<bool> &running)
{
    tuneSock(s);
    FilexferHello hello{};
    if (recvAll(s, &hello, sizeof(hello), running, 5000) != IoResult::Ok) return;
    if (ntohl(hello.magic) != FILEXFER_MAGIC ||
        ntohl(hello.version) != FILEXFER_VERSION) {
        sendFileReply(s, running, FT_ERR_VERSION);
        fileStat("state=denied|reason=version");
        return;
    }
    if (!authorizeServiceClient(s, peer, hello, auth, running)) return;
    /* Announce the service's device name once, before any request reply. The
     * client reads this frame as part of its handshake (protocol v2). */
    if (sendFrame(s, FTF_INFO, encodeInfo(localDeviceName()), running) !=
        IoResult::Ok)
        return;

    while (running.load(std::memory_order_acquire)) {
        Frame frame;
        /* Keep an authorized but idle page alive; TCP keepalive still
         * detects a vanished peer, and the shutdown flag is polled every
         * second inside waitSock. */
        IoResult rc = recvFrame(s, frame, running, 30 * 60 * 1000);
        if (rc != IoResult::Ok) {
            if (rc == IoResult::Timeout) fileStat("state=idle_timeout");
            break;
        }
        if (!serviceEnabled(auth)) {
            sendResult(s, running, FT_ERR_DISABLED);
            fileStat("state=disabled");
            break;
        }
        bool reusable = false;
        switch (frame.type) {
        case FTF_HOME:
        case FTF_ROOTS:
        case FTF_LIST:
            reusable = serviceList(s, auth, running, frame.type, frame.payload);
            break;
        case FTF_PUT_BEGIN:
            reusable = servicePut(s, auth, running, frame.payload);
            break;
        case FTF_GET:
            reusable = serviceGet(s, auth, running, frame.payload);
            break;
        case FTF_MKDIR:
            reusable = serviceMkdir(s, auth, running, frame.payload);
            break;
        case FTF_ABORT: {
            FtStatus ignored = FT_ERR_CANCELLED;
            if (!decodeAbort(frame.payload, ignored)) {
                sendResult(s, running, FT_ERR_PROTOCOL);
                return;
            }
            fileStat("state=cancelled");
            return;
        }
        default:
            sendResult(s, running, FT_ERR_PROTOCOL);
            fileStat("state=error|reason=protocol");
            return;
        }
        if (!reusable) break;
    }
}

/* ── app client: one worker, one pending operation, bounded events ─────── */

enum class OpKind { Roots, Home, List, Upload, Download, Mkdir };
struct ClientOp {
    OpKind kind = OpKind::Roots;
    std::string first;
    std::string second;
};

struct OpOutcome {
    bool fatal = false;
    FtStatus status = FT_OK;
    std::string message;
};

struct Client {
    ~Client();
    std::mutex lifetime;
    std::thread worker;
    std::atomic<bool> active{false};
    std::atomic<bool> running{false};
    std::atomic<bool> busy{false};
    /* Requested by ftClientCancelOperation(); cleared when the operation ends
     * or a new one is queued. */
    std::atomic<bool> op_abort{false};
    std::atomic<intptr_t> socket{(intptr_t)kInvalidSock};
    /* Guards "load + shutdown" against "store invalid + close" so a reused
     * descriptor/handle can never be shut down by a stale stop signal. */
    std::mutex socket_mutex;

    std::mutex op_mutex;
    std::condition_variable op_cv;
    bool pending = false;
    ClientOp op;

    std::mutex password_mutex;
    std::condition_variable password_cv;
    bool password_waiting = false;
    bool password_answered = false;
    bool password_cancelled = false;
    std::string password;

    std::mutex event_mutex;
    std::deque<FtEvent> events;
};
Client g_client;
static const size_t kMaxEvents = 128;

void wipeString(std::string &s)
{
    if (!s.empty()) mivaraSecureZero(&s[0], s.size());
    s.clear();
}

void pushEvent(FtEvent event)
{
    std::lock_guard<std::mutex> lock(g_client.event_mutex);
    if (event.type == FT_EV_PROGRESS && !g_client.events.empty()) {
        FtEvent &last = g_client.events.back();
        if (last.type == FT_EV_PROGRESS && last.upload == event.upload &&
            last.name == event.name) {
            last = std::move(event);
            return;
        }
    }
    if (g_client.events.size() >= kMaxEvents) {
        auto old = std::find_if(g_client.events.begin(), g_client.events.end(),
                                [](const FtEvent &e) {
                                    return e.type == FT_EV_PROGRESS;
                                });
        if (old != g_client.events.end()) g_client.events.erase(old);
        else {
            old = std::find_if(g_client.events.begin(), g_client.events.end(),
                               [](const FtEvent &e) {
                                   return e.type == FT_EV_NOTICE ||
                                          e.type == FT_EV_STATE;
                               });
            if (old != g_client.events.end()) g_client.events.erase(old);
            else g_client.events.pop_front();
        }
    }
    g_client.events.push_back(std::move(event));
}

void clientState(FtClientState state, const std::string &message = std::string(),
                 FtStatus status = FT_OK)
{
    FtEvent event;
    event.type = FT_EV_STATE;
    event.state = state;
    event.status = status;
    event.message = message;
    pushEvent(std::move(event));
}

void clientProgress(bool upload, const std::string &name, uint64_t bytes,
                    uint64_t total)
{
    FtEvent event;
    event.type = FT_EV_PROGRESS;
    event.upload = upload;
    event.name = name;
    event.bytes = bytes;
    event.total = total;
    pushEvent(std::move(event));
}

bool operationAbortRequested()
{
    return g_client.op_abort.load(std::memory_order_acquire);
}

OpOutcome finishOperation(const ClientOp &op, FtStatus status, uint64_t bytes,
                          uint64_t total, const std::string &name,
                          const std::string &message, bool fatal,
                          const FtListing *listing = nullptr)
{
    g_client.busy.store(false, std::memory_order_release);
    g_client.op_abort.store(false, std::memory_order_release);
    FtEvent event;
    event.status = status;
    event.upload = op.kind == OpKind::Upload;
    event.name = name;
    event.bytes = bytes;
    event.total = total;
    event.message = message;
    event.roots = op.kind == OpKind::Roots ||
                  (op.kind == OpKind::List && op.first.empty());
    event.mkdir = op.kind == OpKind::Mkdir;
    if (op.kind == OpKind::Roots || op.kind == OpKind::Home ||
        op.kind == OpKind::List) {
        event.type = FT_EV_LIST;
        if (listing) event.listing = *listing;
    } else {
        event.type = FT_EV_COMPLETE;
    }
    pushEvent(std::move(event));
    OpOutcome outcome;
    outcome.fatal = fatal;
    outcome.status = status;
    outcome.message = message;
    return outcome;
}

bool queueOperation(ClientOp &&op)
{
    std::lock_guard<std::mutex> lock(g_client.op_mutex);
    if (!g_client.active.load(std::memory_order_acquire) ||
        !g_client.running.load(std::memory_order_acquire) ||
        g_client.busy.load(std::memory_order_acquire))
        return false;
    g_client.op = std::move(op);
    g_client.pending = true;
    g_client.op_abort.store(false, std::memory_order_release);
    g_client.busy.store(true, std::memory_order_release);
    g_client.op_cv.notify_one();
    return true;
}

ClientOp takePendingOperation()
{
    std::lock_guard<std::mutex> lock(g_client.op_mutex);
    ClientOp op;
    if (g_client.pending) {
        op = std::move(g_client.op);
        g_client.op = ClientOp{};
        g_client.pending = false;
    }
    return op;
}

bool hasPendingOperation()
{
    std::lock_guard<std::mutex> lock(g_client.op_mutex);
    return g_client.pending;
}

void signalStop()
{
    g_client.running.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(g_client.socket_mutex);
        Sock s = (Sock)g_client.socket.load(std::memory_order_acquire);
        if (s != kInvalidSock) shutdownSock(s);
    }
    g_client.op_cv.notify_all();
    g_client.password_cv.notify_all();
}

Client::~Client()
{
    std::lock_guard<std::mutex> lock(lifetime);
    signalStop();
    if (worker.joinable()) {
        if (worker.get_id() == std::this_thread::get_id()) worker.detach();
        else worker.join();
    }
    wipeString(password);
}

FtStatus statusFromHs(uint32_t status)
{
    switch (status) {
    case HS_ERR_VERSION: return FT_ERR_VERSION;
    case HS_ERR_BUSY: return FT_ERR_BUSY;
    default: return FT_ERR_AUTH;
    }
}

std::string hsStatusText(uint32_t status)
{
    switch (status) {
    case HS_ERR_AUTH: return "连接密码错误，或尝试次数过多";
    case HS_ERR_AUTH_TIMEOUT: return "对方未及时确认文件传输请求";
    case HS_ERR_DENIED: return "对方拒绝了文件传输请求";
    case HS_ERR_BUSY: return "对方正在处理其他连接或授权请求";
    case HS_ERR_AUTH_CONFIG: return "对方授权配置异常";
    case HS_ERR_VERSION: return "协议版本不兼容";
    default: return "文件传输授权失败";
    }
}

bool readReplyAfterMagic(Sock s, uint32_t first_raw,
                         const std::atomic<bool> &running, FtStatus &status,
                         std::string &why)
{
    FilexferReply reply{};
    reply.magic = first_raw;
    if (recvAll(s, (char *)&reply + 4, sizeof(reply) - 4, running, 10000) !=
        IoResult::Ok) {
        why = "未收到完整的文件服务应答";
        return false;
    }
    if (ntohl(reply.magic) != FILEXFER_MAGIC ||
        ntohl(reply.version) != FILEXFER_VERSION) {
        status = FT_ERR_VERSION;
        why = ftStatusText(status);
        return false;
    }
    uint32_t raw = ntohl(reply.status);
    if (!validStatus(raw)) {
        status = FT_ERR_PROTOCOL;
        why = ftStatusText(status);
        return false;
    }
    status = (FtStatus)raw;
    if (status != FT_OK) {
        why = ftStatusText(status);
        return false;
    }
    return true;
}

bool waitForPassword(std::string &password, FtStatus &status, std::string &why)
{
    std::unique_lock<std::mutex> lock(g_client.password_mutex);
    g_client.password_waiting = true;
    g_client.password_answered = false;
    g_client.password_cancelled = false;
    wipeString(g_client.password);
    lock.unlock();
    clientState(FT_CLIENT_NEED_PASSWORD);
    lock.lock();
    const bool answered = g_client.password_cv.wait_for(
        lock, std::chrono::seconds(120), [] {
            return !g_client.running.load(std::memory_order_acquire) ||
                   g_client.password_answered || g_client.password_cancelled;
        });
    g_client.password_waiting = false;
    if (!answered) {
        status = FT_ERR_AUTH;
        why = "密码输入超时";
        return false;
    }
    if (!g_client.running.load(std::memory_order_acquire) ||
        g_client.password_cancelled || g_client.password.empty()) {
        wipeString(g_client.password);
        status = FT_ERR_CANCELLED;
        why = "已取消输入连接密码";
        return false;
    }
    password.swap(g_client.password);
    wipeString(g_client.password);
    return true;
}

bool clientHandshake(Sock s, const std::atomic<bool> &running, FtStatus &status,
                     std::string &why)
{
    FilexferHello hello{};
    hello.magic = htonl(FILEXFER_MAGIC);
    hello.version = htonl(FILEXFER_VERSION);
    if (sendAll(s, &hello, sizeof(hello), running, 5000) != IoResult::Ok) {
        status = running.load() ? FT_ERR_IO : FT_ERR_CANCELLED;
        why = running.load() ? "文件服务握手发送失败" : "已取消";
        return false;
    }
    uint32_t first_raw = 0;
    if (recvAll(s, &first_raw, 4, running, 10000) != IoResult::Ok) {
        status = running.load() ? FT_ERR_IO : FT_ERR_CANCELLED;
        why = running.load() ? "文件服务未回应握手" : "已取消";
        return false;
    }
    const uint32_t first = ntohl(first_raw);
    if (first == FILEXFER_MAGIC)
        return readReplyAfterMagic(s, first_raw, running, status, why);

    if (first == TVHS_MAGIC) {
        HsResponseV2 response{};
        response.magic = first_raw;
        if (recvAll(s, (char *)&response + 4, sizeof(response) - 4, running,
                    10000) != IoResult::Ok ||
            ntohl(response.magic) != TVHS_MAGIC ||
            ntohl(response.version) != TVHS_VERSION) {
            status = FT_ERR_PROTOCOL;
            why = "授权拒绝应答无效";
            return false;
        }
        const uint32_t hs = ntohl(response.status);
        status = statusFromHs(hs);
        why = hsStatusText(hs);
        return false;
    }
    if (first != TVHA_MAGIC) {
        status = FT_ERR_PROTOCOL;
        why = "对方不是兼容的文件服务";
        return false;
    }

    HsAuthStart challenge{};
    challenge.magic = first_raw;
    if (recvAll(s, (char *)&challenge + 4, sizeof(challenge) - 4, running,
                10000) != IoResult::Ok ||
        ntohl(challenge.magic) != TVHA_MAGIC ||
        ntohl(challenge.version) != TVHA_VERSION) {
        status = FT_ERR_PROTOCOL;
        why = "授权挑战无效";
        return false;
    }
    const uint32_t mode = ntohl(challenge.mode);
    if (mode != AUTH_MODE_PASSWORD && mode != AUTH_MODE_MANUAL) {
        status = FT_ERR_PROTOCOL;
        why = "对方下发了无效的授权方式";
        return false;
    }

    HsRequestExtV3 ext{};
    ext.magic = htonl(TVHX_MAGIC);
    ext.version = htonl(TVHA_VERSION);
    const std::string device = localDeviceName();
    std::memcpy(ext.device, device.data(), std::min(device.size(), sizeof(ext.device)));
    if (sendAll(s, &ext, sizeof(ext), running, 5000) != IoResult::Ok) {
        status = running.load() ? FT_ERR_IO : FT_ERR_CANCELLED;
        why = running.load() ? "授权身份发送失败" : "已取消";
        return false;
    }

    if (mode == AUTH_MODE_PASSWORD) {
        const uint32_t iterations = ntohl(challenge.kdf_iter);
        if (iterations < HS_KDF_ITER_MIN || iterations > HS_KDF_ITER_MAX) {
            status = FT_ERR_AUTH;
            why = "对方下发了无效的密码参数";
            return false;
        }
        std::string password;
        if (!waitForPassword(password, status, why)) return false;
        clientState(FT_CLIENT_VERIFYING);
        uint8_t key[32] = {}, proof[32] = {};
        mivaraPasswordKey(password, challenge.salt, sizeof(challenge.salt),
                          iterations, key);
        wipeString(password);
        uint8_t bound[sizeof(FilexferHello) + sizeof(HsRequestExtV3)];
        std::memcpy(bound, &hello, sizeof(hello));
        std::memcpy(bound + sizeof(hello), &ext, sizeof(ext));
        mivaraAuthProof(key, MIVARA_AUTH_PURPOSE_FILES, challenge.nonce,
                        bound, sizeof(bound), proof);
        mivaraSecureZero(key, sizeof(key));
        HsAuthProof response{};
        response.magic = htonl(TVHA_MAGIC);
        response.version = htonl(TVHA_VERSION);
        std::memcpy(response.proof, proof, sizeof(proof));
        mivaraSecureZero(proof, sizeof(proof));
        if (sendAll(s, &response, sizeof(response), running, 5000) != IoResult::Ok) {
            mivaraSecureZero(&response, sizeof(response));
            status = running.load() ? FT_ERR_IO : FT_ERR_CANCELLED;
            why = running.load() ? "密码应答发送失败" : "已取消";
            return false;
        }
        mivaraSecureZero(&response, sizeof(response));
    } else {
        clientState(FT_CLIENT_WAIT_AUTH);
    }

    HsResponseV2 response{};
    if (recvAll(s, &response, sizeof(response), running, 35000) != IoResult::Ok) {
        status = running.load() ? FT_ERR_IO : FT_ERR_CANCELLED;
        why = running.load() ? "授权过程中连接已断开或超时" : "已取消";
        return false;
    }
    if (ntohl(response.magic) != TVHS_MAGIC ||
        ntohl(response.version) != TVHS_VERSION) {
        status = FT_ERR_PROTOCOL;
        why = "授权应答无效";
        return false;
    }
    const uint32_t hs = ntohl(response.status);
    if (hs != HS_OK) {
        status = statusFromHs(hs);
        why = hsStatusText(hs);
        return false;
    }
    FilexferReply reply{};
    if (recvAll(s, &reply, sizeof(reply), running, 10000) != IoResult::Ok ||
        ntohl(reply.magic) != FILEXFER_MAGIC) {
        status = running.load() ? FT_ERR_IO : FT_ERR_CANCELLED;
        why = running.load() ? "授权后未收到文件服务应答" : "已取消";
        return false;
    }
    if (ntohl(reply.version) != FILEXFER_VERSION) {
        status = FT_ERR_VERSION;
        why = ftStatusText(status);
        return false;
    }
    const uint32_t raw = ntohl(reply.status);
    if (!validStatus(raw)) {
        status = FT_ERR_PROTOCOL;
        why = ftStatusText(status);
        return false;
    }
    status = (FtStatus)raw;
    if (status != FT_OK) {
        why = ftStatusText(status);
        return false;
    }
    return true;
}

/* ── client operations (all called on the worker) ───────────────────────── */

OpOutcome operationIoFailure(const ClientOp &op, uint64_t bytes, uint64_t total,
                             const std::string &name)
{
    const bool cancelled = !g_client.running.load(std::memory_order_acquire);
    FtStatus status = cancelled ? FT_ERR_CANCELLED : FT_ERR_IO;
    return finishOperation(op, status, bytes, total, name,
                           cancelled ? "已取消" : "连接已断开或读写超时", true);
}

OpOutcome operationProtocolFailure(const ClientOp &op, uint64_t bytes,
                                   uint64_t total, const std::string &name)
{
    return finishOperation(op, FT_ERR_PROTOCOL, bytes, total, name,
                           ftStatusText(FT_ERR_PROTOCOL), true);
}

OpOutcome runClientList(Sock s, const ClientOp &op)
{
    uint16_t type = FTF_LIST;
    std::vector<uint8_t> payload;
    if (op.kind == OpKind::Roots ||
        (op.kind == OpKind::List && op.first.empty())) {
        type = FTF_ROOTS;
    } else if (op.kind == OpKind::Home) {
        type = FTF_HOME;
    } else {
        payload = encodePath(op.first);
    }
    IoResult rc = sendFrame(s, type, payload, g_client.running);
    if (rc != IoResult::Ok) return operationIoFailure(op, 0, 0, std::string());
    Frame frame;
    rc = recvFrame(s, frame, g_client.running, 30000);
    if (rc != IoResult::Ok) return operationIoFailure(op, 0, 0, std::string());
    if (frame.type == FTF_ABORT) {
        FtStatus status = FT_ERR_IO;
        if (!decodeAbort(frame.payload, status))
            return operationProtocolFailure(op, 0, 0, std::string());
        return finishOperation(op, status, 0, 0, std::string(),
                               ftStatusText(status), true);
    }
    if (frame.type == FTF_RESULT) {
        WireResult result;
        if (!decodeResult(frame.payload, result))
            return operationProtocolFailure(op, 0, 0, std::string());
        if (result.status == FT_OK)
            return operationProtocolFailure(op, 0, 0, std::string());
        return finishOperation(op, result.status, 0, 0, std::string(),
                               ftStatusText(result.status), false);
    }
    if (frame.type != FTF_LIST)
        return operationProtocolFailure(op, 0, 0, std::string());
    FtListing listing;
    FtStatus status = FT_OK;
    std::string message;
    if (!decodeListing(frame.payload, listing, status, message))
        return operationProtocolFailure(op, 0, 0, std::string());
    if (status == FT_OK) {
        try {
            const bool roots = type == FTF_ROOTS;
            if ((roots && !listing.path.empty() && listing.path != "/") ||
                (!roots && (listing.path.empty() || !wireAbsolute(listing.path))))
                return operationProtocolFailure(op, 0, 0, std::string());
        } catch (...) {
            return operationProtocolFailure(op, 0, 0, std::string());
        }
    }
    return finishOperation(op, status, 0, 0, std::string(), message, false,
                           &listing);
}

OpOutcome runClientUpload(Sock s, const ClientOp &op)
{
    FileReader reader;
    FtStatus status = reader.open(op.first, false);
    const std::string fallback_name = ftPathName(op.first);
    if (operationAbortRequested())
        return finishOperation(op, FT_ERR_CANCELLED, 0, 0, fallback_name,
                               "已取消", false);
    if (status != FT_OK)
        return finishOperation(op, status, 0, 0, fallback_name,
                               status == FT_ERR_LIMIT ? ftStatusText(status)
                                                      : "无法读取源文件", false);
    const std::string name = reader.name();
    const uint64_t size = reader.size();
    if (!validWireName(name))
        return finishOperation(op, FT_ERR_PATH, 0, size, name,
                               "源文件名无效", false);
    IoResult rc = sendFrame(s, FTF_PUT_BEGIN,
                            encodePut(op.second, name, size), g_client.running);
    if (rc != IoResult::Ok) return operationIoFailure(op, 0, size, name);
    Frame frame;
    rc = recvFrame(s, frame, g_client.running, 30000);
    if (rc != IoResult::Ok) return operationIoFailure(op, 0, size, name);
    if (frame.type == FTF_ABORT) {
        FtStatus remote = FT_ERR_IO;
        if (!decodeAbort(frame.payload, remote))
            return operationProtocolFailure(op, 0, size, name);
        return finishOperation(op, remote, 0, size, name, ftStatusText(remote), true);
    }
    if (frame.type != FTF_RESULT)
        return operationProtocolFailure(op, 0, size, name);
    WireResult result;
    if (!decodeResult(frame.payload, result))
        return operationProtocolFailure(op, 0, size, name);
    if (result.status != FT_OK)
        return finishOperation(op, result.status, 0, size, name,
                               ftStatusText(result.status), false);
    if (result.bytes != 0 || !result.text.empty())
        return operationProtocolFailure(op, 0, size, name);

    clientProgress(true, name, 0, size);
    MivaraSha256Ctx hash;
    mivaraSha256Init(&hash);
    std::vector<uint8_t> data(FILEXFER_CHUNK_BYTES);
    uint64_t sent = 0;
    while (sent < size) {
        if (operationAbortRequested()) {
            /* The peer reads control frames while receiving a PUT, so an ABORT
             * here leaves the byte stream reusable for the next queued file.
             * A failed ABORT means the stream may be half-written: terminal. */
            if (sendAbort(s, g_client.running, FT_ERR_CANCELLED,
                          "cancelled") != IoResult::Ok)
                return operationIoFailure(op, sent, size, name);
            return finishOperation(op, FT_ERR_CANCELLED, sent, size, name,
                                   "已取消", false);
        }
        if (!g_client.running.load(std::memory_order_acquire))
            return finishOperation(op, FT_ERR_CANCELLED, sent, size, name,
                                   "已取消", true);
        const size_t want = (size_t)std::min<uint64_t>(data.size(), size - sent);
        size_t got = 0;
        if (!reader.read(data.data(), want, got) || !got) {
            sendAbort(s, g_client.running, FT_ERR_IO, "source_read_failed");
            return finishOperation(op, FT_ERR_IO, sent, size, name,
                                   "源文件读取失败或尺寸发生变化", true);
        }
        mivaraSha256Update(&hash, data.data(), got);
        std::vector<uint8_t> body(data.begin(), data.begin() + got);
        rc = sendFrame(s, FTF_DATA, body, g_client.running);
        if (rc != IoResult::Ok) return operationIoFailure(op, sent, size, name);
        sent += got;
        clientProgress(true, name, sent, size);
    }
    uint8_t extra = 0;
    size_t extra_read = 0;
    if (!reader.read(&extra, 1, extra_read) || extra_read ||
        !reader.sizeStill(size)) {
        sendAbort(s, g_client.running, FT_ERR_IO, "source_changed");
        return finishOperation(op, FT_ERR_IO, sent, size, name,
                               "源文件在传输过程中发生变化", true);
    }
    reader.close();
    uint8_t digest[32] = {};
    mivaraSha256Final(&hash, digest);
    std::vector<uint8_t> end(digest, digest + 32);
    mivaraSecureZero(digest, sizeof(digest));
    rc = sendFrame(s, FTF_END, end, g_client.running);
    if (rc != IoResult::Ok) return operationIoFailure(op, sent, size, name);
    rc = recvFrame(s, frame, g_client.running, 30000);
    if (rc != IoResult::Ok) return operationIoFailure(op, sent, size, name);
    if (frame.type == FTF_ABORT) {
        FtStatus remote = FT_ERR_IO;
        if (!decodeAbort(frame.payload, remote))
            return operationProtocolFailure(op, sent, size, name);
        return finishOperation(op, remote, sent, size, name,
                               ftStatusText(remote), true);
    }
    if (frame.type != FTF_RESULT)
        return operationProtocolFailure(op, sent, size, name);
    if (!decodeResult(frame.payload, result))
        return operationProtocolFailure(op, sent, size, name);
    if (result.status != FT_OK)
        return finishOperation(op, result.status, sent, size, name,
                               ftStatusText(result.status), true);
    if (result.bytes != size || !validPathText(result.text, true))
        return operationProtocolFailure(op, sent, size, name);
    if (!result.text.empty() && !wireAbsolute(result.text))
        return operationProtocolFailure(op, sent, size, name);
    return finishOperation(op, FT_OK, size, size, name, result.text, false);
}

OpOutcome runClientDownload(Sock s, const ClientOp &op)
{
    const std::string fallback_name = ftPathName(op.first);
    if (operationAbortRequested())
        return finishOperation(op, FT_ERR_CANCELLED, 0, 0, fallback_name,
                               "已取消", false);
    IoResult rc = sendFrame(s, FTF_GET, encodePath(op.first), g_client.running);
    if (rc != IoResult::Ok)
        return operationIoFailure(op, 0, 0, fallback_name);
    Frame frame;
    rc = recvFrame(s, frame, g_client.running, 30000);
    if (rc != IoResult::Ok)
        return operationIoFailure(op, 0, 0, fallback_name);
    if (frame.type == FTF_ABORT) {
        FtStatus remote = FT_ERR_IO;
        if (!decodeAbort(frame.payload, remote))
            return operationProtocolFailure(op, 0, 0, fallback_name);
        return finishOperation(op, remote, 0, 0, fallback_name,
                               ftStatusText(remote), true);
    }
    if (frame.type == FTF_RESULT) {
        WireResult early;
        if (!decodeResult(frame.payload, early))
            return operationProtocolFailure(op, 0, 0, fallback_name);
        if (early.status == FT_OK)
            return operationProtocolFailure(op, 0, 0, fallback_name);
        return finishOperation(op, early.status, 0, 0, fallback_name,
                               ftStatusText(early.status), false);
    }
    if (frame.type != FTF_PUT_BEGIN)
        return operationProtocolFailure(op, 0, 0, fallback_name);
    std::string empty_dir, name;
    uint64_t size = 0;
    if (!decodePut(frame.payload, empty_dir, name, size) || !empty_dir.empty())
        return operationProtocolFailure(op, 0, 0, fallback_name);
    FileWriter writer;
    FtStatus status = writer.create(op.second, name, false, size);
    if (status != FT_OK) {
        sendAbort(s, g_client.running, status, "local_destination_unavailable");
        const char *why = status == FT_ERR_PATH
            ? "本机无法创建该文件名（可能包含本机不允许的字符）"
            : (status == FT_ERR_LIMIT ? "文件名超出本机文件系统的长度限制"
                                      : ftStatusText(status));
        return finishOperation(op, status, 0, size, name, why, true);
    }

    clientProgress(false, name, 0, size);
    MivaraSha256Ctx hash;
    mivaraSha256Init(&hash);
    uint64_t received = 0;
    std::vector<uint8_t> expected(32);
    for (;;) {
        if (operationAbortRequested()) {
            /* The sender does not read while streaming a file, so there is no
             * in-band cancel for a download: close the connection instead. */
            signalStop();
            return finishOperation(op, FT_ERR_CANCELLED, received, size, name,
                                   "已取消", true);
        }
        rc = recvFrame(s, frame, g_client.running, 30000);
        if (rc != IoResult::Ok)
            return operationIoFailure(op, received, size, name);
        if (frame.type == FTF_ABORT) {
            FtStatus remote = FT_ERR_IO;
            if (!decodeAbort(frame.payload, remote))
                return operationProtocolFailure(op, received, size, name);
            return finishOperation(op, remote, received, size, name,
                                   ftStatusText(remote), true);
        }
        if (frame.type == FTF_DATA) {
            const size_t n = frame.payload.size();
            if (!n || n > FILEXFER_CHUNK_BYTES ||
                received + n > size || received + n < received)
                return operationProtocolFailure(op, received, size, name);
            if (!writer.write(frame.payload.data(), n)) {
                sendAbort(s, g_client.running, FT_ERR_IO, "local_write_failed");
                return finishOperation(op, FT_ERR_IO, received, size, name,
                                       "本地文件写入失败", true);
            }
            mivaraSha256Update(&hash, frame.payload.data(), n);
            received += n;
            clientProgress(false, name, received, size);
            continue;
        }
        if (frame.type != FTF_END || frame.payload.size() != 32 ||
            received != size)
            return operationProtocolFailure(op, received, size, name);
        std::copy(frame.payload.begin(), frame.payload.end(), expected.begin());
        break;
    }
    uint8_t actual[32] = {};
    mivaraSha256Final(&hash, actual);
    const bool hash_ok = mivaraConstEq(actual, expected.data(), 32);
    mivaraSecureZero(actual, sizeof(actual));
    if (!hash_ok) {
        sendAbort(s, g_client.running, FT_ERR_IO, "checksum_mismatch");
        return finishOperation(op, FT_ERR_IO, received, size, name,
                               "文件校验失败", true);
    }
    status = writer.finish();
    if (status != FT_OK) {
        sendAbort(s, g_client.running, status, "local_flush_failed");
        return finishOperation(op, status, received, size, name,
                               ftStatusText(status), true);
    }
    rc = recvFrame(s, frame, g_client.running, 30000);
    if (rc != IoResult::Ok)
        return operationIoFailure(op, received, size, name);
    if (frame.type == FTF_ABORT) {
        FtStatus remote = FT_ERR_IO;
        if (!decodeAbort(frame.payload, remote))
            return operationProtocolFailure(op, received, size, name);
        return finishOperation(op, remote, received, size, name,
                               ftStatusText(remote), true);
    }
    if (frame.type != FTF_RESULT)
        return operationProtocolFailure(op, received, size, name);
    WireResult result;
    if (!decodeResult(frame.payload, result))
        return operationProtocolFailure(op, received, size, name);
    if (result.status != FT_OK)
        return finishOperation(op, result.status, received, size, name,
                               ftStatusText(result.status), true);
    if (result.bytes != size || !result.text.empty())
        return operationProtocolFailure(op, received, size, name);
    status = writer.commit();
    if (status != FT_OK)
        return finishOperation(op, status, received, size, name,
                               ftStatusText(status), false);
    return finishOperation(op, FT_OK, size, size, name, writer.finalPath(), false);
}

OpOutcome runClientMkdir(Sock s, const ClientOp &op)
{
    if (operationAbortRequested())
        return finishOperation(op, FT_ERR_CANCELLED, 0, 0, op.second,
                               "已取消", false);
    IoResult rc = sendFrame(s, FTF_MKDIR, encodeMkdir(op.first, op.second),
                            g_client.running);
    if (rc != IoResult::Ok) return operationIoFailure(op, 0, 0, op.second);
    Frame frame;
    rc = recvFrame(s, frame, g_client.running, 30000);
    if (rc != IoResult::Ok) return operationIoFailure(op, 0, 0, op.second);
    if (frame.type == FTF_ABORT) {
        FtStatus remote = FT_ERR_IO;
        if (!decodeAbort(frame.payload, remote))
            return operationProtocolFailure(op, 0, 0, op.second);
        return finishOperation(op, remote, 0, 0, op.second,
                               ftStatusText(remote), true);
    }
    if (frame.type != FTF_RESULT)
        return operationProtocolFailure(op, 0, 0, op.second);
    WireResult result;
    if (!decodeResult(frame.payload, result))
        return operationProtocolFailure(op, 0, 0, op.second);
    if (result.status != FT_OK)
        return finishOperation(op, result.status, 0, 0, op.second,
                               ftStatusText(result.status), false);
    if (result.bytes != 0 || !validPathText(result.text, false) ||
        !wireAbsolute(result.text))
        return operationProtocolFailure(op, 0, 0, op.second);
    return finishOperation(op, FT_OK, 0, 0, op.second, result.text, false);
}

OpOutcome runClientOperation(Sock s, const ClientOp &op)
{
    switch (op.kind) {
    case OpKind::Roots:
    case OpKind::Home:
    case OpKind::List:
        return runClientList(s, op);
    case OpKind::Upload:
        return runClientUpload(s, op);
    case OpKind::Download:
        return runClientDownload(s, op);
    case OpKind::Mkdir:
        return runClientMkdir(s, op);
    }
    return finishOperation(op, FT_ERR_UNSUPPORTED, 0, 0, std::string(),
                           ftStatusText(FT_ERR_UNSUPPORTED), false);
}

bool peerClosed(Sock s)
{
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(s, &rd);
    timeval tv{};
#ifdef _WIN32
    int rc = select(0, &rd, nullptr, nullptr, &tv);
#else
    int rc = select(s + 1, &rd, nullptr, nullptr, &tv);
#endif
    if (rc <= 0) return false;
    char byte = 0;
    int n = recv(s, &byte, 1, MSG_PEEK);
    if (n == 0) return true;
    if (n > 0) return true; /* no unsolicited frame is valid while idle */
    return !wouldBlock(sockError());
}

void clientWorker(std::string ip, uint16_t port)
{
    FtStatus terminal_status = FT_OK;
    std::string terminal_message;
    bool failed = false;
    bool cancelled = false;
    bool operation_active = false;
    ClientOp current;
    SockHolder socket;

    try {
        clientState(FT_CLIENT_CONNECTING);
        Sock connected = connectSock(ip, port, g_client.running, 5000);
        if (connected == kInvalidSock) {
            failed = g_client.running.load(std::memory_order_acquire);
            cancelled = !failed;
            terminal_status = failed ? FT_ERR_IO : FT_ERR_CANCELLED;
            terminal_message = failed
                ? "无法连接文件服务（对方需开启接收并使用支持文件传输的版本）"
                : "已取消";
        } else {
            socket.reset(connected);
            {
                std::lock_guard<std::mutex> lock(g_client.socket_mutex);
                g_client.socket.store((intptr_t)connected,
                                      std::memory_order_release);
            }
            if (!clientHandshake(connected, g_client.running, terminal_status,
                                 terminal_message)) {
                failed = g_client.running.load(std::memory_order_acquire) &&
                         terminal_status != FT_ERR_CANCELLED;
                cancelled = !failed;
            } else {
                /* Protocol v2: the admitted service announces its device name
                 * before the request loop. Read it here, after BOTH handshake
                 * success paths (the open-mode path returns early from
                 * readReplyAfterMagic), and before READY so the GUI has the
                 * name when its READY-triggered list request runs. */
                Frame info;
                const IoResult info_rc =
                    recvFrame(connected, info, g_client.running, 10000);
                std::string peer_device;
                if (info_rc != IoResult::Ok || info.type != FTF_INFO ||
                    !decodeInfo(info.payload, peer_device)) {
                    const bool stopped =
                        !g_client.running.load(std::memory_order_acquire);
                    terminal_status = stopped ? FT_ERR_CANCELLED
                                              : FT_ERR_PROTOCOL;
                    terminal_message = stopped
                        ? "已取消" : "文件服务未发送有效的设备信息";
                    failed = !stopped;
                    cancelled = stopped;
                } else {
                {
                    FtEvent event;
                    event.type = FT_EV_INFO;
                    event.device = sanitizeDevice(peer_device);
                    pushEvent(std::move(event));
                }
                clientState(FT_CLIENT_READY);
                while (g_client.running.load(std::memory_order_acquire)) {
                    std::unique_lock<std::mutex> lock(g_client.op_mutex);
                    g_client.op_cv.wait_for(lock, std::chrono::seconds(1), [] {
                        return g_client.pending ||
                               !g_client.running.load(std::memory_order_acquire);
                    });
                    const bool pending = g_client.pending;
                    lock.unlock();
                    if (!g_client.running.load(std::memory_order_acquire)) break;
                    if (!pending) {
                        if (peerClosed(connected)) {
                            failed = true;
                            terminal_status = FT_ERR_IO;
                            terminal_message = "文件服务已断开";
                            break;
                        }
                        continue;
                    }
                    current = takePendingOperation();
                    operation_active = true;
                    OpOutcome outcome = runClientOperation(connected, current);
                    operation_active = false;
                    if (outcome.fatal) {
                        terminal_status = outcome.status;
                        terminal_message = outcome.message;
                        failed = outcome.status != FT_ERR_CANCELLED &&
                                 g_client.running.load(std::memory_order_acquire);
                        cancelled = !failed;
                        break;
                    }
                }
                if (!g_client.running.load(std::memory_order_acquire)) {
                    cancelled = true;
                    failed = false;
                    if (terminal_message.empty()) terminal_message = "已取消";
                    if (terminal_status == FT_OK) terminal_status = FT_ERR_CANCELLED;
                }
                }
            }
        }
    } catch (...) {
        failed = g_client.running.load(std::memory_order_acquire);
        cancelled = !failed;
        terminal_status = failed ? FT_ERR_IO : FT_ERR_CANCELLED;
        terminal_message = failed ? "文件传输内部错误" : "已取消";
    }

    g_client.running.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(g_client.socket_mutex);
        g_client.socket.store((intptr_t)kInvalidSock,
                              std::memory_order_release);
    }
    socket.reset();

    if (operation_active) {
        FtStatus status = failed ? FT_ERR_IO : FT_ERR_CANCELLED;
        finishOperation(current, status, 0, 0, std::string(),
                        failed ? "文件传输内部错误" : "已取消", true);
    }
    if (hasPendingOperation()) {
        ClientOp pending = takePendingOperation();
        FtStatus status = cancelled ? FT_ERR_CANCELLED :
                          (terminal_status == FT_OK ? FT_ERR_IO : terminal_status);
        finishOperation(pending, status, 0, 0, std::string(),
                        cancelled ? "已取消" : terminal_message, true);
    }
    g_client.busy.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(g_client.password_mutex);
        g_client.password_waiting = false;
        wipeString(g_client.password);
    }
    g_client.active.store(false, std::memory_order_release);
    if (cancelled)
        clientState(FT_CLIENT_DISCONNECTED,
                    terminal_message.empty() ? "已取消" : terminal_message,
                    FT_ERR_CANCELLED);
    else if (failed)
        clientState(FT_CLIENT_ERROR,
                    terminal_message.empty() ? "文件传输连接失败" : terminal_message,
                    terminal_status);
    else
        clientState(FT_CLIENT_DISCONNECTED);
}

void serviceRunImpl(std::atomic<bool> &running, const FtTargetAuth *auth)
{
    reportStartup(0, 0, 0);
    fileStat("state=starting|port=" + std::to_string(FILEXFER_PORT));
    int startup_error = 0;
    if (!ensureSockets(&startup_error)) {
        reportStartup(-1, 1, startup_error);
        fileStat("state=start_failed|stage=socket|err=" +
                 std::to_string(startup_error));
        return;
    }
    Sock listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == kInvalidSock) {
        int e = sockError();
        reportStartup(-1, 1, e);
        fileStat("state=start_failed|stage=socket|err=" + std::to_string(e));
        return;
    }
    SockHolder listener_guard(listener);
#ifdef _WIN32
    BOOL exclusive = TRUE;
    if (setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                   (const char *)&exclusive, sizeof(exclusive)) != 0) {
        int e = sockError();
        reportStartup(-1, 1, e);
        fileStat("state=start_failed|stage=socket|err=" + std::to_string(e));
        return;
    }
#else
    int reuse = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)FILEXFER_PORT);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(listener, (sockaddr *)&address, sizeof(address)) != 0) {
        int e = sockError();
        reportStartup(-1, 3, e);
        fileStat("state=start_failed|stage=bind|err=" + std::to_string(e));
        return;
    }
    if (listen(listener, 4) != 0) {
        int e = sockError();
        reportStartup(-1, 4, e);
        fileStat("state=start_failed|stage=listen|err=" + std::to_string(e));
        return;
    }
    reportStartup(1, 4, 0);
    fileStat("state=listening|port=" + std::to_string(FILEXFER_PORT));

    std::atomic<bool> active{false};
    std::thread worker;
    while (running.load(std::memory_order_acquire)) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(listener, &rd);
        timeval tv{1, 0};
#ifdef _WIN32
        int selected = select(0, &rd, nullptr, nullptr, &tv);
#else
        int selected = select(listener + 1, &rd, nullptr, nullptr, &tv);
#endif
        if (selected < 0) {
            if (wouldBlock(sockError())) continue;
            break;
        }
        if (selected == 0) continue;
        sockaddr_in peer_address{};
#ifdef _WIN32
        int peer_size = sizeof(peer_address);
#else
        socklen_t peer_size = sizeof(peer_address);
#endif
        Sock client = accept(listener, (sockaddr *)&peer_address, &peer_size);
        if (client == kInvalidSock) continue;
        if (!setNonblocking(client)) {
            closeSock(client);
            continue;
        }
        char ip[INET_ADDRSTRLEN] = {};
        if (!inet_ntop(AF_INET, &peer_address.sin_addr, ip, sizeof(ip)))
            std::strncpy(ip, "unknown", sizeof(ip) - 1);

        if (active.load(std::memory_order_acquire)) {
            /* Read the hello first so a Windows close cannot RST away BUSY. */
            SockHolder busy(client);
            FilexferHello hello{};
            recvAll(client, &hello, sizeof(hello), running, 1000);
            sendFileReply(client, running, FT_ERR_BUSY);
            shutdownSock(client, kShutdownSend);
            fileStat("state=busy");
            continue;
        }
        if (worker.joinable()) worker.join();
        active.store(true, std::memory_order_release);
        try {
            worker = std::thread([client, peer = std::string(ip), &running,
                                  &active, auth] {
                try {
                    serveServiceClient(client, peer, auth, running);
                } catch (...) {
                    fileStat("state=error|reason=internal");
                }
                closeSock(client);
                active.store(false, std::memory_order_release);
                fileStat("state=disconnected");
            });
        } catch (...) {
            active.store(false, std::memory_order_release);
            closeSock(client);
            fileStat("state=error|reason=thread");
        }
    }
    listener_guard.reset();
    if (worker.joinable()) worker.join();
    fileStat("state=stopped");
}

} /* namespace */

/* ── public local browsing helpers ─────────────────────────────────────── */

bool ftLocalHome(std::string &path, std::string &error)
{
    return localHome(path, error);
}

bool ftLocalList(const std::string &path, FtListing &out, std::string &error)
{
    return listLocal(path, out, error);
}

std::string ftLocalDeviceName()
{
    return localDeviceName();
}

bool ftLocalMkdir(const std::string &dir, const std::string &name,
                  std::string &path, std::string &error)
{
    path.clear();
    error.clear();
    if (!validLocalName(name)) {
        error = "文件夹名称无效";
        return false;
    }
    DirLock lock;
    FtStatus status = lock.open(dir, true, true);
    if (status == FT_OK)
        status = createDirectoryLocked(lock, name, path);
    if (status == FT_OK) return true;
    switch (status) {
    case FT_ERR_EXISTS:    error = "同名文件夹已存在"; break;
    case FT_ERR_NOT_FOUND: error = "目录不存在"; break;
    case FT_ERR_PATH:      error = "目录路径无效"; break;
    case FT_ERR_LIMIT:     error = "名称超出长度限制"; break;
    default:               error = "无法创建文件夹"; break;
    }
    return false;
}

std::string ftPathParent(const std::string &path)
{
    if (path.empty() || driveRootName(path)) return std::string();
    /* A POSIX remote path must keep '/' even when this app runs on Windows:
     * std::filesystem would emit the host's native '\\' and a Linux/macOS
     * receiver treats that as part of the component name. */
    if (path[0] == '/' && !(path.size() >= 2 && path[1] == '/')) {
        std::string p = path;
        while (p.size() > 1 && p.back() == '/') p.pop_back();
        if (p.size() <= 1) return std::string();
        const size_t separator = p.find_last_of('/');
        if (separator == 0) return std::string("/");
        return p.substr(0, separator);
    }
    try {
        if (windowsStylePath(path)) {
            std::string p = path;
            while (p.size() > 3 && (p.back() == '/' || p.back() == '\\'))
                p.pop_back();
            if (driveRootName(p.substr(0, 2)) && p.size() <= 3)
                return std::string();
            if (p.size() >= 2 && p[0] == '\\' && p[1] == '\\') {
                const size_t server_end = p.find_first_of("/\\", 2);
                if (server_end == std::string::npos) return std::string();
                const size_t share_end = p.find_first_of("/\\", server_end + 1);
                if (share_end == std::string::npos) return std::string();
            }
            const size_t separator = p.find_last_of("/\\");
            if (separator == std::string::npos) return std::string();
            if (driveRootName(p.substr(0, 2)) && separator < 3)
                return p.substr(0, 3);
            return p.substr(0, separator);
        }
        fs::path p = fs::u8path(path).lexically_normal();
        while (p != p.root_path() && p.filename().empty()) p = p.parent_path();
        if (p == p.root_path()) return std::string();
        fs::path parent = p.parent_path();
        if (parent == p) return std::string();
        return parent.u8string();
    } catch (...) {
        return std::string();
    }
}

std::string ftPathJoin(const std::string &dir, const std::string &name)
{
    if (dir.empty()) {
        if (driveRootName(name)) return name + "\\";
        return name;
    }
    if (name.empty()) return dir;
    if (!validWireName(name)) return std::string();
    /* Keep the remote's separator style. A Windows app talking to a POSIX
     * receiver must send "/home/user/sub", never "/home/user\\sub". */
    if (dir[0] == '/' && !(dir.size() >= 2 && dir[1] == '/')) {
        if (name[0] == '/' || name[0] == '\\') return std::string();
        const bool separator = dir.back() == '/';
        return dir + (separator ? "" : "/") + name;
    }
    try {
        if (driveRootName(dir)) return dir + "\\" + name;
        if (windowsStylePath(dir)) {
            if (wireAbsolute(name) || name[0] == '/' || name[0] == '\\')
                return std::string();
            const bool separator = dir.back() == '/' || dir.back() == '\\';
            return dir + (separator ? "" : "\\") + name;
        }
        fs::path child = fs::u8path(name);
        if (child.is_absolute()) return std::string();
        return (fs::u8path(dir) / child).u8string();
    } catch (...) {
        return std::string();
    }
}

std::string ftPathName(const std::string &path)
{
    if (path.empty()) return std::string();
    /* Same separator rule as ftPathJoin/ftPathParent (cross-OS remotes). */
    if (path[0] == '/' && !(path.size() >= 2 && path[1] == '/')) {
        std::string p = path;
        while (p.size() > 1 && p.back() == '/') p.pop_back();
        if (p.size() <= 1) return std::string("/");
        const size_t separator = p.find_last_of('/');
        return separator == std::string::npos ? p : p.substr(separator + 1);
    }
    try {
        if (windowsStylePath(path)) {
            std::string p = path;
            while (p.size() > 3 && (p.back() == '/' || p.back() == '\\'))
                p.pop_back();
            if (driveRootName(p.substr(0, 2)) && p.size() <= 3) return p;
            if (p.size() >= 2 && p[0] == '\\' && p[1] == '\\') {
                const size_t server_end = p.find_first_of("/\\", 2);
                if (server_end == std::string::npos) return p;
                if (p.find_first_of("/\\", server_end + 1) == std::string::npos)
                    return p;
            }
            const size_t separator = p.find_last_of("/\\");
            return separator == std::string::npos ? p : p.substr(separator + 1);
        }
        fs::path p = fs::u8path(path).lexically_normal();
        while (p != p.root_path() && p.filename().empty()) p = p.parent_path();
        if (p == p.root_path()) return p.u8string();
        return p.filename().u8string();
    } catch (...) {
        return std::string();
    }
}

/* ── public service API ────────────────────────────────────────────────── */

FtStartupStatus ftServiceStatus()
{
    FtStartupStatus status;
    status.state = g_service_state.load(std::memory_order_acquire);
    status.stage = g_service_stage.load(std::memory_order_relaxed);
    status.error_code = g_service_error.load(std::memory_order_relaxed);
    status.port = (uint16_t)FILEXFER_PORT;
    return status;
}

void ftServiceRun(std::atomic<bool> &running, const FtTargetAuth *auth)
{
    serviceRunImpl(running, auth);
}

void ftSetStatSink(void (*sink)(const char *line))
{
    std::lock_guard<std::mutex> lock(g_stat_mutex);
    g_stat_sink = sink;
}

/* ── public asynchronous client API ────────────────────────────────────── */

bool ftClientStart(const std::string &ip, uint16_t port)
{
    if (ip.empty() || ip.size() > 255 || ip.find('\0') != std::string::npos ||
        !port || !numericHost(ip) || !ensureSockets())
        return false;
    std::lock_guard<std::mutex> lock(g_client.lifetime);
    if (g_client.active.load(std::memory_order_acquire)) return false;
    if (g_client.worker.joinable()) {
        if (g_client.worker.get_id() == std::this_thread::get_id()) return false;
        signalStop();
        g_client.worker.join();
    }
    signalStop();
    {
        std::lock_guard<std::mutex> sock_lock(g_client.socket_mutex);
        g_client.socket.store((intptr_t)kInvalidSock,
                              std::memory_order_release);
    }
    {
        std::lock_guard<std::mutex> event_lock(g_client.event_mutex);
        g_client.events.clear();
    }
    {
        std::lock_guard<std::mutex> op_lock(g_client.op_mutex);
        g_client.pending = false;
        g_client.op = ClientOp{};
    }
    {
        std::lock_guard<std::mutex> password_lock(g_client.password_mutex);
        g_client.password_waiting = false;
        g_client.password_answered = false;
        g_client.password_cancelled = false;
        wipeString(g_client.password);
    }
    g_client.busy.store(false, std::memory_order_release);
    g_client.op_abort.store(false, std::memory_order_release);
    g_client.running.store(true, std::memory_order_release);
    g_client.active.store(true, std::memory_order_release);
    try {
        g_client.worker = std::thread(clientWorker, ip, port);
        return true;
    } catch (...) {
        g_client.active.store(false, std::memory_order_release);
        g_client.running.store(false, std::memory_order_release);
        return false;
    }
}

void ftClientStop()
{
    std::lock_guard<std::mutex> lock(g_client.lifetime);
    signalStop();
    if (g_client.worker.joinable()) {
        if (g_client.worker.get_id() == std::this_thread::get_id()) return;
        g_client.worker.join();
    }
    {
        std::lock_guard<std::mutex> sock_lock(g_client.socket_mutex);
        g_client.socket.store((intptr_t)kInvalidSock,
                              std::memory_order_release);
    }
    g_client.busy.store(false, std::memory_order_release);
    g_client.op_abort.store(false, std::memory_order_release);
    g_client.active.store(false, std::memory_order_release);
}

bool ftClientActive()
{
    return g_client.active.load(std::memory_order_acquire);
}

bool ftClientBusy()
{
    return g_client.busy.load(std::memory_order_acquire);
}

bool ftClientListHome()
{
    ClientOp op;
    op.kind = OpKind::Home;
    return queueOperation(std::move(op));
}

bool ftClientList(const std::string &path)
{
    if (!validPathText(path, true)) return false;
    ClientOp op;
    op.kind = OpKind::List;
    op.first = path;
    return queueOperation(std::move(op));
}

bool ftClientRoots()
{
    ClientOp op;
    op.kind = OpKind::Roots;
    return queueOperation(std::move(op));
}

bool ftClientMkdir(const std::string &remote_dir, const std::string &name)
{
    if (remote_dir.empty() || !validPathText(remote_dir, false) ||
        !validWireName(name))
        return false;
    ClientOp op;
    op.kind = OpKind::Mkdir;
    op.first = remote_dir;
    op.second = name;
    return queueOperation(std::move(op));
}

bool ftClientUpload(const std::string &local_file, const std::string &remote_dir)
{
    if (local_file.empty() || !validUtf8(local_file) ||
        local_file.find('\0') != std::string::npos ||
        !validPathText(remote_dir, false))
        return false;
    ClientOp op;
    op.kind = OpKind::Upload;
    op.first = local_file;
    op.second = remote_dir;
    return queueOperation(std::move(op));
}

bool ftClientDownload(const std::string &remote_file, const std::string &local_dir)
{
    if (!validPathText(remote_file, false) || local_dir.empty() ||
        !validUtf8(local_dir) || local_dir.find('\0') != std::string::npos)
        return false;
    ClientOp op;
    op.kind = OpKind::Download;
    op.first = remote_file;
    op.second = local_dir;
    return queueOperation(std::move(op));
}

void ftClientCancel()
{
    signalStop();
}

void ftClientCancelOperation()
{
    g_client.op_abort.store(true, std::memory_order_release);
}

bool ftClientPoll(FtEvent &out)
{
    std::lock_guard<std::mutex> lock(g_client.event_mutex);
    if (g_client.events.empty()) return false;
    out = std::move(g_client.events.front());
    g_client.events.pop_front();
    return true;
}

void ftClientPassword(const std::string &password)
{
    {
        std::lock_guard<std::mutex> lock(g_client.password_mutex);
        if (!g_client.password_waiting || g_client.password_answered ||
            g_client.password_cancelled)
            return;
        g_client.password_answered = true;
        if (password.empty()) {
            g_client.password_cancelled = true;
        } else {
            try {
                g_client.password = password;
            } catch (...) {
                g_client.password_cancelled = true;
            }
        }
    }
    if (password.empty()) signalStop();
    else g_client.password_cv.notify_one();
}

const char *ftStatusText(FtStatus status)
{
    switch (status) {
    case FT_OK: return "成功";
    case FT_ERR_VERSION: return "协议版本不兼容";
    case FT_ERR_AUTH: return "授权失败";
    case FT_ERR_BUSY: return "对方正忙";
    case FT_ERR_DISABLED: return "对端未允许文件传输，请在其设置中开启";
    case FT_ERR_LIMIT: return "名称或大小超出限制";
    case FT_ERR_PATH: return "路径无效";
    case FT_ERR_IO: return "读写或连接失败";
    case FT_ERR_CANCELLED: return "已取消";
    case FT_ERR_UNSUPPORTED: return "不支持此操作";
    case FT_ERR_NOT_FOUND: return "文件或目录不存在";
    case FT_ERR_DISK: return "磁盘空间不足";
    case FT_ERR_EXISTS: return "目标文件已存在";
    case FT_ERR_PROTOCOL: return "协议数据无效";
    default: return "未知错误";
    }
}

