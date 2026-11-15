/**
 * DROP.CPP – 8085 file-drop channel (see drop.h).
 *
 * Wire format (all integers network byte order):
 *
 *   DropHello (72 B)  magic 'MVD1', version 1, token[32], transfer id,
 *                     release point (target screen px), item count, total
 *                     bytes, flags, reserved.
 *   DropReply (16 B)  magic, version, status, reserved.
 *   frame (16 B)      magic, version u16, type u16, payload length, reserved
 *                     + payload.
 *
 *   client → server : MANIFEST, FILE_BEGIN, FILE_DATA*, FILE_END, DONE
 *   server → client : RESULT
 *
 * The target writes into a private staging directory, verifies each file's
 * SHA-256, then copies the top-level items into the folder under the release
 * point (Windows: the Explorer/desktop folder; Linux/X11: the XDND-aware
 * window, falling back to the default receive directory when there is none).
 * Nothing is overwritten and the source is never modified.
 */
#include "drop.h"
#include "auth.h"
#include "dropshell.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#  include <shlobj.h>
#else
#  include <arpa/inet.h>
#  include <dirent.h>
#  include <fcntl.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <poll.h>
#  include <sys/select.h>
#  include <sys/socket.h>
#  include <sys/statvfs.h>
#  include <sys/stat.h>
#  include <sys/time.h>
#  include <sys/types.h>
#  include <unistd.h>
#  ifndef MSG_NOSIGNAL
#    define MSG_NOSIGNAL 0
#  endif
#  if defined(__linux__)
#    include <X11/Xatom.h>
#    include <X11/Xlib.h>
#    include <X11/Xutil.h>
#  endif
#endif

namespace {

void (*g_stat_sink)(const char *) = nullptr;
std::mutex g_stat_mtx;

void dropStat(const std::string &line)
{
    if (!g_stat_sink) return;
    std::lock_guard<std::mutex> lk(g_stat_mtx);
    g_stat_sink(line.c_str());
}

std::string statSafe(const std::string &s)
{
    std::string out;
    out.reserve(std::min<size_t>(s.size(), 200));
    for (unsigned char c : s) {
        if (out.size() >= 200) break;
        out.push_back((c < 0x20 || c == 0x7f || c == '|') ? ' ' : (char)c);
    }
    return out;
}

} /* namespace */

void dropSetStatSink(void (*sink)(const char *line))
{
    std::lock_guard<std::mutex> lk(g_stat_mtx);
    g_stat_sink = sink;
}

#ifdef _WIN32

namespace {

using drop_sock_t = SOCKET;
const drop_sock_t DROP_INVALID = INVALID_SOCKET;
const uint32_t DROP_FRAME_MAGIC = DROP_MAGIC;

enum DropFrameType : uint16_t {
    DF_MANIFEST   = 1,
    DF_FILE_BEGIN = 2,
    DF_FILE_DATA  = 3,
    DF_FILE_END   = 4,
    DF_DONE       = 5,
    DF_RESULT     = 6,
    DF_ABORT      = 7
};

#pragma pack(push, 1)
struct DropHello {
    uint32_t magic;
    uint32_t version;
    uint8_t  token[DROP_TOKEN_BYTES];
    uint32_t transfer_id;
    int32_t  x;
    int32_t  y;
    uint32_t item_count;
    uint64_t total_bytes;
    uint32_t flags;
    uint32_t reserved;
};

struct DropReply {
    uint32_t magic;
    uint32_t version;
    uint32_t status;
    uint32_t reserved;
};

struct DropFrame {
    uint32_t magic;
    uint16_t version;
    uint16_t type;
    uint32_t length;
    uint32_t reserved;
};
#pragma pack(pop)

static_assert(sizeof(DropHello) == 72, "DropHello must be 72 bytes");
static_assert(sizeof(DropReply) == 16, "DropReply must be 16 bytes");
static_assert(sizeof(DropFrame) == 16, "DropFrame must be 16 bytes");

/* ── byte order / buffers ────────────────────────────────────────────────── */

uint64_t hton64(uint64_t v)
{
    return ((uint64_t)htonl((uint32_t)(v >> 32))) |
           ((uint64_t)htonl((uint32_t)v) << 32);
}

uint64_t ntoh64(uint64_t v)
{
    return ((uint64_t)ntohl((uint32_t)(v >> 32))) |
           ((uint64_t)ntohl((uint32_t)v) << 32);
}

void appendU32(std::vector<uint8_t> &v, uint32_t x)
{
    x = htonl(x);
    const uint8_t *p = (const uint8_t *)&x;
    v.insert(v.end(), p, p + 4);
}

void appendU64(std::vector<uint8_t> &v, uint64_t x)
{
    x = hton64(x);
    const uint8_t *p = (const uint8_t *)&x;
    v.insert(v.end(), p, p + 8);
}

struct ByteCursor {
    const uint8_t *p;
    size_t n;
    bool ok = true;

    bool need(size_t k)
    {
        if (!ok || k > n) { ok = false; return false; }
        return true;
    }
    uint8_t u8()
    {
        if (!need(1)) return 0;
        uint8_t v = *p; p++; n--; return v;
    }
    uint32_t u32()
    {
        if (!need(4)) return 0;
        uint32_t v;
        std::memcpy(&v, p, 4);
        p += 4; n -= 4;
        return ntohl(v);
    }
    uint64_t u64()
    {
        if (!need(8)) return 0;
        uint64_t v;
        std::memcpy(&v, p, 8);
        p += 8; n -= 8;
        return ntoh64(v);
    }
    std::string bytes(size_t k)
    {
        if (!need(k)) return std::string();
        std::string s((const char *)p, k);
        p += k; n -= k;
        return s;
    }
};

/* ── sockets ─────────────────────────────────────────────────────────────── */

bool connectSocket(drop_sock_t &out, const char *ip, int port,
                   const std::atomic<bool> &running, int ms)
{
    drop_sock_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == DROP_INVALID) return false;
    u_long nonblocking = 1;
    ioctlsocket(s, FIONBIO, &nonblocking);

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        closesocket(s);
        return false;
    }
    connect(s, (sockaddr *)&addr, sizeof(addr));
    if (WSAGetLastError() != WSAEWOULDBLOCK) {
        closesocket(s);
        return false;
    }
    const DWORD start = GetTickCount();
    for (;;) {
        if (!running.load()) { closesocket(s); return false; }
        if ((int)(GetTickCount() - start) > ms) { closesocket(s); return false; }
        fd_set wr, ex;
        FD_ZERO(&wr); FD_ZERO(&ex);
        FD_SET(s, &wr); FD_SET(s, &ex);
        timeval tv = {0, 100000};
        int sel = select(0, nullptr, &wr, &ex, &tv);
        if (sel < 0) { closesocket(s); return false; }
        if (sel == 0) continue;
        int err = 0;
        int len = sizeof(err);
        if (getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &len) != 0 ||
            err != 0) {
            closesocket(s);
            return false;
        }
        u_long blocking = 0;
        ioctlsocket(s, FIONBIO, &blocking);
        out = s;
        return true;
    }
}

void setTimeouts(drop_sock_t s, DWORD recv_ms, DWORD send_ms)
{
    if (recv_ms) setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
                            (const char *)&recv_ms, sizeof(recv_ms));
    if (send_ms) setsockopt(s, SOL_SOCKET, SO_SNDTIMEO,
                            (const char *)&send_ms, sizeof(send_ms));
}

void enableKeepAlive(drop_sock_t s)
{
    BOOL on = TRUE;
    setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, (const char *)&on, sizeof(on));
}

bool sendAll(drop_sock_t s, const void *buf, size_t len)
{
    const char *p = (const char *)buf;
    size_t off = 0;
    while (off < len) {
        int n = send(s, p + off, (int)(len - off), 0);
        if (n <= 0) return false;
        off += (size_t)n;
    }
    return true;
}

/* 1 = ok, 0 = deadline, -1 = error/stop. */
int recvAll(drop_sock_t s, void *buf, size_t len,
            const std::atomic<bool> &running, int timeout_ms)
{
    char *p = (char *)buf;
    size_t off = 0;
    const DWORD start = GetTickCount();
    while (off < len) {
        if (!running.load()) return -1;
        if (timeout_ms > 0 && (int)(GetTickCount() - start) > timeout_ms)
            return 0;
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(s, &rd);
        timeval tv = {0, 200000};
        int sel = select(0, &rd, nullptr, nullptr, &tv);
        if (sel < 0) return -1;
        if (sel == 0) continue;
        int n = recv(s, p + off, (int)(len - off), 0);
        if (n <= 0) return -1;
        off += (size_t)n;
    }
    return 1;
}

bool sendFrame(drop_sock_t s, uint16_t type, const void *payload, uint32_t len)
{
    DropFrame h = {};
    h.magic = htonl(DROP_FRAME_MAGIC);
    h.version = htons((uint16_t)DROP_VERSION);
    h.type = htons(type);
    h.length = htonl(len);
    if (!sendAll(s, &h, sizeof(h))) return false;
    return len == 0 || sendAll(s, payload, len);
}

bool recvFrame(drop_sock_t s, DropFrame &h, std::vector<uint8_t> &payload,
               const std::atomic<bool> &running, int timeout_ms)
{
    payload.clear();
    if (recvAll(s, &h, sizeof(h), running, timeout_ms) != 1) return false;
    if (ntohl(h.magic) != DROP_FRAME_MAGIC ||
        ntohs(h.version) != (uint16_t)DROP_VERSION)
        return false;
    const uint32_t len = ntohl(h.length);
    if (len > 4u * 1024u * 1024u) return false;
    payload.resize(len);
    return len == 0 || recvAll(s, payload.data(), len, running, timeout_ms) == 1;
}

/* ── token gate ──────────────────────────────────────────────────────────── */

std::mutex g_gate_m;
uint8_t  g_gate_token[DROP_TOKEN_BYTES] = {};
uint64_t g_gate_epoch = 0;
bool     g_gate_active = false;
uint64_t g_gate_bytes = 0;   /* per-session budget, reset on install */

bool gateReserve(uint64_t bytes)
{
    std::lock_guard<std::mutex> lk(g_gate_m);
    if (!g_gate_active) return false;
    const uint64_t cap = 4ull * 1024ull * 1024ull * 1024ull;
    if (bytes > cap || g_gate_bytes + bytes > cap) return false;
    g_gate_bytes += bytes;
    return true;
}

/* Refund the unused part of a reservation. Token-checked so a worker from an
 * old session cannot refund (or underflow) the new session's budget. */
void gateRelease(const uint8_t token[DROP_TOKEN_BYTES], uint64_t bytes)
{
    std::lock_guard<std::mutex> lk(g_gate_m);
    if (!g_gate_active ||
        !mivaraConstEq(g_gate_token, token, DROP_TOKEN_BYTES))
        return;
    g_gate_bytes = (bytes >= g_gate_bytes) ? 0 : g_gate_bytes - bytes;
}

/* ── service state ───────────────────────────────────────────────────────── */

std::atomic<int> g_drop_start_state{0};
std::atomic<int> g_drop_start_stage{0};
std::atomic<int> g_drop_start_error{0};
std::atomic<uint16_t> g_drop_start_port{(uint16_t)DROP_PORT};

void reportStartup(int state, int stage, int error)
{
    g_drop_start_error.store(error);
    g_drop_start_stage.store(stage);
    g_drop_start_state.store(state);
}

/* ── paths ───────────────────────────────────────────────────────────────── */

std::string wideToUtf8(const std::wstring &w)
{
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n,
                        nullptr, nullptr);
    return s;
}

std::wstring utf8ToWide(const std::string &s)
{
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
                                (int)s.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(),
                        &w[0], n);
    return w;
}

std::wstring joinPath(const std::wstring &a, const std::wstring &b)
{
    if (a.empty()) return b;
    if (b.empty()) return a;
    if (a.back() == L'\\' || a.back() == L'/') return a + b;
    return a + L"\\" + b;
}

/* Windows paths compare case-insensitively. Used only to reject duplicate
 * manifest/selection entries before anything is written. */
std::string foldRel(const std::string &rel)
{
    std::wstring w = utf8ToWide(rel);
    if (w.empty()) return rel;
    CharLowerW(&w[0]);
    return wideToUtf8(w);
}

bool isReservedName(const std::string &component)
{
    std::string base = component;
    size_t dot = base.find('.');
    if (dot != std::string::npos) base.resize(dot);
    for (char &c : base)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    if (base == "con" || base == "prn" || base == "aux" || base == "nul")
        return true;
    if (base.size() == 4 &&
        (base.compare(0, 3, "com") == 0 || base.compare(0, 3, "lpt") == 0) &&
        base[3] >= '1' && base[3] <= '9')
        return true;
    return false;
}

bool validRelPath(const std::string &rel, std::string &why)
{
    if (rel.empty() || rel.size() > DROP_MAX_PATH_BYTES) {
        why = "empty or overlong path";
        return false;
    }
    if (rel.front() == '/' || rel.front() == '\\') {
        why = "absolute path";
        return false;
    }
    if (rel.find('\0') != std::string::npos) {
        why = "NUL in path";
        return false;
    }
    unsigned depth = 0;
    size_t start = 0;
    for (;;) {
        const size_t slash = rel.find('/', start);
        const std::string comp = rel.substr(
            start, slash == std::string::npos ? std::string::npos
                                              : slash - start);
        if (comp.empty() || comp == "." || comp == "..") {
            why = "empty, dot or dot-dot component";
            return false;
        }
        if (comp.back() == '.' || comp.back() == ' ') {
            why = "component ends with dot or space";
            return false;
        }
        if (comp.find('\\') != std::string::npos ||
            comp.find(':') != std::string::npos) {
            why = "separator or stream in component";
            return false;
        }
        for (unsigned char c : comp) {
            if (c < 0x20 || c == 0x7f) {
                why = "control character in path";
                return false;
            }
        }
        if (isReservedName(comp)) {
            why = "reserved device name";
            return false;
        }
        depth++;
        if (depth > DROP_MAX_DEPTH) {
            why = "path too deep";
            return false;
        }
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return true;
}

bool ensureDir(const std::wstring &dir)
{
    if (CreateDirectoryW(dir.c_str(), nullptr)) return true;
    if (GetLastError() != ERROR_ALREADY_EXISTS) return false;
    const DWORD a = GetFileAttributesW(dir.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) &&
           !(a & FILE_ATTRIBUTE_REPARSE_POINT);
}

bool createRelDirs(const std::wstring &root, const std::string &rel,
                   bool include_last, std::wstring &full)
{
    std::wstring cur = root;
    size_t start = 0;
    for (;;) {
        const size_t slash = rel.find('/', start);
        const bool last = (slash == std::string::npos);
        if (last && !include_last) {
            const std::wstring leaf = utf8ToWide(rel.substr(start));
            if (leaf.empty()) return false;
            full = joinPath(cur, leaf);
            return true;
        }
        const std::string comp = rel.substr(
            start, last ? std::string::npos : slash - start);
        const std::wstring w = utf8ToWide(comp);
        if (w.empty()) return false;
        cur = joinPath(cur, w);
        if (!ensureDir(cur)) return false;
        if (last) { full = cur; return true; }
        start = slash + 1;
    }
}

void deleteTree(const std::wstring &dir)
{
    /* A relative/empty path would make joinPath("", "*") enumerate the
     * process CWD and delete its contents. Only absolute drive paths may be
     * removed; staging/cleanup never needs a UNC or relative target. */
    if (dir.size() < 3 || dir[1] != L':')
        return;
    const std::wstring pat = joinPath(dir, L"*");
    WIN32_FIND_DATAW fd = {};
    HANDLE h = FindFirstFileW(pat.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(fd.cFileName, L".") == 0 ||
                wcscmp(fd.cFileName, L"..") == 0)
                continue;
            const std::wstring child = joinPath(dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                    deleteTree(child);
                else
                    RemoveDirectoryW(child.c_str());
            } else {
                DeleteFileW(child.c_str());
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

/* No transfer can be active before the listener starts, so every subdirectory
 * of Drop\ is a crash/exit residue. Remove it once per receiver start. */
void cleanupStaging()
{
    const std::wstring base = dropStagingBase();
    if (base.empty()) return;
    const std::wstring pattern = joinPath(base, L"*");
    WIN32_FIND_DATAW fd = {};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 ||
            wcscmp(fd.cFileName, L"..") == 0)
            continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            deleteTree(joinPath(base, fd.cFileName));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

bool enoughSpace(const std::wstring &dir, uint64_t need)
{
    ULARGE_INTEGER freeBytes = {};
    if (!GetDiskFreeSpaceExW(dir.c_str(), &freeBytes, nullptr, nullptr))
        return false;
    const uint64_t reserve = 64ull * 1024ull * 1024ull;
    return freeBytes.QuadPart > need && freeBytes.QuadPart - need > reserve;
}

/* ── source scan ─────────────────────────────────────────────────────────── */

struct Entry {
    std::string rel;
    std::wstring abs;
    bool dir = false;
    uint64_t size = 0;
    uint64_t mtime = 0;
};

struct ScanState {
    std::vector<Entry> entries;
    uint64_t total = 0;
    uint32_t max_items = DROP_MAX_ITEMS;
    uint64_t max_bytes = DROP_MAX_TOTAL_BYTES;
    std::string err;
};

/* Compare the open handle with the scan-time size/mtime. The transfer's
 * SHA-256 proves what was read; this proves the source did not change between
 * the scan/open and the end of the read. */
bool fileUnchanged(HANDLE f, const Entry &e)
{
    BY_HANDLE_FILE_INFORMATION fi = {};
    if (!GetFileInformationByHandle(f, &fi)) return false;
    const uint64_t mt = ((uint64_t)fi.ftLastWriteTime.dwHighDateTime << 32) |
                        (uint64_t)fi.ftLastWriteTime.dwLowDateTime;
    const uint64_t sz = ((uint64_t)fi.nFileSizeHigh << 32) | fi.nFileSizeLow;
    return sz == e.size && mt == e.mtime;
}

std::wstring baseName(const std::wstring &p)
{
    size_t end = p.size();
    while (end > 0 && (p[end - 1] == L'\\' || p[end - 1] == L'/')) end--;
    if (end == 0) return std::wstring();
    const size_t b = p.find_last_of(L"\\/", end - 1);
    return p.substr(b == std::wstring::npos ? 0 : b + 1,
                    end - (b == std::wstring::npos ? 0 : b + 1));
}

bool scanOne(const std::wstring &abs, const std::string &rel, unsigned depth,
             ScanState &st)
{
    WIN32_FILE_ATTRIBUTE_DATA fad = {};
    if (!GetFileAttributesExW(abs.c_str(), GetFileExInfoStandard, &fad)) {
        st.err = "cannot stat " + wideToUtf8(abs);
        return false;
    }
    if (fad.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        st.err = "reparse point is not supported: " + wideToUtf8(abs);
        return false;
    }
    Entry e;
    e.rel = rel;
    e.abs = abs;
    e.dir = (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    e.mtime = ((uint64_t)fad.ftLastWriteTime.dwHighDateTime << 32) |
              (uint64_t)fad.ftLastWriteTime.dwLowDateTime;
    if (!e.dir) {
        e.size = ((uint64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
        if (e.size > DROP_MAX_FILE_BYTES) {
            st.err = "single file over the limit";
            return false;
        }
        st.total += e.size;
        if (st.total > st.max_bytes) {
            st.err = "selection over the total size limit";
            return false;
        }
    }
    if (st.entries.size() >= st.max_items) {
        st.err = "selection has too many items";
        return false;
    }
    st.entries.push_back(e);
    if (!e.dir) return true;

    if (depth >= DROP_MAX_DEPTH) {
        st.err = "directory tree too deep";
        return false;
    }
    const std::wstring pattern = joinPath(abs, L"*");
    WIN32_FIND_DATAW fd = {};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        st.err = "cannot enumerate " + wideToUtf8(abs);
        return false;
    }
    bool ok = true;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 ||
            wcscmp(fd.cFileName, L"..") == 0)
            continue;
        const std::wstring childAbs = joinPath(abs, fd.cFileName);
        const std::string name = wideToUtf8(fd.cFileName);
        if (name.empty()) { ok = false; st.err = "invalid filename"; break; }
        const std::string childRel = rel + "/" + name;
        std::string why;
        if (!validRelPath(childRel, why)) {
            ok = false;
            st.err = "invalid path: " + why;
            break;
        }
        if (!scanOne(childAbs, childRel, depth + 1, st)) { ok = false; break; }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}

bool scanPaths(const std::vector<std::string> &paths, ScanState &st)
{
    std::unordered_set<std::string> roots;
    for (const std::string &p : paths) {
        const std::wstring w = utf8ToWide(p);
        if (w.empty()) { st.err = "invalid source path"; return false; }
        if (w.size() < 3 || w[1] != L':' ||
            (w[2] != L'\\' && w[2] != L'/')) {
            st.err = "source path is not absolute: " + p;
            return false;
        }
        const std::wstring name = baseName(w);
        if (name.empty()) { st.err = "cannot derive source name"; return false; }
        const std::string rel = wideToUtf8(name);
        std::string why;
        if (!validRelPath(rel, why)) {
            st.err = "invalid source name: " + why;
            return false;
        }
        if (!roots.insert(foldRel(rel)).second) {
            st.err = "two selected items share the same name: " + rel;
            return false;
        }
        if (!scanOne(w, rel, 0, st)) return false;
    }
    return true;
}

/* ── manifest ────────────────────────────────────────────────────────────── */

bool sendManifest(drop_sock_t s, const std::vector<Entry> &entries)
{
    std::vector<uint8_t> payload;
    payload.reserve(4096);
    appendU32(payload, (uint32_t)entries.size());
    for (const Entry &e : entries) {
        payload.push_back(e.dir ? 1 : 0);
        payload.push_back(0); payload.push_back(0); payload.push_back(0);
        appendU32(payload, (uint32_t)e.rel.size());
        appendU64(payload, e.size);
        appendU64(payload, e.mtime);
        payload.insert(payload.end(), e.rel.begin(), e.rel.end());
    }
    if (payload.size() > 4u * 1024u * 1024u) return false;
    return sendFrame(s, DF_MANIFEST, payload.data(), (uint32_t)payload.size());
}

/* ── receiver ────────────────────────────────────────────────────────────── */

struct ManifestEntry {
    std::string rel;
    bool dir = false;
    uint64_t size = 0;
    uint64_t mtime = 0;
};

bool parseManifest(const std::vector<uint8_t> &payload,
                   std::vector<ManifestEntry> &out, std::string &why,
                   uint64_t &total, uint32_t max_items, uint64_t max_bytes)
{
    ByteCursor c{payload.data(), payload.size()};
    const uint32_t count = c.u32();
    if (!c.ok || count > max_items) { why = "invalid item count"; return false; }
    out.clear();
    out.reserve(count);
    total = 0;
    std::unordered_set<std::string> seen;
    for (uint32_t i = 0; i < count; i++) {
        ManifestEntry e;
        e.dir = c.u8() != 0;
        c.u8(); c.u8(); c.u8();
        const uint32_t n = c.u32();
        e.size = c.u64();
        e.mtime = c.u64();
        if (!c.ok || n == 0 || n > DROP_MAX_PATH_BYTES) {
            why = "invalid manifest entry";
            return false;
        }
        e.rel = c.bytes(n);
        if (!c.ok) { why = "truncated manifest"; return false; }
        if (!validRelPath(e.rel, why)) return false;
        if (!seen.insert(foldRel(e.rel)).second) {
            why = "duplicate manifest path";
            return false;
        }
        if (!e.dir) {
            if (e.size > DROP_MAX_FILE_BYTES) {
                why = "single file over the limit";
                return false;
            }
            total += e.size;
            if (total > max_bytes) { why = "transfer over the limit"; return false; }
        }
        out.push_back(std::move(e));
    }
    if (!c.ok) { why = "truncated manifest"; return false; }
    return true;
}

bool sendResult(drop_sock_t s, uint32_t status, uint32_t committed,
                uint32_t failed, bool fallback, const std::string &msg)
{
    std::vector<uint8_t> payload;
    appendU32(payload, status);
    appendU32(payload, committed);
    appendU32(payload, failed);
    appendU32(payload, fallback ? 1u : 0u);
    appendU32(payload, (uint32_t)msg.size());
    payload.insert(payload.end(), msg.begin(), msg.end());
    return sendFrame(s, DF_RESULT, payload.data(), (uint32_t)payload.size());
}

bool sendAbort(drop_sock_t s, uint32_t status, const std::string &msg)
{
    std::vector<uint8_t> payload;
    appendU32(payload, status);
    appendU32(payload, (uint32_t)msg.size());
    payload.insert(payload.end(), msg.begin(), msg.end());
    return sendFrame(s, DF_ABORT, payload.data(), (uint32_t)payload.size());
}

uint32_t receiveFrames(drop_sock_t s, const DropHello &hello,
                       const std::vector<ManifestEntry> &man,
                       const std::wstring &staging,
                       const std::atomic<bool> &running,
                       std::string &why, uint64_t &received)
{
    std::vector<HANDLE> open(man.size(), INVALID_HANDLE_VALUE);
    std::vector<MivaraSha256Ctx> hash(man.size());
    std::vector<uint64_t> expected(man.size(), 0);
    std::vector<std::wstring> part(man.size());
    std::vector<std::wstring> finalPath(man.size());
    int current = -1;
    uint32_t status = DROP_OK;
    uint32_t frames = 0;
    received = 0;

    for (size_t i = 0; i < man.size(); i++) {
        std::wstring full;
        if (man[i].dir) {
            if (!createRelDirs(staging, man[i].rel, true, full)) {
                why = "cannot create staging directory";
                status = DROP_ERR_PATH;
                goto cleanup;
            }
        } else {
            if (!createRelDirs(staging, man[i].rel, false, full)) {
                why = "cannot create staging path";
                status = DROP_ERR_PATH;
                goto cleanup;
            }
            finalPath[i] = full;
            part[i] = full + L".part";
        }
    }

    for (;;) {
        if (!running.load()) {
            status = DROP_ERR_CANCELLED;
            why = "session ended";
            goto cleanup;
        }
        DropFrame h = {};
        std::vector<uint8_t> payload;
        if (!recvFrame(s, h, payload, running, 30000)) {
            status = DROP_ERR_IO;
            why = "transfer timed out or disconnected";
            goto cleanup;
        }
        const uint16_t type = ntohs(h.type);
        ByteCursor c{payload.data(), payload.size()};

        /* The 8084 session may end mid-transfer; abort promptly instead of
         * streaming a folder into a dead session's staging area. */
        if (((++frames) & 63u) == 0 && !dropGateCheck(hello.token)) {
            status = DROP_ERR_AUTH;
            why = "session ended";
            goto cleanup;
        }

        if (type == DF_ABORT) {
            const uint32_t st = c.u32();
            const uint32_t n = c.u32();
            why = c.bytes(n);
            status = st ? st : DROP_ERR_IO;
            goto cleanup;
        }
        if (type == DF_DONE) {
            if (current >= 0) {
                status = DROP_ERR_IO;
                why = "file left open";
                goto cleanup;
            }
            break;
        }
        if (type == DF_FILE_BEGIN) {
            const uint32_t idx = c.u32();
            const uint64_t size = c.u64();
            if (!c.ok || idx >= man.size() || man[idx].dir ||
                size != man[idx].size || current >= 0) {
                status = DROP_ERR_PATH;
                why = "invalid file begin";
                goto cleanup;
            }
            if (!enoughSpace(staging, man[idx].size)) {
                status = DROP_ERR_DISK;
                why = "staging disk is full";
                goto cleanup;
            }
            if (open[idx] != INVALID_HANDLE_VALUE) CloseHandle(open[idx]);
            open[idx] = CreateFileW(part[idx].c_str(), GENERIC_WRITE, 0, nullptr,
                                    CREATE_NEW, FILE_ATTRIBUTE_NORMAL |
                                    FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            if (open[idx] == INVALID_HANDLE_VALUE) {
                status = DROP_ERR_IO;
                why = "cannot create staging file";
                goto cleanup;
            }
            mivaraSha256Init(&hash[idx]);
            expected[idx] = 0;
            current = (int)idx;
            continue;
        }
        if (type == DF_FILE_DATA) {
            const uint32_t idx = c.u32();
            const uint64_t off = c.u64();
            const size_t n = payload.size() >= 12 ? payload.size() - 12 : 0;
            if (!c.ok || (int)idx != current || off != expected[idx] ||
                n == 0 || n > 64u * 1024u) {
                status = DROP_ERR_IO;
                why = "invalid file data";
                goto cleanup;
            }
            if (received + n > hello.total_bytes ||
                received + n > DROP_MAX_TOTAL_BYTES) {
                status = DROP_ERR_LIMIT;
                why = "transfer exceeds declared size";
                goto cleanup;
            }
            const uint8_t *data = payload.data() + 12;
            DWORD wrote = 0;
            if (!WriteFile(open[idx], data, (DWORD)n, &wrote, nullptr) ||
                wrote != n) {
                status = DROP_ERR_IO;
                why = "write failed";
                goto cleanup;
            }
            mivaraSha256Update(&hash[idx], data, n);
            expected[idx] += n;
            received += n;
            continue;
        }
        if (type == DF_FILE_END) {
            const uint32_t idx = c.u32();
            if (!c.ok || (int)idx != current || c.n < 32) {
                status = DROP_ERR_IO;
                why = "invalid file end";
                goto cleanup;
            }
            uint8_t want[32] = {}, got[32] = {};
            std::memcpy(want, c.p, 32);
            mivaraSha256Final(&hash[idx], got);
            if (!mivaraConstEq(want, got, 32) || expected[idx] != man[idx].size) {
                status = DROP_ERR_IO;
                why = "checksum mismatch";
                goto cleanup;
            }
            FILETIME ft;
            ft.dwLowDateTime = (DWORD)(man[idx].mtime & 0xFFFFFFFFull);
            ft.dwHighDateTime = (DWORD)(man[idx].mtime >> 32);
            SetFileTime(open[idx], nullptr, nullptr, &ft);
            CloseHandle(open[idx]);
            open[idx] = INVALID_HANDLE_VALUE;
            if (!MoveFileExW(part[idx].c_str(), finalPath[idx].c_str(),
                             MOVEFILE_WRITE_THROUGH)) {
                status = DROP_ERR_IO;
                why = "cannot commit staged file";
                goto cleanup;
            }
            current = -1;
            continue;
        }
        status = DROP_ERR_IO;
        why = "unknown frame";
        goto cleanup;
    }

cleanup:
    for (HANDLE h : open)
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    return status;
}

void sendStatus(drop_sock_t s, uint32_t status)
{
    DropReply r = {};
    r.magic = htonl(DROP_MAGIC);
    r.version = htonl(DROP_VERSION);
    r.status = htonl(status);
    sendAll(s, &r, sizeof(r));
}

/* `pre` is the hello the listener already read and token-checked. Only one
 * authenticated transfer runs at a time; unauthenticated connections never
 * claim that slot (see dropServiceRun). */
void dropWorker(drop_sock_t s, const std::string &peer,
                const std::atomic<bool> &running, const DropHello &pre)
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        typedef void *(WINAPI *SetCtxFn)(void *);
        auto fn = (SetCtxFn)GetProcAddress(user32, "SetThreadDpiAwarenessContext");
        if (fn) fn((void *)(intptr_t)-4);
    }

    setTimeouts(s, 30000, 30000);
    enableKeepAlive(s);

    DropHello hello = pre;
    const uint32_t magic = ntohl(hello.magic);
    const uint32_t version = ntohl(hello.version);
    if (magic != DROP_MAGIC || version != DROP_VERSION) {
        DropReply r = {};
        r.magic = htonl(DROP_MAGIC);
        r.version = htonl(DROP_VERSION);
        r.status = htonl(DROP_ERR_VERSION);
        sendAll(s, &r, sizeof(r));
        closesocket(s);
        CoUninitialize();
        return;
    }
    if (!dropGateCheck(hello.token)) {
        DropReply r = {};
        r.magic = htonl(DROP_MAGIC);
        r.version = htonl(DROP_VERSION);
        r.status = htonl(DROP_ERR_AUTH);
        sendAll(s, &r, sizeof(r));
        closesocket(s);
        CoUninitialize();
        return;
    }

    const uint32_t item_count = ntohl(hello.item_count);
    const uint64_t total_bytes = ntoh64(hello.total_bytes);
    uint64_t received = 0;
    if (item_count > DROP_MAX_ITEMS || total_bytes > DROP_MAX_TOTAL_BYTES) {
        DropReply r = {};
        r.magic = htonl(DROP_MAGIC);
        r.version = htonl(DROP_VERSION);
        r.status = htonl(DROP_ERR_LIMIT);
        sendAll(s, &r, sizeof(r));
        closesocket(s);
        CoUninitialize();
        return;
    }
    if (!gateReserve(total_bytes)) {
        DropReply r = {};
        r.magic = htonl(DROP_MAGIC);
        r.version = htonl(DROP_VERSION);
        r.status = htonl(DROP_ERR_LIMIT);
        sendAll(s, &r, sizeof(r));
        closesocket(s);
        CoUninitialize();
        return;
    }
    struct GateRefund {
        const uint8_t *token;
        uint64_t reserved;
        const uint64_t *got;
        ~GateRefund()
        {
            const uint64_t used = (*got > reserved) ? reserved : *got;
            gateRelease(token, reserved - used);
        }
    } refund{hello.token, total_bytes, &received};

    DropReply ok = {};
    ok.magic = htonl(DROP_MAGIC);
    ok.version = htonl(DROP_VERSION);
    ok.status = htonl(DROP_OK);
    if (!sendAll(s, &ok, sizeof(ok))) {
        closesocket(s);
        CoUninitialize();
        return;
    }

    dropStat("event=drop|state=receiving|items=" + std::to_string(item_count) +
             "|bytes=" + std::to_string(total_bytes));

    std::string why;
    std::wstring dest;
    bool fallback = false;
    dropSetRunFlag(&running);
    struct RunGuard {
        ~RunGuard() { dropSetRunFlag(nullptr); }
    } run_guard;
    const int drop_x = (int)(int32_t)ntohl((uint32_t)hello.x);
    const int drop_y = (int)(int32_t)ntohl((uint32_t)hello.y);
    int resolve_x = drop_x, resolve_y = drop_y;
    if ((ntohl(hello.flags) & DROP_FLAG_REVERSE) != 0) {
        /* Reverse drag copy (target → controller): the user released over the
         * target's edge while watching the TARGET's monitor, so hello.x/y is
         * a target coordinate, not ours. Resolve from THIS machine's cursor —
         * after leaveRemote it is parked on this desktop's edge, which
         * resolves to the controller's desktop (or the Explorer window there). */
        POINT cp = {};
        if (GetCursorPos(&cp)) { resolve_x = cp.x; resolve_y = cp.y; }
    }
    if (!dropResolveFolder(resolve_x, resolve_y, dest, why)) {
        dropStat("event=drop|state=resolve_error|x=" + std::to_string(resolve_x) +
                 "|y=" + std::to_string(resolve_y) + "|msg=" + statSafe(why));
        if (!dropDefaultRecvDir(dest)) {
            sendResult(s, DROP_ERR_NO_TARGET, 0, 0, false, why);
            closesocket(s);
            CoUninitialize();
            return;
        }
        fallback = true;
    }
    dropStat(std::string("event=drop|state=target|fallback=") +
             (fallback ? "1" : "0") + "|x=" + std::to_string(resolve_x) +
             "|y=" + std::to_string(resolve_y));

    const std::wstring staging = dropStagingRoot(ntohl(hello.transfer_id));
    if (staging.empty()) {
        sendResult(s, DROP_ERR_DISK, 0, 0, fallback, "staging unavailable");
        closesocket(s);
        CoUninitialize();
        return;
    }
    if (!enoughSpace(staging, total_bytes)) {
        sendResult(s, DROP_ERR_DISK, 0, 0, fallback, "staging disk is full");
        deleteTree(staging);
        closesocket(s);
        CoUninitialize();
        return;
    }

    DropFrame h = {};
    std::vector<uint8_t> payload;
    std::vector<ManifestEntry> man;
    uint64_t declared = 0;
    if (!recvFrame(s, h, payload, running, 30000) ||
        ntohs(h.type) != DF_MANIFEST ||
        !parseManifest(payload, man, why, declared, DROP_MAX_ITEMS,
                       DROP_MAX_TOTAL_BYTES)) {
        const uint32_t st = why.empty() ? DROP_ERR_IO : DROP_ERR_PATH;
        sendResult(s, st, 0, 0, fallback, why.empty() ? "missing manifest" : why);
        deleteTree(staging);
        closesocket(s);
        CoUninitialize();
        return;
    }
    if (declared > total_bytes) {
        sendResult(s, DROP_ERR_LIMIT, 0, 0, fallback, "manifest exceeds hello");
        deleteTree(staging);
        closesocket(s);
        CoUninitialize();
        return;
    }

    const uint32_t rx = receiveFrames(s, hello, man, staging, running, why,
                                      received);
    if (rx != DROP_OK) {
        sendResult(s, rx, 0, 0, fallback, why);
        dropStat("event=drop|state=error|msg=" + statSafe(why));
        deleteTree(staging);
        closesocket(s);
        CoUninitialize();
        return;
    }

    /* The session may have ended while a large transfer was in flight. */
    if (!dropGateCheck(hello.token)) {
        sendResult(s, DROP_ERR_AUTH, 0, 0, fallback, "session ended");
        deleteTree(staging);
        closesocket(s);
        CoUninitialize();
        return;
    }

    std::vector<std::wstring> roots;
    for (const ManifestEntry &e : man) {
        if (e.rel.find('/') == std::string::npos) {
            const std::wstring w = utf8ToWide(e.rel);
            if (!w.empty()) roots.push_back(w);
        }
    }
    uint32_t committed = 0, failed = 0;
    std::string msg;
    dropCommitItems(staging, dest, roots, committed, failed, msg);
    /* Place the copied icons at the release point (best effort; an
     * auto-arranged folder or a non-visible view simply skips this). */
    if (committed > 0)
        dropPlaceIcons(dest, resolve_x, resolve_y, roots);
    /* A partial commit is not "done": at least one top-level item failed, so
     * report an error while still carrying the per-item counts. */
    const uint32_t status =
        (committed > 0 && failed == 0) ? DROP_OK : DROP_ERR_IO;
    sendResult(s, status, committed, failed, fallback, msg);
    dropStat(std::string("event=drop|state=") +
             (status == DROP_OK ? "done" : "error") +
             "|items=" + std::to_string(committed) +
             "|failed=" + std::to_string(failed) +
             "|fallback=" + (fallback ? "1" : "0") +
             (msg.empty() ? "" : "|msg=" + statSafe(msg)));
    deleteTree(staging);
    closesocket(s);
    CoUninitialize();
    std::cout << "[drop] transfer from " << peer << " committed "
              << committed << "/" << man.size() << "\n" << std::flush;
}

} /* namespace */

DropStartupStatus dropServiceStatus()
{
    DropStartupStatus st = {};
    st.state = g_drop_start_state.load();
    st.stage = g_drop_start_stage.load();
    st.error_code = g_drop_start_error.load();
    st.port = g_drop_start_port.load();
    return st;
}

void dropServiceStatusReset()
{
    reportStartup(0, 0, 0);
}

bool dropGateInstall(const uint8_t token[DROP_TOKEN_BYTES], uint64_t epoch)
{
    if (!token) return false;
    std::lock_guard<std::mutex> lk(g_gate_m);
    std::memcpy(g_gate_token, token, DROP_TOKEN_BYTES);
    g_gate_epoch = epoch;
    g_gate_active = true;
    g_gate_bytes = 0;
    return true;
}

void dropGateClear(uint64_t epoch)
{
    std::lock_guard<std::mutex> lk(g_gate_m);
    if (g_gate_active && g_gate_epoch == epoch) {
        mivaraSecureZero(g_gate_token, sizeof(g_gate_token));
        g_gate_epoch = 0;
        g_gate_active = false;
        g_gate_bytes = 0;
    }
}

bool dropGateCheck(const uint8_t token[DROP_TOKEN_BYTES])
{
    if (!token) return false;
    std::lock_guard<std::mutex> lk(g_gate_m);
    const bool ok = g_gate_active &&
                    mivaraConstEq(g_gate_token, token, DROP_TOKEN_BYTES);
    if (!ok) {
        /* Diagnose auth refusals in the field: an inactive gate means the
         * session never installed one HERE (e.g. the listener belongs to
         * another process/port); active means the token itself differed. The
         * token bytes themselves are never logged. */
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "event=drop|state=gate_reject|port=%u|active=%d|"
                      "epoch=%llu",
                      (unsigned)g_drop_start_port.load(),
                      g_gate_active ? 1 : 0,
                      (unsigned long long)g_gate_epoch);
        dropStat(buf);
    }
    return ok;
}

void dropServiceRun(std::atomic<bool> &running, uint16_t port)
{
    reportStartup(0, 0, 0);
    g_drop_start_port.store(port ? port : (uint16_t)DROP_PORT);
    cleanupStaging();
    drop_sock_t srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv == DROP_INVALID) {
        reportStartup(-1, 1, WSAGetLastError());
        std::cout << "[drop] socket init failed; file drop disabled\n"
                  << std::flush;
        return;
    }
    BOOL exclusive = TRUE;
    setsockopt(srv, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
               (const char *)&exclusive, sizeof(exclusive));
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)g_drop_start_port.load());
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(srv, (sockaddr *)&addr, sizeof(addr)) < 0) {
        reportStartup(-1, 3, WSAGetLastError());
        std::cout << "[drop] bind failed on " << g_drop_start_port.load()
                  << "; file drop disabled\n" << std::flush;
        closesocket(srv);
        return;
    }
    if (listen(srv, 4) < 0) {
        reportStartup(-1, 4, WSAGetLastError());
        closesocket(srv);
        return;
    }
    reportStartup(1, 4, 0);
    std::cout << "[drop] file-drop listener on TCP "
              << g_drop_start_port.load() << "\n"
              << std::flush;

    std::atomic<bool> active{false};
    std::thread worker;
    while (running.load()) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(srv, &rd);
        timeval tv = {1, 0};
        int sel = select(0, &rd, nullptr, nullptr, &tv);
        if (sel <= 0) continue;
        sockaddr_in cli = {};
        int len = sizeof(cli);
        drop_sock_t c = accept(srv, (sockaddr *)&cli, &len);
        if (c == DROP_INVALID) continue;
        char ip[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &cli.sin_addr, ip, sizeof(ip));

        if (active.load()) {
            DropHello hello = {};
            setTimeouts(c, 2000, 2000);
            recvAll(c, &hello, sizeof(hello), running, 2000);
            sendStatus(c, DROP_ERR_BUSY);
            closesocket(c);
            continue;
        }
        if (worker.joinable()) worker.join();

        /* Authenticate before claiming the single transfer slot. A silent
         * unauthenticated connection only costs this loop one short read; it
         * can never make a legitimate controller see BUSY. */
        setTimeouts(c, 3000, 3000);
        DropHello hello = {};
        if (recvAll(c, &hello, sizeof(hello), running, 3000) != 1) {
            closesocket(c);
            continue;
        }
        if (ntohl(hello.magic) != DROP_MAGIC ||
            ntohl(hello.version) != DROP_VERSION) {
            dropStat("event=drop|state=reject|stage=version");
            sendStatus(c, DROP_ERR_VERSION);
            closesocket(c);
            continue;
        }
        if (!dropGateCheck(hello.token)) {
            dropStat("event=drop|state=reject|stage=auth");
            sendStatus(c, DROP_ERR_AUTH);
            closesocket(c);
            continue;
        }
        if (active.exchange(true)) {
            dropStat("event=drop|state=reject|stage=busy");
            sendStatus(c, DROP_ERR_BUSY);
            closesocket(c);
            continue;
        }
        worker = std::thread([c, ip, &running, &active, hello] {
            dropWorker(c, std::string(ip), running, hello);
            active = false;
        });
    }
    if (worker.joinable()) worker.join();
    closesocket(srv);
    std::cout << "[drop] file-drop listener stopped\n" << std::flush;
}

DropResult dropSendFiles(const DropOptions &opt,
                         const std::vector<std::string> &paths,
                         const uint8_t token[DROP_TOKEN_BYTES],
                         const std::atomic<bool> &running,
                         void (*progress)(uint64_t, uint64_t, void *),
                         void *progress_ctx)
{
    DropResult res;
    if (!opt.peer_ip || !*opt.peer_ip || !token) {
        res.status = DROP_ERR_AUTH;
        res.message = "missing peer or token";
        return res;
    }
    ScanState st;
    if (!scanPaths(paths, st)) {
        res.status = DROP_ERR_LIMIT;
        res.message = st.err;
        return res;
    }
    if (st.entries.empty()) {
        res.status = DROP_ERR_PATH;
        res.message = "empty selection";
        return res;
    }

    drop_sock_t s = DROP_INVALID;
    if (!connectSocket(s, opt.peer_ip, opt.port, running, 5000)) {
        res.status = DROP_ERR_IO;
        res.message = "cannot connect to the file-drop port";
        return res;
    }
    /* A 2 s send timeout keeps session teardown (which joins the drop thread)
     * bounded even if the peer vanished mid-transfer. */
    setTimeouts(s, 30000, 2000);
    enableKeepAlive(s);

    DropHello hello = {};
    hello.magic = htonl(DROP_MAGIC);
    hello.version = htonl(DROP_VERSION);
    std::memcpy(hello.token, token, DROP_TOKEN_BYTES);
    hello.transfer_id = htonl(opt.transfer_id ? opt.transfer_id
                                              : (uint32_t)GetTickCount64());
    hello.x = (int32_t)htonl((uint32_t)opt.x);
    hello.y = (int32_t)htonl((uint32_t)opt.y);
    hello.item_count = htonl((uint32_t)st.entries.size());
    hello.total_bytes = hton64(st.total);
    hello.flags = htonl(opt.flags);
    if (!sendAll(s, &hello, sizeof(hello))) {
        closesocket(s);
        res.status = DROP_ERR_IO;
        res.message = "handshake send failed";
        return res;
    }
    DropReply reply = {};
    if (recvAll(s, &reply, sizeof(reply), running, 10000) != 1 ||
        ntohl(reply.magic) != DROP_MAGIC ||
        ntohl(reply.version) != DROP_VERSION) {
        closesocket(s);
        res.status = DROP_ERR_IO;
        res.message = "handshake reply missing";
        return res;
    }
    if (ntohl(reply.status) != DROP_OK) {
        closesocket(s);
        res.status = ntohl(reply.status);
        res.message = "target refused the transfer";
        return res;
    }
    if (!sendManifest(s, st.entries)) {
        closesocket(s);
        res.status = DROP_ERR_IO;
        res.message = "manifest send failed";
        return res;
    }

    uint64_t sent_total = 0;
    for (size_t i = 0; i < st.entries.size(); i++) {
        const Entry &e = st.entries[i];
        if (e.dir) continue;
        if (!running.load()) {
            sendAbort(s, DROP_ERR_CANCELLED, "cancelled");
            closesocket(s);
            res.status = DROP_ERR_CANCELLED;
            res.message = "cancelled";
            return res;
        }
        /* Read-only sharing excludes concurrent writers/deleters for the whole
         * file, so a same-size in-place save cannot produce a torn copy. */
        HANDLE f = CreateFileW(e.abs.c_str(), GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN,
                               nullptr);
        if (f == INVALID_HANDLE_VALUE) {
            sendAbort(s, DROP_ERR_IO, "cannot read source");
            closesocket(s);
            res.status = DROP_ERR_IO;
            res.message = "cannot read source: " + wideToUtf8(e.abs);
            return res;
        }
        LARGE_INTEGER li = {};
        if (!GetFileSizeEx(f, &li) || (uint64_t)li.QuadPart != e.size ||
            !fileUnchanged(f, e)) {
            CloseHandle(f);
            sendAbort(s, DROP_ERR_IO, "source changed");
            closesocket(s);
            res.status = DROP_ERR_IO;
            res.message = "source changed during transfer";
            return res;
        }
        std::vector<uint8_t> begin;
        appendU32(begin, (uint32_t)i);
        appendU64(begin, e.size);
        if (!sendFrame(s, DF_FILE_BEGIN, begin.data(), (uint32_t)begin.size())) {
            CloseHandle(f);
            closesocket(s);
            res.status = DROP_ERR_IO;
            res.message = "file begin send failed";
            return res;
        }
        MivaraSha256Ctx hash;
        mivaraSha256Init(&hash);
        uint64_t off = 0;
        std::vector<uint8_t> buf(64 * 1024);
        for (;;) {
            DWORD got = 0;
            if (!ReadFile(f, buf.data(), (DWORD)buf.size(), &got, nullptr)) {
                CloseHandle(f);
                sendAbort(s, DROP_ERR_IO, "read failed");
                closesocket(s);
                res.status = DROP_ERR_IO;
                res.message = "source read failed";
                return res;
            }
            if (got == 0) break;
            if (!running.load()) {
                CloseHandle(f);
                sendAbort(s, DROP_ERR_CANCELLED, "cancelled");
                closesocket(s);
                res.status = DROP_ERR_CANCELLED;
                res.message = "cancelled";
                return res;
            }
            if (off + got > e.size) {
                CloseHandle(f);
                sendAbort(s, DROP_ERR_IO, "source grew");
                closesocket(s);
                res.status = DROP_ERR_IO;
                res.message = "source changed during transfer";
                return res;
            }
            std::vector<uint8_t> data;
            data.reserve(12 + got);
            appendU32(data, (uint32_t)i);
            appendU64(data, off);
            data.insert(data.end(), buf.begin(), buf.begin() + got);
            if (!sendFrame(s, DF_FILE_DATA, data.data(),
                          (uint32_t)data.size())) {
                CloseHandle(f);
                closesocket(s);
                res.status = DROP_ERR_IO;
                res.message = "data send failed";
                return res;
            }
            mivaraSha256Update(&hash, buf.data(), got);
            off += got;
            sent_total += got;
            if (progress) progress(sent_total, st.total, progress_ctx);
        }
        const bool unchanged = fileUnchanged(f, e);
        CloseHandle(f);
        if (off != e.size || !unchanged) {
            sendAbort(s, DROP_ERR_IO, "source changed");
            closesocket(s);
            res.status = DROP_ERR_IO;
            res.message = "source changed during transfer";
            return res;
        }
        uint8_t digest[32] = {};
        mivaraSha256Final(&hash, digest);
        std::vector<uint8_t> end;
        appendU32(end, (uint32_t)i);
        end.insert(end.end(), digest, digest + 32);
        if (!sendFrame(s, DF_FILE_END, end.data(), (uint32_t)end.size())) {
            closesocket(s);
            res.status = DROP_ERR_IO;
            res.message = "file end send failed";
            return res;
        }
    }

    std::vector<uint8_t> done;
    appendU32(done, (uint32_t)st.entries.size());
    if (!sendFrame(s, DF_DONE, done.data(), (uint32_t)done.size())) {
        closesocket(s);
        res.status = DROP_ERR_IO;
        res.message = "done send failed";
        return res;
    }

    DropFrame h = {};
    std::vector<uint8_t> payload;
    if (!recvFrame(s, h, payload, running, 600000) ||
        ntohs(h.type) != DF_RESULT) {
        closesocket(s);
        res.status = DROP_ERR_IO;
        res.message = "result missing";
        return res;
    }
    ByteCursor c{payload.data(), payload.size()};
    res.status = c.u32();
    res.committed = c.u32();
    res.failed = c.u32();
    const uint32_t flags = c.u32();
    const uint32_t n = c.u32();
    res.message = c.bytes(n);
    res.fallback = (flags & 1u) != 0;
    if (!c.ok) {
        closesocket(s);
        res.status = DROP_ERR_IO;
        res.committed = 0;
        res.failed = 0;
        res.message = "malformed result";
        return res;
    }
    closesocket(s);
    return res;
}

#else /* !_WIN32 */

namespace {

using drop_sock_t = int;
const drop_sock_t DROP_INVALID = -1;
const uint32_t DROP_FRAME_MAGIC = DROP_MAGIC;

enum DropFrameType : uint16_t {
    DF_MANIFEST   = 1,
    DF_FILE_BEGIN = 2,
    DF_FILE_DATA  = 3,
    DF_FILE_END   = 4,
    DF_DONE       = 5,
    DF_RESULT     = 6,
    DF_ABORT      = 7
};

#pragma pack(push, 1)
struct DropHello {
    uint32_t magic;
    uint32_t version;
    uint8_t  token[DROP_TOKEN_BYTES];
    uint32_t transfer_id;
    int32_t  x;
    int32_t  y;
    uint32_t item_count;
    uint64_t total_bytes;
    uint32_t flags;
    uint32_t reserved;
};

struct DropReply {
    uint32_t magic;
    uint32_t version;
    uint32_t status;
    uint32_t reserved;
};

struct DropFrame {
    uint32_t magic;
    uint16_t version;
    uint16_t type;
    uint32_t length;
    uint32_t reserved;
};
#pragma pack(pop)

static_assert(sizeof(DropHello) == 72, "DropHello must be 72 bytes");
static_assert(sizeof(DropReply) == 16, "DropReply must be 16 bytes");
static_assert(sizeof(DropFrame) == 16, "DropFrame must be 16 bytes");

/* ── byte order / buffers ────────────────────────────────────────────────── */

uint64_t hton64(uint64_t v)
{
    return ((uint64_t)htonl((uint32_t)(v >> 32))) |
           ((uint64_t)htonl((uint32_t)v) << 32);
}

uint64_t ntoh64(uint64_t v)
{
    return ((uint64_t)ntohl((uint32_t)(v >> 32))) |
           ((uint64_t)ntohl((uint32_t)v) << 32);
}

void appendU32(std::vector<uint8_t> &v, uint32_t x)
{
    x = htonl(x);
    const uint8_t *p = (const uint8_t *)&x;
    v.insert(v.end(), p, p + 4);
}

void appendU64(std::vector<uint8_t> &v, uint64_t x)
{
    x = hton64(x);
    const uint8_t *p = (const uint8_t *)&x;
    v.insert(v.end(), p, p + 8);
}

struct ByteCursor {
    const uint8_t *p;
    size_t n;
    bool ok = true;

    bool need(size_t k)
    {
        if (!ok || k > n) { ok = false; return false; }
        return true;
    }
    uint8_t u8()
    {
        if (!need(1)) return 0;
        const uint8_t v = *p; p++; n--; return v;
    }
    uint32_t u32()
    {
        if (!need(4)) return 0;
        uint32_t v;
        std::memcpy(&v, p, 4);
        p += 4; n -= 4;
        return ntohl(v);
    }
    uint64_t u64()
    {
        if (!need(8)) return 0;
        uint64_t v;
        std::memcpy(&v, p, 8);
        p += 8; n -= 8;
        return ntoh64(v);
    }
    std::string bytes(size_t k)
    {
        if (!need(k)) return std::string();
        std::string s((const char *)p, k);
        p += k; n -= k;
        return s;
    }
};

/* ── sockets ─────────────────────────────────────────────────────────────── */

void dropClose(drop_sock_t s)
{
    if (s >= 0) close(s);
}

void setTimeouts(drop_sock_t s, int recv_ms, int send_ms)
{
    if (recv_ms) {
        struct timeval tv = {recv_ms / 1000, (recv_ms % 1000) * 1000};
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }
    if (send_ms) {
        struct timeval tv = {send_ms / 1000, (send_ms % 1000) * 1000};
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }
}

void enableKeepAlive(drop_sock_t s)
{
    int on = 1;
    setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));
}

bool sendAll(drop_sock_t s, const void *buf, size_t len)
{
    const char *p = (const char *)buf;
    size_t off = 0;
    while (off < len) {
        const ssize_t n = send(s, p + off, len - off, MSG_NOSIGNAL);
        if (n <= 0) return false;
        off += (size_t)n;
    }
    return true;
}

/* 1 = ok, 0 = deadline, -1 = closed/error/caller stopped. */
int recvAllTimeout(drop_sock_t s, void *buf, size_t len,
                   const std::atomic<bool> &running, int timeout_ms)
{
    char *p = (char *)buf;
    size_t off = 0;
    const auto start = std::chrono::steady_clock::now();
    while (off < len) {
        if (!running.load()) return -1;
        if (timeout_ms > 0) {
            const auto elapsed = std::chrono::duration_cast<
                std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start);
            if (elapsed.count() > timeout_ms) return 0;
        }
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(s, &rd);
        struct timeval tv = {0, 200000};
        const int sel = select(s + 1, &rd, nullptr, nullptr, &tv);
        if (sel < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (sel == 0) continue;
        const ssize_t n = recv(s, p + off, len - off, 0);
        if (n == 0) return -1;
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                continue;
            return -1;
        }
        off += (size_t)n;
    }
    return 1;
}

drop_sock_t connectTimeout(const char *ip, int port,
                           const std::atomic<bool> &running, int ms)
{
    drop_sock_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return DROP_INVALID;
    const int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, flags | O_NONBLOCK);

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        dropClose(s);
        return DROP_INVALID;
    }
    int rc = connect(s, (sockaddr *)&addr, sizeof(addr));
    if (rc != 0 && errno != EINPROGRESS && errno != EWOULDBLOCK) {
        dropClose(s);
        return DROP_INVALID;
    }
    if (rc == 0) {
        fcntl(s, F_SETFL, flags);
        return s;
    }
    const auto start = std::chrono::steady_clock::now();
    while (running.load()) {
        const auto elapsed = std::chrono::duration_cast<
            std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
        if (elapsed.count() > ms) break;
        fd_set wr;
        FD_ZERO(&wr);
        FD_SET(s, &wr);
        struct timeval tv = {0, 100000};
        const int sel = select(s + 1, nullptr, &wr, nullptr, &tv);
        if (sel < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (sel == 0) continue;
        int err = 0;
        socklen_t len = sizeof(err);
        if (getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0)
            break;
        fcntl(s, F_SETFL, flags);
        return s;
    }
    dropClose(s);
    return DROP_INVALID;
}

bool sendFrame(drop_sock_t s, uint16_t type, const void *data, uint32_t len)
{
    DropFrame h = {};
    h.magic = htonl(DROP_FRAME_MAGIC);
    h.version = htons((uint16_t)DROP_VERSION);
    h.type = htons(type);
    h.length = htonl(len);
    if (!sendAll(s, &h, sizeof(h))) return false;
    return len == 0 || sendAll(s, data, len);
}

bool recvFrame(drop_sock_t s, DropFrame &h, std::vector<uint8_t> &payload,
               const std::atomic<bool> &running, int timeout_ms)
{
    payload.clear();
    if (recvAllTimeout(s, &h, sizeof(h), running, timeout_ms) != 1)
        return false;
    if (ntohl(h.magic) != DROP_FRAME_MAGIC ||
        ntohs(h.version) != DROP_VERSION)
        return false;
    const uint32_t len = ntohl(h.length);
    if (len > 4u * 1024u * 1024u) return false;
    payload.resize(len);
    if (len && recvAllTimeout(s, payload.data(), len, running, timeout_ms) != 1)
        return false;
    return true;
}

/* ── paths / files ───────────────────────────────────────────────────────── */

std::string baseName(const std::string &p)
{
    size_t end = p.size();
    while (end > 0 && p[end - 1] == '/') end--;
    if (end == 0) return std::string();
    const size_t b = p.find_last_of('/', end - 1);
    return p.substr(b == std::string::npos ? 0 : b + 1,
                    end - (b == std::string::npos ? 0 : b + 1));
}

bool validRelPath(const std::string &rel, std::string &why)
{
    if (rel.empty()) { why = "empty path"; return false; }
    if (rel.size() > DROP_MAX_PATH_BYTES) { why = "path too long"; return false; }
    if (rel[0] == '/') { why = "absolute path"; return false; }
    size_t start = 0;
    for (;;) {
        const size_t slash = rel.find('/', start);
        const size_t end = slash == std::string::npos ? rel.size() : slash;
        const std::string comp = rel.substr(start, end - start);
        if (comp.empty() || comp == "." || comp == "..") {
            why = "invalid path component";
            return false;
        }
        if (comp.size() > 255) { why = "path component too long"; return false; }
        for (unsigned char c : comp) {
            if (c == 0 || c == '\\' || c < 0x20 || c == 0x7f) {
                why = "invalid character in path";
                return false;
            }
        }
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return true;
}

uint64_t unixToFiletime(time_t sec)
{
    if (sec <= 0) return 0;
    return ((uint64_t)sec + 11644473600ull) * 10000000ull;
}

time_t filetimeToUnix(uint64_t ft)
{
    if (ft < 11644473600ull * 10000000ull) return 0;
    return (time_t)(ft / 10000000ull - 11644473600ull);
}

bool mkdirP(const std::string &path, std::string &why)
{
    if (path.empty() || path[0] != '/') { why = "not an absolute path"; return false; }
    std::string cur;
    size_t i = 0;
    while (i < path.size()) {
        while (i < path.size() && path[i] == '/') i++;
        if (i >= path.size()) break;
        const size_t slash = path.find('/', i);
        const size_t end = slash == std::string::npos ? path.size() : slash;
        cur += "/";
        cur += path.substr(i, end - i);
        if (mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) {
            why = "cannot create " + cur;
            return false;
        }
        struct stat sb;
        if (stat(cur.c_str(), &sb) != 0 || !S_ISDIR(sb.st_mode)) {
            why = "not a directory: " + cur;
            return false;
        }
        i = end;
    }
    return true;
}

void removeTree(const std::string &path)
{
    struct stat sb;
    if (lstat(path.c_str(), &sb) != 0) return;
    if (!S_ISDIR(sb.st_mode)) {
        unlink(path.c_str());
        return;
    }
    DIR *d = opendir(path.c_str());
    if (d) {
        while (dirent *e = readdir(d)) {
            if (std::strcmp(e->d_name, ".") == 0 ||
                std::strcmp(e->d_name, "..") == 0)
                continue;
            removeTree(path + "/" + e->d_name);
        }
        closedir(d);
    }
    rmdir(path.c_str());
}

bool existsNoFollow(const std::string &path)
{
    struct stat sb;
    return lstat(path.c_str(), &sb) == 0;
}

std::string uniqueName(const std::string &dir, const std::string &name)
{
    if (!existsNoFollow(dir + "/" + name)) return name;
    std::string stem = name, ext;
    const size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && dot > 0) {
        stem = name.substr(0, dot);
        ext = name.substr(dot);
    }
    for (int i = 1; i < 10000; i++) {
        const std::string suffix = " (" + std::to_string(i) + ")";
        size_t max_stem = 255;
        const size_t fixed = suffix.size() + ext.size();
        max_stem = fixed < 255 ? 255 - fixed : 0;
        std::string s = stem;
        if (s.size() > max_stem) {
            size_t cut = max_stem;
            while (cut > 0 && cut < s.size() &&
                   ((unsigned char)s[cut] & 0xC0) == 0x80)
                cut--;
            s.resize(cut);
        }
        const std::string candidate = s + suffix + ext;
        if (!existsNoFollow(dir + "/" + candidate)) return candidate;
    }
    return name;
}

bool copyFile(const std::string &src, const std::string &dst_dir,
              const std::string &name, std::string &why)
{
    struct stat sb;
    if (lstat(src.c_str(), &sb) != 0 || !S_ISREG(sb.st_mode)) {
        why = "source is not a regular file";
        return false;
    }
    const std::string chosen = uniqueName(dst_dir, name);
    const std::string dst = dst_dir + "/" + chosen;
    const int in = open(src.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (in < 0) { why = "cannot open source"; return false; }
    const int out = open(dst.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC |
                         O_NOFOLLOW, 0600);
    if (out < 0) {
        close(in);
        why = "cannot create destination";
        return false;
    }
    bool ok = true;
    char buf[64 * 1024];
    for (;;) {
        const ssize_t n = read(in, buf, sizeof(buf));
        if (n == 0) break;
        if (n < 0) {
            if (errno == EINTR) continue;
            ok = false;
            break;
        }
        size_t off = 0;
        while (off < (size_t)n) {
            const ssize_t w = write(out, buf + off, (size_t)n - off);
            if (w <= 0) { ok = false; break; }
            off += (size_t)w;
        }
        if (!ok) break;
    }
    if (ok) {
        fchmod(out, sb.st_mode & 0777);
        if (fsync(out) != 0) ok = false;
    }
    close(in);
    close(out);
    if (!ok) {
        unlink(dst.c_str());
        why = "copy failed";
        return false;
    }
    struct timeval tv[2] = {};
    tv[0].tv_sec = sb.st_mtime;
    tv[1].tv_sec = sb.st_mtime;
    utimes(dst.c_str(), tv);
    return true;
}

bool copyDir(const std::string &src, const std::string &dst_dir,
             const std::string &name, std::string &why)
{
    struct stat sb;
    if (lstat(src.c_str(), &sb) != 0 || !S_ISDIR(sb.st_mode)) {
        why = "source is not a directory";
        return false;
    }
    const std::string chosen = uniqueName(dst_dir, name);
    const std::string dst = dst_dir + "/" + chosen;
    if (mkdir(dst.c_str(), 0700) != 0) {
        why = "cannot create destination directory";
        return false;
    }
    DIR *d = opendir(src.c_str());
    if (!d) {
        rmdir(dst.c_str());
        why = "cannot enumerate source directory";
        return false;
    }
    bool ok = true;
    while (dirent *e = readdir(d)) {
        if (std::strcmp(e->d_name, ".") == 0 ||
            std::strcmp(e->d_name, "..") == 0)
            continue;
        const std::string child_src = src + "/" + e->d_name;
        struct stat cs;
        if (lstat(child_src.c_str(), &cs) != 0) { ok = false; break; }
        if (S_ISDIR(cs.st_mode)) {
            if (!copyDir(child_src, dst, e->d_name, why)) { ok = false; break; }
        } else if (S_ISREG(cs.st_mode)) {
            if (!copyFile(child_src, dst, e->d_name, why)) { ok = false; break; }
        } else {
            why = "symlink or special file is not supported";
            ok = false;
            break;
        }
    }
    closedir(d);
    if (!ok) {
        removeTree(dst);
        return false;
    }
    chmod(dst.c_str(), sb.st_mode & 0777);
    struct timeval tv[2] = {};
    tv[0].tv_sec = sb.st_mtime;
    tv[1].tv_sec = sb.st_mtime;
    utimes(dst.c_str(), tv);
    return true;
}

/* ── destination / staging ───────────────────────────────────────────────── */

bool enoughSpace(const std::string &dir, uint64_t need)
{
    struct statvfs vfs;
    if (statvfs(dir.c_str(), &vfs) != 0) return false;
    const uint64_t freeb = (uint64_t)vfs.f_bavail * (uint64_t)vfs.f_frsize;
    const uint64_t reserve = 64ull * 1024ull * 1024ull;
    return freeb > need && freeb - need > reserve;
}

bool defaultRecvDir(std::string &dest)
{
    std::string base;
    const char *env = getenv("MIVARA_DROP_DIR");
    if (env && *env) {
        base = env;
    } else {
        const char *dl = getenv("XDG_DOWNLOAD_DIR");
        if (dl && *dl) {
            base = dl;
        } else {
            const char *home = getenv("HOME");
            base = home && *home ? std::string(home) + "/Downloads" : "/tmp";
        }
        base += "/Mivara";
    }
    std::string why;
    if (!mkdirP(base, why)) return false;
    dest = base;
    return true;
}

std::string cacheRoot()
{
    const char *xdg = getenv("XDG_CACHE_HOME");
    std::string base;
    if (xdg && *xdg) {
        base = xdg;
    } else {
        const char *home = getenv("HOME");
        base = home && *home ? std::string(home) + "/.cache" : "/tmp";
    }
    base += "/mivara/drop";
    return base;
}

void cleanupStaging()
{
    const std::string root = cacheRoot();
    std::string why;
    if (!mkdirP(root, why)) return;
    DIR *d = opendir(root.c_str());
    if (!d) return;
    while (dirent *e = readdir(d)) {
        if (std::strcmp(e->d_name, ".") == 0 ||
            std::strcmp(e->d_name, "..") == 0)
            continue;
        removeTree(root + "/" + e->d_name);
    }
    closedir(d);
}

std::string stagingRoot(uint32_t transfer_id)
{
    const std::string root = cacheRoot();
    std::string why;
    if (!mkdirP(root, why)) return std::string();
    char name[80];
    std::snprintf(name, sizeof(name), "%08x-%d", transfer_id, (int)getpid());
    const std::string path = root + "/" + name;
    if (mkdir(path.c_str(), 0700) != 0) return std::string();
    return path;
}

/* ── source scan ─────────────────────────────────────────────────────────── */

struct Entry {
    std::string rel;
    std::string abs;
    bool dir = false;
    uint64_t size = 0;
    uint64_t mtime = 0;
};

struct ScanState {
    std::vector<Entry> entries;
    uint64_t total = 0;
    uint32_t max_items = DROP_MAX_ITEMS;
    uint64_t max_bytes = DROP_MAX_TOTAL_BYTES;
    std::string err;
};

bool scanOne(const std::string &abs, const std::string &rel, unsigned depth,
             ScanState &st)
{
    struct stat sb;
    if (lstat(abs.c_str(), &sb) != 0) {
        st.err = "cannot stat " + abs;
        return false;
    }
    if (S_ISLNK(sb.st_mode)) {
        st.err = "symlink is not supported: " + abs;
        return false;
    }
    Entry e;
    e.rel = rel;
    e.abs = abs;
    e.dir = S_ISDIR(sb.st_mode);
    e.mtime = unixToFiletime(sb.st_mtime);
    if (!e.dir) {
        if (!S_ISREG(sb.st_mode)) {
            st.err = "special file is not supported: " + abs;
            return false;
        }
        e.size = (uint64_t)sb.st_size;
        if (e.size > DROP_MAX_FILE_BYTES) {
            st.err = "single file over the limit";
            return false;
        }
        st.total += e.size;
        if (st.total > st.max_bytes) {
            st.err = "selection over the total size limit";
            return false;
        }
    }
    if (st.entries.size() >= st.max_items) {
        st.err = "selection has too many items";
        return false;
    }
    st.entries.push_back(e);
    if (!e.dir) return true;

    if (depth >= DROP_MAX_DEPTH) {
        st.err = "directory tree too deep";
        return false;
    }
    DIR *d = opendir(abs.c_str());
    if (!d) {
        st.err = "cannot enumerate " + abs;
        return false;
    }
    bool ok = true;
    while (dirent *de = readdir(d)) {
        if (std::strcmp(de->d_name, ".") == 0 ||
            std::strcmp(de->d_name, "..") == 0)
            continue;
        const std::string child_abs = abs + "/" + de->d_name;
        const std::string child_rel = rel + "/" + de->d_name;
        std::string why;
        if (!validRelPath(child_rel, why)) {
            st.err = "invalid path: " + why;
            ok = false;
            break;
        }
        if (!scanOne(child_abs, child_rel, depth + 1, st)) {
            ok = false;
            break;
        }
    }
    closedir(d);
    return ok;
}

bool scanPaths(const std::vector<std::string> &paths, ScanState &st)
{
    std::unordered_set<std::string> roots;
    for (const std::string &p : paths) {
        if (p.empty() || p[0] != '/') {
            st.err = "source path is not absolute: " + p;
            return false;
        }
        const std::string name = baseName(p);
        if (name.empty()) {
            st.err = "cannot derive source name";
            return false;
        }
        std::string why;
        if (!validRelPath(name, why)) {
            st.err = "invalid source name: " + why;
            return false;
        }
        if (!roots.insert(name).second) {
            st.err = "two selected items share the same name: " + name;
            return false;
        }
        if (!scanOne(p, name, 0, st)) return false;
    }
    return true;
}

bool fileUnchanged(int fd, const Entry &e)
{
    struct stat sb;
    if (fstat(fd, &sb) != 0) return false;
    return S_ISREG(sb.st_mode) && (uint64_t)sb.st_size == e.size &&
           unixToFiletime(sb.st_mtime) == e.mtime;
}

/* ── manifest ────────────────────────────────────────────────────────────── */

bool sendManifest(drop_sock_t s, const std::vector<Entry> &entries)
{
    std::vector<uint8_t> payload;
    payload.reserve(4096);
    appendU32(payload, (uint32_t)entries.size());
    for (const Entry &e : entries) {
        payload.push_back(e.dir ? 1 : 0);
        payload.push_back(0);
        payload.push_back(0);
        payload.push_back(0);
        appendU32(payload, (uint32_t)e.rel.size());
        appendU64(payload, e.size);
        appendU64(payload, e.mtime);
        payload.insert(payload.end(), e.rel.begin(), e.rel.end());
    }
    if (payload.size() > 4u * 1024u * 1024u) return false;
    return sendFrame(s, DF_MANIFEST, payload.data(), (uint32_t)payload.size());
}

struct ManifestEntry {
    std::string rel;
    bool dir = false;
    uint64_t size = 0;
    uint64_t mtime = 0;
};

bool parseManifest(const std::vector<uint8_t> &payload,
                   std::vector<ManifestEntry> &out, std::string &why,
                   uint64_t &total, uint32_t max_items, uint64_t max_bytes)
{
    ByteCursor c{payload.data(), payload.size()};
    const uint32_t count = c.u32();
    if (!c.ok || count > max_items) { why = "invalid item count"; return false; }
    out.clear();
    out.reserve(count);
    total = 0;
    std::unordered_set<std::string> seen;
    for (uint32_t i = 0; i < count; i++) {
        ManifestEntry e;
        e.dir = c.u8() != 0;
        c.u8(); c.u8(); c.u8();
        const uint32_t n = c.u32();
        e.size = c.u64();
        e.mtime = c.u64();
        if (!c.ok || n == 0 || n > DROP_MAX_PATH_BYTES) {
            why = "invalid manifest entry";
            return false;
        }
        e.rel = c.bytes(n);
        if (!c.ok) { why = "truncated manifest"; return false; }
        if (!validRelPath(e.rel, why)) return false;
        if (!seen.insert(e.rel).second) {
            why = "duplicate manifest path";
            return false;
        }
        if (!e.dir) {
            if (e.size > DROP_MAX_FILE_BYTES) {
                why = "single file over the limit";
                return false;
            }
            total += e.size;
            if (total > max_bytes) { why = "transfer over the limit"; return false; }
        }
        out.push_back(std::move(e));
    }
    if (!c.ok) { why = "truncated manifest"; return false; }
    return true;
}

/* ── receiver ────────────────────────────────────────────────────────────── */

/* Create the parent directories of `rel` under `root` without following a
 * symlink at any level. Returns the final path. */
bool preparePath(const std::string &root, const std::string &rel, bool want_dir,
                 std::string &full, std::string &why)
{
    std::string cur = root;
    size_t start = 0;
    for (;;) {
        const size_t slash = rel.find('/', start);
        const size_t end = slash == std::string::npos ? rel.size() : slash;
        const std::string comp = rel.substr(start, end - start);
        cur += "/" + comp;
        const bool last = slash == std::string::npos;
        if (last && !want_dir) {
            full = cur;
            return true;
        }
        if (mkdir(cur.c_str(), 0700) != 0 && errno != EEXIST) {
            why = "cannot create staging directory";
            return false;
        }
        struct stat sb;
        if (lstat(cur.c_str(), &sb) != 0 || !S_ISDIR(sb.st_mode)) {
            why = "staging path is not a directory";
            return false;
        }
        if (last) {
            full = cur;
            return true;
        }
        start = end + 1;
    }
}

uint32_t receiveFrames(drop_sock_t s, const DropHello &hello,
                       const std::vector<ManifestEntry> &man,
                       const std::string &staging,
                       const std::atomic<bool> &running,
                       std::string &why, uint64_t &received)
{
    std::vector<int> open_fd(man.size(), -1);
    std::vector<MivaraSha256Ctx> hash(man.size());
    std::vector<uint64_t> expected(man.size(), 0);
    std::vector<std::string> part(man.size());
    std::vector<std::string> final_path(man.size());
    int current = -1;
    uint32_t status = DROP_OK;
    uint32_t frames = 0;
    received = 0;

    for (size_t i = 0; i < man.size(); i++) {
        std::string full;
        if (!preparePath(staging, man[i].rel, man[i].dir, full, why)) {
            status = DROP_ERR_PATH;
            goto cleanup;
        }
        if (man[i].dir) continue;
        final_path[i] = full;
        part[i] = full + ".part";
    }

    for (;;) {
        if (!running.load()) {
            status = DROP_ERR_CANCELLED;
            why = "session ended";
            goto cleanup;
        }
        DropFrame h = {};
        std::vector<uint8_t> payload;
        if (!recvFrame(s, h, payload, running, 30000)) {
            status = DROP_ERR_IO;
            why = "transfer timed out or disconnected";
            goto cleanup;
        }
        const uint16_t type = ntohs(h.type);
        ByteCursor c{payload.data(), payload.size()};

        if (((++frames) & 63u) == 0 && !dropGateCheck(hello.token)) {
            status = DROP_ERR_AUTH;
            why = "session ended";
            goto cleanup;
        }

        if (type == DF_ABORT) {
            const uint32_t st = c.u32();
            const uint32_t n = c.u32();
            why = c.bytes(n);
            status = st ? st : DROP_ERR_IO;
            goto cleanup;
        }
        if (type == DF_DONE) {
            if (current >= 0) {
                status = DROP_ERR_IO;
                why = "file left open";
                goto cleanup;
            }
            break;
        }
        if (type == DF_FILE_BEGIN) {
            const uint32_t idx = c.u32();
            const uint64_t size = c.u64();
            if (!c.ok || idx >= man.size() || man[idx].dir ||
                size != man[idx].size || current >= 0) {
                status = DROP_ERR_PATH;
                why = "invalid file begin";
                goto cleanup;
            }
            if (!enoughSpace(staging, man[idx].size)) {
                status = DROP_ERR_DISK;
                why = "staging disk is full";
                goto cleanup;
            }
            if (open_fd[idx] >= 0) close(open_fd[idx]);
            open_fd[idx] = open(part[idx].c_str(),
                                O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC |
                                    O_NOFOLLOW,
                                0600);
            if (open_fd[idx] < 0) {
                status = DROP_ERR_IO;
                why = "cannot create staging file";
                goto cleanup;
            }
            mivaraSha256Init(&hash[idx]);
            expected[idx] = 0;
            current = (int)idx;
            continue;
        }
        if (type == DF_FILE_DATA) {
            const uint32_t idx = c.u32();
            const uint64_t off = c.u64();
            const size_t n = payload.size() >= 12 ? payload.size() - 12 : 0;
            if (!c.ok || (int)idx != current || off != expected[idx] ||
                n == 0 || n > 64u * 1024u) {
                status = DROP_ERR_IO;
                why = "invalid file data";
                goto cleanup;
            }
            if (received + n > hello.total_bytes ||
                received + n > DROP_MAX_TOTAL_BYTES) {
                status = DROP_ERR_LIMIT;
                why = "transfer exceeds declared size";
                goto cleanup;
            }
            const uint8_t *data = payload.data() + 12;
            size_t woff = 0;
            while (woff < n) {
                const ssize_t w = write(open_fd[idx], data + woff, n - woff);
                if (w <= 0) {
                    if (w < 0 && errno == EINTR) continue;
                    status = DROP_ERR_IO;
                    why = "write failed";
                    goto cleanup;
                }
                woff += (size_t)w;
            }
            mivaraSha256Update(&hash[idx], data, n);
            expected[idx] += n;
            received += n;
            continue;
        }
        if (type == DF_FILE_END) {
            const uint32_t idx = c.u32();
            if (!c.ok || (int)idx != current || c.n < 32) {
                status = DROP_ERR_IO;
                why = "invalid file end";
                goto cleanup;
            }
            uint8_t want[32] = {}, got[32] = {};
            std::memcpy(want, c.p, 32);
            mivaraSha256Final(&hash[idx], got);
            if (!mivaraConstEq(want, got, 32) ||
                expected[idx] != man[idx].size) {
                status = DROP_ERR_IO;
                why = "checksum mismatch";
                goto cleanup;
            }
            fsync(open_fd[idx]);
            close(open_fd[idx]);
            open_fd[idx] = -1;
            struct timeval tv[2] = {};
            tv[0].tv_sec = filetimeToUnix(man[idx].mtime);
            tv[1].tv_sec = tv[0].tv_sec;
            utimes(part[idx].c_str(), tv);
            if (link(part[idx].c_str(), final_path[idx].c_str()) != 0) {
                status = DROP_ERR_IO;
                why = "cannot commit staged file";
                goto cleanup;
            }
            unlink(part[idx].c_str());
            current = -1;
            continue;
        }
        status = DROP_ERR_IO;
        why = "unknown frame";
        goto cleanup;
    }

cleanup:
    for (int fd : open_fd)
        if (fd >= 0) close(fd);
    return status;
}

void sendStatus(drop_sock_t s, uint32_t status)
{
    DropReply r = {};
    r.magic = htonl(DROP_MAGIC);
    r.version = htonl(DROP_VERSION);
    r.status = htonl(status);
    sendAll(s, &r, sizeof(r));
}

bool sendResult(drop_sock_t s, uint32_t status, uint32_t committed,
                uint32_t failed, bool fallback, const std::string &msg)
{
    std::vector<uint8_t> payload;
    appendU32(payload, status);
    appendU32(payload, committed);
    appendU32(payload, failed);
    appendU32(payload, fallback ? 1u : 0u);
    appendU32(payload, (uint32_t)msg.size());
    payload.insert(payload.end(), msg.begin(), msg.end());
    return sendFrame(s, DF_RESULT, payload.data(), (uint32_t)payload.size());
}

bool sendAbort(drop_sock_t s, uint32_t status, const std::string &msg)
{
    std::vector<uint8_t> payload;
    appendU32(payload, status);
    appendU32(payload, (uint32_t)msg.size());
    payload.insert(payload.end(), msg.begin(), msg.end());
    return sendFrame(s, DF_ABORT, payload.data(), (uint32_t)payload.size());
}

void commitItems(const std::string &staging, const std::string &dest,
                 const std::vector<std::string> &roots, uint32_t &committed,
                 uint32_t &failed, std::string &msg)
{
    for (const std::string &root : roots) {
        const std::string src = staging + "/" + root;
        struct stat sb;
        if (lstat(src.c_str(), &sb) != 0) {
            failed++;
            if (msg.size() < 200) msg += "missing " + root + "; ";
            continue;
        }
        std::string why;
        bool ok = false;
        if (S_ISDIR(sb.st_mode)) ok = copyDir(src, dest, root, why);
        else if (S_ISREG(sb.st_mode)) ok = copyFile(src, dest, root, why);
        else why = "unsupported item type";
        if (ok) {
            committed++;
        } else {
            failed++;
            if (msg.size() < 200) msg += root + ": " + why + "; ";
        }
    }
}

#if defined(__linux__)
/* ── XDND destination delivery ─────────────────────────────────────────────
 * Windows resolves the folder under the release point through the shell's OLE
 * drop target. On X11 the equivalent is a short-lived XDND source: find the
 * deepest XdndAware window at the release point, own XdndSelection, list the
 * staged top-level items as text/uri-list, and let that window copy them. If
 * no window accepts (desktop, panel, terminal, headless session) the caller
 * falls back to the default receive directory. */

uint64_t xdndNowMs()
{
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool xdndNextEvent(Display *d, XEvent &ev, uint64_t deadline)
{
    for (;;) {
        if (XPending(d) > 0) {
            XNextEvent(d, &ev);
            return true;
        }
        const uint64_t now = xdndNowMs();
        if (now >= deadline) return false;
        struct pollfd p = {};
        p.fd = ConnectionNumber(d);
        p.events = POLLIN;
        const int wait = (int)std::min<uint64_t>(deadline - now, 500);
        const int r = poll(&p, 1, wait);
        if (r < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (r > 0 && (p.revents & (POLLHUP | POLLERR | POLLNVAL)))
            return false;
    }
}

/* Deepest viewable window containing root coordinates (x,y). XQueryTree
 * returns children bottom-to-top, so iterate in reverse to hit the topmost. */
Window xdndWindowAt(Display *d, int x, int y)
{
    Window cur = DefaultRootWindow(d);
    for (int depth = 0; depth < 64; depth++) {
        Window root_ret = None, parent_ret = None, *kids = nullptr;
        unsigned n = 0;
        if (!XQueryTree(d, cur, &root_ret, &parent_ret, &kids, &n))
            return cur;
        Window hit = None;
        int hx = 0, hy = 0;
        for (int i = (int)n - 1; i >= 0; i--) {
            XWindowAttributes a;
            if (!XGetWindowAttributes(d, kids[i], &a)) continue;
            if (a.map_state != IsViewable) continue;
            const int bx = a.x + a.border_width;
            const int by = a.y + a.border_width;
            if (x >= bx && x < bx + a.width &&
                y >= by && y < by + a.height) {
                hit = kids[i];
                hx = x - bx;
                hy = y - by;
                break;
            }
        }
        if (kids) XFree(kids);
        if (hit == None) return cur;
        cur = hit;
        x = hx;
        y = hy;
    }
    return cur;
}

/* Only hand the data to a known file manager (or a desktop provided by one).
 * Other XDND-aware apps (terminals, editors, browsers) may accept the URI
 * list without copying it, so treating their XdndFinished as a copy would
 * delete the staging tree while the app still references those paths. */
bool xdndIsFileManager(Display *d, Window w)
{
    static const char *const classes[] = {
        "nautilus", "org.gnome.nautilus", "dolphin", "org.kde.dolphin",
        "thunar", "pcmanfm", "pcmanfm-qt", "nemo", "caja", "krusader",
        "spacefm", "rox-filer", "rox", "doublecmd", "sunflower",
        "plasmashell", "xfdesktop"
    };
    const Window root = DefaultRootWindow(d);
    for (int depth = 0; depth < 64 && w != None; depth++) {
        XClassHint hint = {};
        if (XGetClassHint(d, w, &hint)) {
            std::string cls, inst;
            if (hint.res_class) {
                cls.assign(hint.res_class);
                XFree(hint.res_class);
            }
            if (hint.res_name) {
                inst.assign(hint.res_name);
                XFree(hint.res_name);
            }
            for (char &c : cls)
                if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            for (char &c : inst)
                if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            for (const char *name : classes)
                if (cls == name || inst == name) return true;
        }
        if (w == root) break;
        Window root_ret = None, parent_ret = None, *kids = nullptr;
        unsigned n = 0;
        if (!XQueryTree(d, w, &root_ret, &parent_ret, &kids, &n)) break;
        if (kids) XFree(kids);
        if (parent_ret == None || parent_ret == w) break;
        w = parent_ret;
    }
    return false;
}

bool xdndFindTarget(Display *d, int root_x, int root_y, Window &target,
                    int &version)
{
    const Atom aware = XInternAtom(d, "XdndAware", False);
    const Window root = DefaultRootWindow(d);
    Window w = xdndWindowAt(d, root_x, root_y);
    for (int depth = 0; depth < 64 && w != None; depth++) {
        Atom actual = 0;
        int fmt = 0;
        unsigned long n = 0, rem = 0;
        unsigned char *prop = nullptr;
        if (XGetWindowProperty(d, w, aware, 0, 1, False, AnyPropertyType,
                               &actual, &fmt, &n, &rem, &prop) == Success &&
            prop) {
            if (fmt == 32 && n >= 1) {
                version = (int)((long *)prop)[0];
                XFree(prop);
                if (version >= 2) {
                    target = w;
                    return true;
                }
                return false;
            }
            XFree(prop);
        }
        if (w == root) break;
        Window root_ret = None, parent_ret = None, *kids = nullptr;
        unsigned nk = 0;
        if (!XQueryTree(d, w, &root_ret, &parent_ret, &kids, &nk)) break;
        if (kids) XFree(kids);
        if (parent_ret == None || parent_ret == w) break;
        w = parent_ret;
    }
    return false;
}

void xdndSend(Display *d, Window dst, Atom type, long l0, long l1, long l2,
              long l3, long l4)
{
    XEvent ev = {};
    ev.xclient.type = ClientMessage;
    ev.xclient.display = d;
    ev.xclient.window = dst;
    ev.xclient.message_type = type;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = l0;
    ev.xclient.data.l[1] = l1;
    ev.xclient.data.l[2] = l2;
    ev.xclient.data.l[3] = l3;
    ev.xclient.data.l[4] = l4;
    XSendEvent(d, dst, False, NoEventMask, &ev);
    XFlush(d);
}

void xdndServeSelection(Display *d, const XSelectionRequestEvent &r,
                        Atom uri_atom, const std::string &uris)
{
    XSelectionEvent n = {};
    n.type = SelectionNotify;
    n.display = d;
    n.requestor = r.requestor;
    n.selection = r.selection;
    n.target = r.target;
    n.time = r.time;
    n.property = None;
    const Atom targets_atom = XInternAtom(d, "TARGETS", False);
    if (r.property != None) {
        if (r.target == targets_atom) {
            Atom types[2] = {uri_atom, XA_STRING};
            XChangeProperty(d, r.requestor, r.property, XA_ATOM, 32,
                            PropModeReplace, (unsigned char *)types, 2);
            n.property = r.property;
        } else if (r.target == uri_atom || r.target == XA_STRING) {
            const Atom type = r.target == XA_STRING ? XA_STRING : uri_atom;
            XChangeProperty(d, r.requestor, r.property, type, 8,
                            PropModeReplace,
                            (const unsigned char *)uris.data(),
                            (int)uris.size());
            n.property = r.property;
        }
    }
    XSendEvent(d, r.requestor, False, NoEventMask, (XEvent *)&n);
    XFlush(d);
}

std::string xdndUriEscape(const std::string &path)
{
    static const char hex[] = "0123456789ABCDEF";
    std::string out = "file://";
    for (unsigned char c : path) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '/' || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            out.push_back((char)c);
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 15]);
        }
    }
    return out;
}

/* Drop the staged top-level items on the XDND-aware window at (root_x,
 * root_y). Returns true only when the target confirmed action=copy with
 * XdndFinished, i.e. it has finished copying the data. */
bool xdndDropStaged(const std::string &staging,
                    const std::vector<std::string> &roots, int root_x,
                    int root_y, std::string &why)
{
    static std::once_flag once;
    std::call_once(once, [] { XInitThreads(); });

    why.clear();
    Display *d = XOpenDisplay(nullptr);
    if (!d) {
        why = "no X display";
        return false;
    }
    const Window root = DefaultRootWindow(d);
    const Atom sel = XInternAtom(d, "XdndSelection", False);
    const Atom enter = XInternAtom(d, "XdndEnter", False);
    const Atom position = XInternAtom(d, "XdndPosition", False);
    const Atom status = XInternAtom(d, "XdndStatus", False);
    const Atom leave = XInternAtom(d, "XdndLeave", False);
    const Atom drop = XInternAtom(d, "XdndDrop", False);
    const Atom finished = XInternAtom(d, "XdndFinished", False);
    const Atom action_copy = XInternAtom(d, "XdndActionCopy", False);
    const Atom uri_atom = XInternAtom(d, "text/uri-list", False);
    const Atom time_prop = XInternAtom(d, "MivaraXdndTime", False);

    Window target = None;
    int target_version = 0;
    Window owner = None;
    Time stamp = CurrentTime;
    bool ok = false;

    do {
        if (root_x < 0 || root_y < 0 ||
            !xdndFindTarget(d, root_x, root_y, target, target_version)) {
            why = "no XDND target at the release point";
            break;
        }
        if (!xdndIsFileManager(d, target)) {
            why = "no file manager at the release point";
            break;
        }
        std::string uris;
        for (const std::string &r : roots) {
            uris += xdndUriEscape(staging + "/" + r);
            uris += "\r\n";
        }
        if (uris.empty()) {
            why = "empty selection";
            break;
        }

        owner = XCreateSimpleWindow(d, root, -10, -10, 1, 1, 0, 0, 0);
        if (owner == None) {
            why = "cannot create the XDND source window";
            break;
        }
        XSelectInput(d, owner, PropertyChangeMask);
        unsigned char dummy = 0;
        XChangeProperty(d, owner, time_prop, XA_STRING, 8, PropModeAppend,
                        &dummy, 0);
        XEvent ev = {};
        if (xdndNextEvent(d, ev, xdndNowMs() + 1000) &&
            ev.type == PropertyNotify && ev.xproperty.window == owner)
            stamp = ev.xproperty.time;
        XSetSelectionOwner(d, sel, owner, stamp);
        if (XGetSelectionOwner(d, sel) != owner) {
            why = "cannot own XdndSelection";
            break;
        }

        xdndSend(d, target, enter, (long)owner,
                 (long)std::min(target_version, 5) << 24, (long)uri_atom,
                 0, 0);
        xdndSend(d, target, position, (long)owner, 0,
                 (long)(((root_x & 0xFFFF) << 16) | (root_y & 0xFFFF)),
                 (long)stamp, (long)action_copy);

        bool sent_drop = false;
        const uint64_t deadline = xdndNowMs() + 120000;
        for (;;) {
            if (!xdndNextEvent(d, ev, deadline)) {
                why = sent_drop ? "target did not confirm the drop"
                                : "target did not accept the drop";
                break;
            }
            if (ev.type == SelectionRequest) {
                xdndServeSelection(d, ev.xselectionrequest, uri_atom, uris);
                continue;
            }
            if (ev.type == SelectionClear) {
                why = "selection was taken away";
                break;
            }
            if (ev.type != ClientMessage) continue;
            if (ev.xclient.message_type == status) {
                if ((Window)ev.xclient.data.l[0] != target) continue;
                if (!(ev.xclient.data.l[1] & 1)) {
                    why = "target rejected the drop";
                    break;
                }
                if (!sent_drop) {
                    xdndSend(d, target, drop, (long)owner, 0, (long)stamp,
                             0, 0);
                    sent_drop = true;
                }
                continue;
            }
            if (ev.xclient.message_type == leave) {
                why = "target cancelled the drop";
                break;
            }
            if (ev.xclient.message_type == finished) {
                if (ev.xclient.data.l[1] & 1) {
                    ok = true;
                } else {
                    why = "target failed to commit the drop";
                }
                break;
            }
        }
    } while (false);

    if (owner != None) {
        if (ok) XSetSelectionOwner(d, sel, None, stamp);
        XDestroyWindow(d, owner);
    }
    XFlush(d);
    XCloseDisplay(d);
    return ok;
}
#else
bool xdndDropStaged(const std::string &, const std::vector<std::string> &,
                    int, int, std::string &why)
{
    why = "XDND is only available on Linux/X11";
    return false;
}
#endif

#if defined(__linux__)
/* Reverse transfers are resolved from the receiving controller's own cursor,
 * exactly like the Windows controller's GetCursorPos path. */
bool localPointerX11(int &x, int &y)
{
    static std::once_flag once;
    std::call_once(once, [] { XInitThreads(); });
    Display *d = XOpenDisplay(nullptr);
    if (!d) return false;
    const Window root = DefaultRootWindow(d);
    Window root_ret = None, child = None;
    int rx = 0, ry = 0, wx = 0, wy = 0;
    unsigned mask = 0;
    const Bool ok = XQueryPointer(d, root, &root_ret, &child, &rx, &ry,
                                  &wx, &wy, &mask);
    XCloseDisplay(d);
    if (!ok) return false;
    x = rx;
    y = ry;
    return true;
}
#else
bool localPointerX11(int &, int &)
{
    return false;
}
#endif

void dropWorker(drop_sock_t s, const std::string &peer,
                const std::atomic<bool> &running, const DropHello &pre)
{
    setTimeouts(s, 30000, 30000);
    enableKeepAlive(s);

    DropHello hello = pre;
    if (ntohl(hello.magic) != DROP_MAGIC ||
        ntohl(hello.version) != DROP_VERSION) {
        sendStatus(s, DROP_ERR_VERSION);
        dropClose(s);
        return;
    }
    if (!dropGateCheck(hello.token)) {
        sendStatus(s, DROP_ERR_AUTH);
        dropClose(s);
        return;
    }

    const uint32_t item_count = ntohl(hello.item_count);
    const uint64_t total_bytes = ntoh64(hello.total_bytes);
    uint64_t received = 0;
    if (item_count > DROP_MAX_ITEMS || total_bytes > DROP_MAX_TOTAL_BYTES) {
        sendStatus(s, DROP_ERR_LIMIT);
        dropClose(s);
        return;
    }

    DropReply ok = {};
    ok.magic = htonl(DROP_MAGIC);
    ok.version = htonl(DROP_VERSION);
    ok.status = htonl(DROP_OK);
    if (!sendAll(s, &ok, sizeof(ok))) {
        dropClose(s);
        return;
    }

    dropStat("event=drop|state=receiving|items=" + std::to_string(item_count) +
             "|bytes=" + std::to_string(total_bytes));

    std::string why;
    const std::string staging = stagingRoot(ntohl(hello.transfer_id));
    if (staging.empty()) {
        sendResult(s, DROP_ERR_DISK, 0, 0, true, "staging unavailable");
        dropClose(s);
        return;
    }
    if (!enoughSpace(staging, total_bytes)) {
        sendResult(s, DROP_ERR_DISK, 0, 0, true, "staging disk is full");
        removeTree(staging);
        dropClose(s);
        return;
    }

    DropFrame h = {};
    std::vector<uint8_t> payload;
    std::vector<ManifestEntry> man;
    uint64_t declared = 0;
    if (!recvFrame(s, h, payload, running, 30000) ||
        ntohs(h.type) != DF_MANIFEST ||
        !parseManifest(payload, man, why, declared, DROP_MAX_ITEMS,
                       DROP_MAX_TOTAL_BYTES)) {
        const uint32_t st = why.empty() ? DROP_ERR_IO : DROP_ERR_PATH;
        sendResult(s, st, 0, 0, true,
                   why.empty() ? "missing manifest" : why);
        removeTree(staging);
        dropClose(s);
        return;
    }
    if (declared > total_bytes) {
        sendResult(s, DROP_ERR_LIMIT, 0, 0, true,
                   "manifest exceeds hello");
        removeTree(staging);
        dropClose(s);
        return;
    }

    const uint32_t rx =
        receiveFrames(s, hello, man, staging, running, why, received);
    if (rx != DROP_OK) {
        sendResult(s, rx, 0, 0, true, why);
        dropStat("event=drop|state=error|msg=" + statSafe(why));
        removeTree(staging);
        dropClose(s);
        return;
    }
    if (!dropGateCheck(hello.token)) {
        sendResult(s, DROP_ERR_AUTH, 0, 0, true, "session ended");
        removeTree(staging);
        dropClose(s);
        return;
    }

    std::vector<std::string> roots;
    for (const ManifestEntry &e : man)
        if (e.rel.find('/') == std::string::npos) roots.push_back(e.rel);
    uint32_t committed = 0, failed = 0;
    std::string msg;
    bool fallback = true;
    const int release_x = (int)(int32_t)ntohl((uint32_t)hello.x);
    const int release_y = (int)(int32_t)ntohl((uint32_t)hello.y);
    const bool reverse = (ntohl(hello.flags) & DROP_FLAG_REVERSE) != 0;
    /* Forward: the controller reports (0,0) before it has ever moved the
     * remote cursor; that is not a meaningful release point. Reverse: the
     * receiving controller resolves the point from its own cursor at drop
     * time (a reverse hello carries no useful x/y). */
    int point_x = release_x, point_y = release_y;
    bool have_point = !reverse && (release_x != 0 || release_y != 0);
    if (reverse) {
        int lx = 0, ly = 0;
        if (localPointerX11(lx, ly)) {
            point_x = lx;
            point_y = ly;
            have_point = true;
        }
    }
    if (!roots.empty() && have_point &&
        xdndDropStaged(staging, roots, point_x, point_y, why)) {
        committed = (uint32_t)roots.size();
        fallback = false;
        msg = "dropped on the target window";
        dropStat("event=drop|state=target|fallback=0|x=" +
                 std::to_string(point_x) + "|y=" +
                 std::to_string(point_y));
    } else {
        if (have_point)
            dropStat("event=drop|state=xdnd|fallback=1|msg=" +
                     statSafe(why));
        std::string dest;
        if (!defaultRecvDir(dest)) {
            sendResult(s, DROP_ERR_NO_TARGET, 0, 0, true,
                       "cannot create a receive directory");
            removeTree(staging);
            dropClose(s);
            return;
        }
        dropStat("event=drop|state=target|fallback=1|x=" +
                 std::to_string(point_x) + "|y=" +
                 std::to_string(point_y) + "|dir=" + statSafe(dest));
        if (!msg.empty()) msg += "; ";
        commitItems(staging, dest, roots, committed, failed, msg);
    }
    const uint32_t status =
        (committed > 0 && failed == 0) ? DROP_OK : DROP_ERR_IO;
    sendResult(s, status, committed, failed, fallback, msg);
    dropStat(std::string("event=drop|state=") +
             (status == DROP_OK ? "done" : "error") +
             "|items=" + std::to_string(committed) +
             "|failed=" + std::to_string(failed) +
             "|fallback=" + (fallback ? "1" : "0") +
             (msg.empty() ? "" : "|msg=" + statSafe(msg)));
    removeTree(staging);
    dropClose(s);
    std::cout << "[drop] transfer from " << peer << " committed "
              << committed << "/" << man.size() << "\n"
              << std::flush;
}

/* ── gate / startup ──────────────────────────────────────────────────────── */

std::mutex g_gate_m;
uint8_t g_gate_token[DROP_TOKEN_BYTES] = {};
uint64_t g_gate_epoch = 0;
bool g_gate_active = false;

std::atomic<int> g_start_state{0};
std::atomic<int> g_start_stage{0};
std::atomic<int> g_start_error{0};
std::atomic<int> g_start_port{(int)DROP_PORT};

void reportStartup(int state, int stage, int error)
{
    g_start_error.store(error);
    g_start_stage.store(stage);
    g_start_state.store(state);
}

} /* namespace */

DropStartupStatus dropServiceStatus()
{
    DropStartupStatus st = {};
    st.state = g_start_state.load();
    st.stage = g_start_stage.load();
    st.error_code = g_start_error.load();
    st.port = (uint16_t)g_start_port.load();
    return st;
}

void dropServiceStatusReset()
{
    reportStartup(0, 0, 0);
}

bool dropGateInstall(const uint8_t token[DROP_TOKEN_BYTES], uint64_t epoch)
{
    if (!token) return false;
    std::lock_guard<std::mutex> lk(g_gate_m);
    std::memcpy(g_gate_token, token, DROP_TOKEN_BYTES);
    g_gate_epoch = epoch;
    g_gate_active = true;
    return true;
}

void dropGateClear(uint64_t epoch)
{
    std::lock_guard<std::mutex> lk(g_gate_m);
    if (g_gate_active && g_gate_epoch == epoch) {
        mivaraSecureZero(g_gate_token, sizeof(g_gate_token));
        g_gate_epoch = 0;
        g_gate_active = false;
    }
}

bool dropGateCheck(const uint8_t token[DROP_TOKEN_BYTES])
{
    if (!token) return false;
    std::lock_guard<std::mutex> lk(g_gate_m);
    const bool ok = g_gate_active &&
                    mivaraConstEq(g_gate_token, token, DROP_TOKEN_BYTES);
    if (!ok) {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "event=drop|state=gate_reject|port=%d|active=%d|"
                      "epoch=%llu",
                      (int)g_start_port.load(), g_gate_active ? 1 : 0,
                      (unsigned long long)g_gate_epoch);
        dropStat(buf);
    }
    return ok;
}

void dropServiceRun(std::atomic<bool> &running, uint16_t port)
{
    reportStartup(0, 0, 0);
    g_start_port.store(port ? port : (uint16_t)DROP_PORT);
    cleanupStaging();
    const int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) {
        reportStartup(-1, 1, errno);
        std::cout << "[drop] socket init failed; file drop disabled\n"
                  << std::flush;
        return;
    }
    int reuse = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)g_start_port.load());
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(srv, (sockaddr *)&addr, sizeof(addr)) < 0) {
        reportStartup(-1, 3, errno);
        std::cout << "[drop] bind failed on " << g_start_port.load()
                  << "; file drop disabled\n" << std::flush;
        dropClose(srv);
        return;
    }
    if (listen(srv, 4) < 0) {
        reportStartup(-1, 4, errno);
        dropClose(srv);
        return;
    }
    reportStartup(1, 4, 0);
    std::cout << "[drop] file-drop listener on TCP " << g_start_port.load()
              << "\n" << std::flush;

    std::atomic<bool> active{false};
    std::thread worker;
    while (running.load()) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(srv, &rd);
        struct timeval tv = {1, 0};
        const int sel = select(srv + 1, &rd, nullptr, nullptr, &tv);
        if (sel <= 0) continue;
        sockaddr_in cli = {};
        socklen_t len = sizeof(cli);
        const int c = accept(srv, (sockaddr *)&cli, &len);
        if (c < 0) continue;
        char ip[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &cli.sin_addr, ip, sizeof(ip));

        if (active.load()) {
            DropHello hello = {};
            setTimeouts(c, 2000, 2000);
            recvAllTimeout(c, &hello, sizeof(hello), running, 2000);
            sendStatus(c, DROP_ERR_BUSY);
            dropClose(c);
            continue;
        }
        if (worker.joinable()) worker.join();

        setTimeouts(c, 3000, 3000);
        DropHello hello = {};
        if (recvAllTimeout(c, &hello, sizeof(hello), running, 3000) != 1) {
            dropClose(c);
            continue;
        }
        if (ntohl(hello.magic) != DROP_MAGIC ||
            ntohl(hello.version) != DROP_VERSION) {
            dropStat("event=drop|state=reject|stage=version");
            sendStatus(c, DROP_ERR_VERSION);
            dropClose(c);
            continue;
        }
        if (!dropGateCheck(hello.token)) {
            dropStat("event=drop|state=reject|stage=auth");
            sendStatus(c, DROP_ERR_AUTH);
            dropClose(c);
            continue;
        }
        if (active.exchange(true)) {
            dropStat("event=drop|state=reject|stage=busy");
            sendStatus(c, DROP_ERR_BUSY);
            dropClose(c);
            continue;
        }
        worker = std::thread([c, ip, &running, &active, hello] {
            dropWorker(c, std::string(ip), running, hello);
            active = false;
        });
    }
    if (worker.joinable()) worker.join();
    dropClose(srv);
    std::cout << "[drop] file-drop listener stopped\n" << std::flush;
}

DropResult dropSendFiles(const DropOptions &opt,
                         const std::vector<std::string> &paths,
                         const uint8_t token[DROP_TOKEN_BYTES],
                         const std::atomic<bool> &running,
                         void (*progress)(uint64_t, uint64_t, void *),
                         void *progress_ctx)
{
    DropResult res;
    if (!opt.peer_ip || !*opt.peer_ip || !token) {
        res.status = DROP_ERR_AUTH;
        res.message = "missing peer or token";
        return res;
    }
    ScanState st;
    if (!scanPaths(paths, st)) {
        res.status = DROP_ERR_LIMIT;
        res.message = st.err;
        return res;
    }
    if (st.entries.empty()) {
        res.status = DROP_ERR_PATH;
        res.message = "empty selection";
        return res;
    }

    drop_sock_t s = connectTimeout(opt.peer_ip, opt.port, running, 5000);
    if (s < 0) {
        res.status = DROP_ERR_IO;
        res.message = "cannot connect to the file-drop port";
        return res;
    }
    setTimeouts(s, 30000, 2000);
    enableKeepAlive(s);

    DropHello hello = {};
    hello.magic = htonl(DROP_MAGIC);
    hello.version = htonl(DROP_VERSION);
    std::memcpy(hello.token, token, DROP_TOKEN_BYTES);
    hello.transfer_id =
        htonl(opt.transfer_id ? opt.transfer_id
                              : (uint32_t)(time(nullptr) ^
                                           ((uint32_t)getpid() << 16)));
    hello.x = (int32_t)htonl((uint32_t)opt.x);
    hello.y = (int32_t)htonl((uint32_t)opt.y);
    hello.item_count = htonl((uint32_t)st.entries.size());
    hello.total_bytes = hton64(st.total);
    hello.flags = htonl(opt.flags);
    if (!sendAll(s, &hello, sizeof(hello))) {
        dropClose(s);
        res.status = DROP_ERR_IO;
        res.message = "handshake send failed";
        return res;
    }
    DropReply reply = {};
    if (recvAllTimeout(s, &reply, sizeof(reply), running, 10000) != 1 ||
        ntohl(reply.magic) != DROP_MAGIC ||
        ntohl(reply.version) != DROP_VERSION) {
        dropClose(s);
        res.status = DROP_ERR_IO;
        res.message = "handshake reply missing";
        return res;
    }
    if (ntohl(reply.status) != DROP_OK) {
        dropClose(s);
        res.status = ntohl(reply.status);
        res.message = "target refused the transfer";
        return res;
    }
    if (!sendManifest(s, st.entries)) {
        dropClose(s);
        res.status = DROP_ERR_IO;
        res.message = "manifest send failed";
        return res;
    }

    uint64_t sent_total = 0;
    for (size_t i = 0; i < st.entries.size(); i++) {
        const Entry &e = st.entries[i];
        if (e.dir) continue;
        if (!running.load()) {
            sendAbort(s, DROP_ERR_CANCELLED, "cancelled");
            dropClose(s);
            res.status = DROP_ERR_CANCELLED;
            res.message = "cancelled";
            return res;
        }
        const int f = open(e.abs.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (f < 0) {
            sendAbort(s, DROP_ERR_IO, "cannot read source");
            dropClose(s);
            res.status = DROP_ERR_IO;
            res.message = "cannot read source: " + e.abs;
            return res;
        }
        if (!fileUnchanged(f, e)) {
            close(f);
            sendAbort(s, DROP_ERR_IO, "source changed");
            dropClose(s);
            res.status = DROP_ERR_IO;
            res.message = "source changed during transfer";
            return res;
        }
        std::vector<uint8_t> begin;
        appendU32(begin, (uint32_t)i);
        appendU64(begin, e.size);
        if (!sendFrame(s, DF_FILE_BEGIN, begin.data(),
                       (uint32_t)begin.size())) {
            close(f);
            dropClose(s);
            res.status = DROP_ERR_IO;
            res.message = "file begin send failed";
            return res;
        }
        MivaraSha256Ctx hash;
        mivaraSha256Init(&hash);
        uint64_t off = 0;
        std::vector<uint8_t> buf(64 * 1024);
        for (;;) {
            const ssize_t got = read(f, buf.data(), buf.size());
            if (got < 0) {
                if (errno == EINTR) continue;
                close(f);
                sendAbort(s, DROP_ERR_IO, "read failed");
                dropClose(s);
                res.status = DROP_ERR_IO;
                res.message = "source read failed";
                return res;
            }
            if (got == 0) break;
            if (!running.load()) {
                close(f);
                sendAbort(s, DROP_ERR_CANCELLED, "cancelled");
                dropClose(s);
                res.status = DROP_ERR_CANCELLED;
                res.message = "cancelled";
                return res;
            }
            if (off + (uint64_t)got > e.size) {
                close(f);
                sendAbort(s, DROP_ERR_IO, "source grew");
                dropClose(s);
                res.status = DROP_ERR_IO;
                res.message = "source changed during transfer";
                return res;
            }
            std::vector<uint8_t> data;
            data.reserve(12 + (size_t)got);
            appendU32(data, (uint32_t)i);
            appendU64(data, off);
            data.insert(data.end(), buf.begin(), buf.begin() + got);
            if (!sendFrame(s, DF_FILE_DATA, data.data(),
                           (uint32_t)data.size())) {
                close(f);
                dropClose(s);
                res.status = DROP_ERR_IO;
                res.message = "data send failed";
                return res;
            }
            mivaraSha256Update(&hash, buf.data(), (size_t)got);
            off += (uint64_t)got;
            sent_total += (uint64_t)got;
            if (progress) progress(sent_total, st.total, progress_ctx);
        }
        const bool unchanged = fileUnchanged(f, e);
        close(f);
        if (off != e.size || !unchanged) {
            sendAbort(s, DROP_ERR_IO, "source changed");
            dropClose(s);
            res.status = DROP_ERR_IO;
            res.message = "source changed during transfer";
            return res;
        }
        uint8_t digest[32] = {};
        mivaraSha256Final(&hash, digest);
        std::vector<uint8_t> end;
        appendU32(end, (uint32_t)i);
        end.insert(end.end(), digest, digest + 32);
        if (!sendFrame(s, DF_FILE_END, end.data(), (uint32_t)end.size())) {
            dropClose(s);
            res.status = DROP_ERR_IO;
            res.message = "file end send failed";
            return res;
        }
    }

    std::vector<uint8_t> done;
    appendU32(done, (uint32_t)st.entries.size());
    if (!sendFrame(s, DF_DONE, done.data(), (uint32_t)done.size())) {
        dropClose(s);
        res.status = DROP_ERR_IO;
        res.message = "done send failed";
        return res;
    }

    DropFrame h = {};
    std::vector<uint8_t> payload;
    if (!recvFrame(s, h, payload, running, 600000) ||
        ntohs(h.type) != DF_RESULT) {
        dropClose(s);
        res.status = DROP_ERR_IO;
        res.message = "result missing";
        return res;
    }
    ByteCursor c{payload.data(), payload.size()};
    res.status = c.u32();
    res.committed = c.u32();
    res.failed = c.u32();
    const uint32_t flags = c.u32();
    const uint32_t n = c.u32();
    res.message = c.bytes(n);
    res.fallback = (flags & 1u) != 0;
    if (!c.ok) {
        dropClose(s);
        res.status = DROP_ERR_IO;
        res.committed = 0;
        res.failed = 0;
        res.message = "malformed result";
        return res;
    }
    dropClose(s);
    return res;
}

#endif /* _WIN32 */
