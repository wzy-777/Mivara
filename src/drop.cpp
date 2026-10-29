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
 * point. Nothing is overwritten and the source is never modified.
 */
#include "drop.h"
#include "auth.h"
#include "dropshell.h"

#include <algorithm>
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
#  include <unistd.h>
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

struct Cursor {
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
    Cursor c{payload.data(), payload.size()};
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
        Cursor c{payload.data(), payload.size()};

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
    if (!dropResolveFolder(drop_x, drop_y, dest, why)) {
        if (!dropDefaultRecvDir(dest)) {
            sendResult(s, DROP_ERR_NO_TARGET, 0, 0, false, why);
            closesocket(s);
            CoUninitialize();
            return;
        }
        fallback = true;
    }

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
    st.port = (uint16_t)DROP_PORT;
    return st;
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
    return g_gate_active && mivaraConstEq(g_gate_token, token, DROP_TOKEN_BYTES);
}

void dropServiceRun(std::atomic<bool> &running)
{
    reportStartup(0, 0, 0);
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
    addr.sin_port = htons((u_short)DROP_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(srv, (sockaddr *)&addr, sizeof(addr)) < 0) {
        reportStartup(-1, 3, WSAGetLastError());
        std::cout << "[drop] bind failed on " << DROP_PORT
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
    std::cout << "[drop] file-drop listener on TCP " << DROP_PORT << "\n"
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
            sendStatus(c, DROP_ERR_VERSION);
            closesocket(c);
            continue;
        }
        if (!dropGateCheck(hello.token)) {
            sendStatus(c, DROP_ERR_AUTH);
            closesocket(c);
            continue;
        }
        if (active.exchange(true)) {
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
    Cursor c{payload.data(), payload.size()};
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

DropStartupStatus dropServiceStatus()
{
    DropStartupStatus st = {};
    st.state = 2;
    st.port = (uint16_t)DROP_PORT;
    return st;
}

void dropServiceRun(std::atomic<bool> &) {}
bool dropGateInstall(const uint8_t *, uint64_t) { return false; }
void dropGateClear(uint64_t) {}
bool dropGateCheck(const uint8_t *) { return false; }

DropResult dropSendFiles(const DropOptions &, const std::vector<std::string> &,
                         const uint8_t *, const std::atomic<bool> &,
                         void (*)(uint64_t, uint64_t, void *), void *)
{
    DropResult r;
    r.status = DROP_ERR_UNSUPPORTED;
    r.message = "file drop is Windows only";
    return r;
}

#endif /* _WIN32 */
