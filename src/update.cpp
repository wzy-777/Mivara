/**
 * UPDATE.CPP – in-app update check + one-click updater (GUI app only)
 *
 * Design notes (see AGENTS.md / README):
 *  - The check uses the web redirect https://github.com/wzy-777/Mivara/releases/latest
 *    and reads the tag out of the 302 Location header. api.github.com is
 *    deliberately NOT used: unauthenticated requests are limited to 60/h per IP.
 *  - Windows and Linux download the package themselves; macOS (and POSIX
 *    builds on architectures without release artifacts) only checks and hands
 *    the release page to the desktop browser.
 *  - Trust chain for the executed installer: HTTPS with TLS verification and
 *    no https->http downgrade on both platforms; the tag is validated down to
 *    a fixed charset before it is used to build a URL or file name; the
 *    download must match the SHA-256 published in the release's
 *    SHA256SUMS.txt / SHA256SUMS-linux.txt (no sums file, or no line for this
 *    architecture => no in-app install), and the hash is re-verified on disk
 *    immediately before the installer is handed over. Windows additionally
 *    requires the final URL after redirects to be on a GitHub-owned host.
 *    The Windows installer is written into %ProgramData%\Mivara\Updates with a
 *    DACL that denies write access to non-administrators, so a
 *    medium-integrity process cannot plant or swap the binary we execute.
 *    Linux stages the package in a 0700 $XDG_CACHE_HOME/mivara/updates and
 *    verifies SHA256SUMS-linux.txt; the detached helper re-checks the package
 *    hash immediately before apt-get/install.sh sees it.
 *  - Only github.com/wzy-777/Mivara is ever contacted; only a User-Agent is
 *    sent; nothing is uploaded.
 *
 * Local diagnostics (never built by the makefile, cf. ports.cpp -DPORTS_MAIN):
 *   g++ -std=c++17 -DWIN32_LEAN_AND_MEAN -DUPDATE_TEST_MAIN src/update.cpp \
 *       -o bin/upd_test.exe -lwinhttp -lshell32 -lbcrypt -ladvapi32
 *   g++ -std=c++17 -DUPDATE_TEST_MAIN src/update.cpp -o bin/upd_test
 *   bin/upd_test.exe unit | check | download <tag> [asset] | hash <file>
 */
#include "update.h"
#include "version.h"

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef _WIN32_WINNT
#    define _WIN32_WINNT 0x0601
#  endif
#  include <windows.h>
#  include <aclapi.h>
#  include <bcrypt.h>
#  include <sddl.h>
#  include <shellapi.h>
#  include <shlobj.h>
#  include <winhttp.h>
#else
#  include <cerrno>
#  include <climits>
#  include <dirent.h>
#  include <fcntl.h>
#  include <signal.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

/* The Linux in-app updater is built only for the architectures the release
 * pipeline publishes. Other POSIX builds (macOS, exotic arches) keep the
 * check + browser hand-off path. */
#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
#  define MIVARA_LINUX_UPDATER 1
#endif

namespace {

/* ── fixed endpoints ─────────────────────────────────────────────────────── */
const char *const kLatestUrl   = "https://github.com/wzy-777/Mivara/releases/latest";
const char *const kTagPrefix   = "https://github.com/wzy-777/Mivara/releases/tag/";
const char *const kPathPrefix  = "/wzy-777/Mivara/releases/download/";
const char *const kAssetName   = "Mivara-Setup.exe";      /* fixed-name asset */
const char *const kSumsName    = "SHA256SUMS.txt";
const long long   kMaxAsset    = 512LL * 1024 * 1024;     /* sanity cap */
const long long   kMaxSums     = 64LL * 1024;

#ifdef MIVARA_LINUX_UPDATER
const char *const kLinuxSumsName = "SHA256SUMS-linux.txt";
#  if defined(__x86_64__)
const char *const kLinuxArch = "x86_64";
const char *const kDebArch   = "amd64";
#  else
const char *const kLinuxArch = "aarch64";
const char *const kDebArch   = "arm64";
#  endif
#endif

/* ── worker state (single worker thread; UI polls) ───────────────────────── */
std::mutex        s_mtx;              /* guards s_info only */
UpdateInfo        s_info;
std::atomic<bool> s_running{false};
std::atomic<bool> s_done{false};
std::atomic<bool> s_abort{false};
std::atomic<int>  s_percent{-1};
std::atomic<bool> s_launched{false};   /* the installer was handed over already */
std::mutex        s_reqMtx;            /* guards s_activeReq */
#ifdef _WIN32
HINTERNET         s_activeReq = nullptr;   /* request currently in flight */
#endif

/* Owns the worker thread. Declared after the state it touches so that it is
 * destroyed first: a joinable std::thread at static-destruction time would
 * call std::terminate, and this must hold for every possible exit path - not
 * only the one that remembers to call updateJoin(). */
struct Worker {
    std::thread th;
    ~Worker()
    {
        s_abort = true;
        if (th.joinable()) th.join();
    }
};
Worker s_worker;

/* ── small string helpers ────────────────────────────────────────────────── */
std::string lower(const std::string &s)
{
    std::string o = s;
    for (char &c : o) c = (char)std::tolower((unsigned char)c);
    return o;
}

std::string stripV(const std::string &tag)
{
    return (!tag.empty() && (tag[0] == 'v' || tag[0] == 'V')) ? tag.substr(1) : tag;
}

bool isHex64(const std::string &s)
{
    if (s.size() != 64) return false;
    for (char c : s)
        if (!std::isxdigit((unsigned char)c)) return false;
    return true;
}

/* Strict numeric version: 2..4 dot-separated components of 1..4 digits each
 * without leading zeros; an optional -pre/+build suffix is accepted but
 * ignored for the comparison. Tolerance is deliberate: a maintainer typo in a
 * tag must not blind every client (a rejected tag = "no update" forever). */
bool parseVersion(const std::string &s, int parts[4], int &n)
{
    n = 0;
    size_t i = 0;
    for (;;) {
        if (n >= 4) return false;
        size_t start = i;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') i++;
        size_t len = i - start;
        if (len == 0 || len > 4) return false;
        if (len > 1 && s[start] == '0') return false;
        int v = 0;
        for (size_t k = start; k < i; k++) v = v * 10 + (s[k] - '0');
        parts[n++] = v;
        if (i < s.size() && s[i] == '.') { i++; continue; }
        break;
    }
    if (n < 2) return false;
    if (i == s.size()) return true;
    if (s[i] != '-' && s[i] != '+') return false;
    if (i + 1 >= s.size()) return false;
    for (size_t k = i + 1; k < s.size(); k++) {
        char c = s[k];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
              (c >= 'A' && c <= 'Z') || c == '.' || c == '-' || c == '_'))
            return false;
    }
    return true;
}

int cmpVersion(const std::string &a, const std::string &b)
{
    int pa[4] = {0, 0, 0, 0}, pb[4] = {0, 0, 0, 0}, na = 0, nb = 0;
    if (!parseVersion(a, pa, na) || !parseVersion(b, pb, nb)) return 0;
    int n = na > nb ? na : nb;
    for (int i = 0; i < n; i++) {
        if (pa[i] != pb[i]) return pa[i] < pb[i] ? -1 : 1;
    }
    return 0;
}

/* A tag is only ever used to build a URL / file name after passing this. */
bool validTagChars(const std::string &tag)
{
    if (tag.size() < 3 || tag.size() > 40) return false;
    for (char c : tag) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
              (c >= 'A' && c <= 'Z') || c == '.' || c == '-' || c == '_'))
            return false;
    }
    return true;
}

/* Update the failure text of the current state without publishing a
 * completion (used for launch failures, where no operation finished). */
#if defined(_WIN32) || defined(MIVARA_LINUX_UPDATER)
void setDetail(const std::string &detail)
{
    std::lock_guard<std::mutex> lk(s_mtx);
    s_info.detail = detail;
}
#endif

/* Closing a request handle is the documented way to cancel an in-flight
 * synchronous WinHTTP call. Without it 取消下载/退出 would have to wait out the
 * receive timeout (the read loop only checks s_abort between receives).
 * Ownership handshake: whoever clears s_activeReq closes the handle exactly
 * once - the canceller here, or the request's own destructor. */
void cancelActiveRequest()
{
#ifdef _WIN32
    HINTERNET r = nullptr;
    {
        std::lock_guard<std::mutex> lk(s_reqMtx);
        r = s_activeReq;
        s_activeReq = nullptr;
    }
    if (r) WinHttpCloseHandle(r);
#endif
}

void finishState(UpdateState st, const std::string &detail, bool isDl,
                 const std::string &tag, const std::string &path = "",
                 const std::string &sha = "")
{
    {
        std::lock_guard<std::mutex> lk(s_mtx);
        s_info = UpdateInfo{};
        s_info.state = st;
        s_info.is_download = isDl;
        s_info.remote_tag = tag;
        s_info.version = stripV(tag);
        s_info.install_path = path;
        s_info.sha256 = sha;
        s_info.detail = detail;
    }
    s_percent = -1;
    s_running = false;
    s_done = true;
}

#ifdef _WIN32

/* ── Win32 string helpers (paths are UTF-8 in our API, wide in Win32) ────── */
std::wstring u82w(const std::string &s)
{
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

std::string w2u8(const std::wstring &w)
{
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

/* RAII WinHTTP handle chain */
struct HttpChain {
    HINTERNET session = nullptr, conn = nullptr, req = nullptr;
    ~HttpChain()
    {
        if (req) {
            bool mine = false;
            {
                std::lock_guard<std::mutex> lk(s_reqMtx);
                if (s_activeReq == req) { s_activeReq = nullptr; mine = true; }
            }
            if (mine) WinHttpCloseHandle(req);  /* else the canceller closed it */
        }
        if (conn) WinHttpCloseHandle(conn);
        if (session) WinHttpCloseHandle(session);
    }
};

const char *mapNetError(unsigned long e)
{
    switch (e) {
    case ERROR_WINHTTP_NAME_NOT_RESOLVED:      return "net_dns";
    case ERROR_WINHTTP_CANNOT_CONNECT:         return "net_connect";
    case ERROR_WINHTTP_CONNECTION_ERROR:       return "net_connect";
    case ERROR_WINHTTP_TIMEOUT:                return "net_timeout";
    case ERROR_WINHTTP_SECURE_FAILURE:         return "net_tls";
    case ERROR_WINHTTP_CLIENT_AUTH_CERT_NEEDED:return "net_tls";
    default:                                   return "net_other";
    }
}

const char *abortOr(const char *e) { return s_abort ? "aborted" : e; }

/* GitHub serves releases from github.com and *.githubusercontent.com only. */
bool hostAllowed(const std::string &h)
{
    if (h == "github.com") return true;
    const std::string suf = ".githubusercontent.com";
    return h.size() > suf.size() && h.compare(h.size() - suf.size(), suf.size(), suf) == 0;
}

/* Open + send one GET to https://github.com<path>. Redirect handling is
 * explicit: the check needs the raw 302, the downloads follow but must land on
 * a GitHub host over HTTPS (verified from the final URL before any body byte
 * is read). Certificate verification is never disabled (no SECURITY_FLAG_IGNORE_*,
 * no cert callback) and no protocol mask is pinned - see the note in the body
 * explaining why schannel's own policy is left authoritative.
 * 'recvMs' is per blocking receive: it must be generous for the multi-MB asset
 * (each 64 KiB chunk gets its own budget) and short for the tiny responses. */
bool httpOpen(HttpChain &h, const std::string &path, bool follow, int recvMs,
              const char **err)
{
    std::string ua = std::string("Mivara-UpdateCheck/") + MIVARA_VERSION;
    std::wstring wua = u82w(ua);
    h.session = WinHttpOpen(wua.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!h.session)
        h.session = WinHttpOpen(wua.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!h.session) { *err = "winhttp_init"; return false; }

    /* resolve / connect / send(TLS handshake happens here) / receive.
     * Measured github.com first-contact on this machine: ~1.5 s, so the old
     * 800 ms send budget produced spurious «请求超时» on flaky links.
     *
     * Deliberately NOT calling WinHttpSetOption(WINHTTP_OPTION_SECURE_PROTOCOLS):
     * pinning the mask to TLS1.2|TLS1.3 was measured to make ~1 in 4 requests
     * stall for the full 4 s connect+send budget (ERROR_WINHTTP_TIMEOUT) on
     * this machine, and it never added a guarantee - its return value is
     * ignored and schannel's own protocol policy is authoritative anyway
     * (GitHub additionally requires TLS >= 1.2 server-side). Verification is
     * never disabled: no SECURITY_FLAG_IGNORE_*, no certificate callback. */
    WinHttpSetTimeouts(h.session, 2000, 2000, 2000, recvMs);
    h.conn = WinHttpConnect(h.session, L"github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!h.conn) { *err = mapNetError(GetLastError()); return false; }

    std::wstring wpath = u82w(path);
    h.req = WinHttpOpenRequest(h.conn, L"GET", wpath.c_str(), nullptr, WINHTTP_NO_REFERER,
                               WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!h.req) { *err = "winhttp_init"; return false; }
    {
        /* Register immediately: from here on every early return is closed by
         * the HttpChain destructor (or by a canceller), never leaked. */
        std::lock_guard<std::mutex> lk(s_reqMtx);
        s_activeReq = h.req;
    }

    DWORD df = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
    WinHttpSetOption(h.req, WINHTTP_OPTION_DISABLE_FEATURE, &df, sizeof(df));

    DWORD rp = follow ? WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP
                      : WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    if (!WinHttpSetOption(h.req, WINHTTP_OPTION_REDIRECT_POLICY, &rp, sizeof(rp))) {
        *err = "winhttp_init";   /* fail closed: the redirect guarantee depends on it */
        return false;
    }
    if (s_abort) { *err = "aborted"; return false; }
    if (!WinHttpSendRequest(h.req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        *err = abortOr(mapNetError(GetLastError()));
        return false;
    }
    if (!WinHttpReceiveResponse(h.req, nullptr)) {
        *err = abortOr(mapNetError(GetLastError()));
        return false;
    }
    return true;
}

bool httpFinalUrlOk(HttpChain &h, const char **err)
{
    wchar_t url[2048] = {0};
    DWORD len = sizeof(url);
    if (!WinHttpQueryOption(h.req, WINHTTP_OPTION_URL, url, &len) || !url[0]) {
        *err = "bad_response";
        return false;
    }
    std::string a = w2u8(url);
    if (a.compare(0, 8, "https://") != 0) { *err = "bad_response"; return false; }
    /* Take the whole authority first, then split it. Stopping the host scan at
     * the port colon would read "https://github.com:443@evil.com/x" as
     * github.com while the bytes actually come from evil.com — the authority is
     * everything up to the first '/', and userinfo is never something GitHub
     * emits, so anything containing '@' is rejected outright. */
    const char *auth = a.c_str() + 8;
    size_t al = 0;
    while (auth[al] && auth[al] != '/' && al < 255) al++;
    const std::string authority(auth, al);
    if (authority.find('@') != std::string::npos) { *err = "bad_response"; return false; }
    const size_t colon = authority.rfind(':');
    const std::string host =
        (colon == std::string::npos) ? authority : authority.substr(0, colon);
    if (!hostAllowed(host)) { *err = "bad_response"; return false; }
    return true;
}

bool httpStatusOk(HttpChain &h, long long &contentLen, const char **err)
{
    DWORD status = 0, len = sizeof(status);
    if (!WinHttpQueryHeaders(h.req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX)) {
        *err = "bad_response";
        return false;
    }
    if (status == 404) { *err = "not_found"; return false; }
    if (status != 200) { *err = "bad_response"; return false; }
    DWORD cl = 0;
    len = sizeof(cl);
    if (WinHttpQueryHeaders(h.req, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &cl, &len, WINHTTP_NO_HEADER_INDEX))
        contentLen = (long long)cl;
    else
        contentLen = -1;
    return true;
}

/* ── SHA-256 (CNG) ───────────────────────────────────────────────────────── */
struct Sha256 {
    BCRYPT_ALG_HANDLE  alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::vector<unsigned char> obj;
    bool ok = false;

    bool init()
    {
        if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0)))
            return false;
        DWORD cb = 0, cbObj = 0;
        if (!BCRYPT_SUCCESS(BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&cbObj,
                                              sizeof(cbObj), &cb, 0)))
            return false;
        obj.resize(cbObj);
        if (!BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, obj.data(), cbObj, nullptr, 0, 0)))
            return false;
        ok = true;
        return true;
    }
    void update(const void *d, size_t n)
    {
        if (ok && n) BCryptHashData(hash, (PUCHAR)d, (ULONG)n, 0);
    }
    bool finish(std::string &hex)
    {
        if (!ok) return false;
        unsigned char out[32];
        if (!BCRYPT_SUCCESS(BCryptFinishHash(hash, out, sizeof(out), 0))) return false;
        static const char *hx = "0123456789abcdef";
        hex.clear();
        for (int i = 0; i < 32; i++) { hex += hx[out[i] >> 4]; hex += hx[out[i] & 15]; }
        return true;
    }
    ~Sha256()
    {
        if (hash) BCryptDestroyHash(hash);
        if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    }
};

/* ── body streaming ──────────────────────────────────────────────────────── */
bool httpReadBody(HttpChain &h, HANDLE file, std::string *body, long long maxBody,
                  bool hash, std::string &shaHex, long long &got, long long total,
                  const char **err)
{
    Sha256 sha;
    if (hash && !sha.init()) { *err = "internal"; return false; }
    std::vector<unsigned char> buf(64 * 1024);
    for (;;) {
        if (s_abort) { *err = "aborted"; return false; }
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(h.req, &avail)) { *err = abortOr(mapNetError(GetLastError())); return false; }
        if (avail == 0) break;
        DWORD want = avail > buf.size() ? (DWORD)buf.size() : avail;
        DWORD read = 0;
        if (!WinHttpReadData(h.req, buf.data(), want, &read)) { *err = abortOr(mapNetError(GetLastError())); return false; }
        if (read == 0) break;
        if (got + (long long)read > kMaxAsset) { *err = "too_big"; return false; }
        if (body && maxBody > 0 && got + (long long)read > maxBody) { *err = "too_big"; return false; }
        if (file) {
            DWORD written = 0;
            if (!WriteFile(file, buf.data(), read, &written, nullptr) || written != read) {
                *err = "file_io";
                return false;
            }
        }
        if (body) body->append((const char *)buf.data(), read);
        if (hash) sha.update(buf.data(), read);
        got += read;
        if (total > 0) s_percent = (int)(got * 100 / total);
    }
    if (hash && !sha.finish(shaHex)) { *err = "internal"; return false; }
    return true;
}

/* Small in-memory GET (SHA256SUMS.txt). */
bool fetchSmall(const std::string &path, std::string &body, const char **err)
{
    HttpChain h;
    if (!httpOpen(h, path, true, 2000, err)) return false;
    if (!httpFinalUrlOk(h, err)) return false;
    long long clen = -1;
    if (!httpStatusOk(h, clen, err)) return false;
    if (clen > kMaxSums) { *err = "too_big"; return false; }
    long long got = 0;
    std::string dummy;
    return httpReadBody(h, nullptr, &body, kMaxSums, false, dummy, got, clen, err);
}

/* Download the installer to `file` (already opened for writing); hash while
 * streaming and require an exact Content-Length match. */
bool fetchAsset(const std::string &path, HANDLE file, std::string &shaHex,
                long long &got, const char **err)
{
    HttpChain h;
    /* 5 s per 64 KiB chunk: a stalled or slower-than-12 KiB/s link fails with a
       clear message instead of hanging the UI, but ordinary throttled links
       still complete (the check's 2 s budget would abort them). */
    if (!httpOpen(h, path, true, 5000, err)) return false;
    if (!httpFinalUrlOk(h, err)) return false;
    long long clen = -1;
    if (!httpStatusOk(h, clen, err)) return false;
    if (clen <= 0 || clen > kMaxAsset) { *err = "bad_response"; return false; }
    s_percent = 0;
    if (!httpReadBody(h, file, nullptr, 0, true, shaHex, got, clen, err)) return false;
    if (got != clen) { *err = "size_mismatch"; return false; }
    return true;
}

/* ── protected download directory ────────────────────────────────────────── */
std::string updatesDirUtf8()
{
    wchar_t base[MAX_PATH] = {0};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr, 0, base)) || !base[0])
        return std::string();
    return w2u8(std::wstring(base) + L"\\Mivara\\Updates");
}

/* Applies O:BA plus a protected DACL to a file or directory: SYSTEM and
 * Administrators full, Users read/execute. Owner and DACL are read back after
 * every change. New installers are created inside the protected directory and
 * secured individually, so a medium-integrity process cannot pre-plant one or
 * replace it between verification and launch. */
bool lockDownPath(const std::wstring &path)
{
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"O:BAD:P(A;OICI;GA;;;SY)(A;OICI;GA;;;BA)(A;OICI;GRGX;;;BU)",
            SDDL_REVISION_1, &sd, nullptr))
        return false;
    PSID owner = nullptr;
    PACL dacl = nullptr;
    BOOL present = FALSE, deflt = FALSE;
    if (!GetSecurityDescriptorOwner(sd, &owner, &deflt) ||
        !GetSecurityDescriptorDacl(sd, &present, &dacl, &deflt) || !present) {
        LocalFree(sd);
        return false;
    }
    std::wstring buf = path;   /* SetNamedSecurityInfo may write to the buffer */
    DWORD rc = SetNamedSecurityInfoW(&buf[0], SE_FILE_OBJECT,
                                     OWNER_SECURITY_INFORMATION |
                                         DACL_SECURITY_INFORMATION |
                                         PROTECTED_DACL_SECURITY_INFORMATION,
                                     owner, nullptr, dacl, nullptr);
    LocalFree(sd);
    if (rc != ERROR_SUCCESS) return false;

    /* Trust nothing: read it back. Without elevation the API can succeed while
     * silently leaving the owner unchanged (measured), and an owner keeps
     * implicit WRITE_DAC - so a directory we did not really take over must not
     * be used to stage a binary we are going to execute.
     *
     * DACL_SECURITY_INFORMATION is required here, not just OWNER: the returned
     * descriptor contains only the parts that were asked for, so an OWNER-only
     * read-back yields a control word with neither SE_DACL_PRESENT nor
     * SE_DACL_PROTECTED set. Testing the protected bit against it is false even
     * when the DACL really is protected - measured on Windows 11 26300 against
     * a directory whose DACL was just written protected: OWNER-only reports
     * ctrl=0x8000 (DACL_PRESENT=0), OWNER|DACL reports ctrl=0x9404
     * (DACL_PRESENT=1, DACL_PROTECTED=1). Without this the whole in-app
     * download path fails at its first step with "file_io". */
    PSID want = nullptr;
    SID_IDENTIFIER_AUTHORITY ntAuth = SECURITY_NT_AUTHORITY;
    if (!AllocateAndInitializeSid(&ntAuth, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                  DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &want))
        return false;
    PSID actualOwner = nullptr;
    PSECURITY_DESCRIPTOR got = nullptr;
    std::wstring buf2 = path;
    bool ok = false;
    if (GetNamedSecurityInfoW(&buf2[0], SE_FILE_OBJECT,
                              OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                              &actualOwner, nullptr, nullptr, nullptr,
                              &got) == ERROR_SUCCESS &&
        actualOwner) {
        SECURITY_DESCRIPTOR_CONTROL ctrl = 0;
        DWORD rev = 0;
        ok = EqualSid(actualOwner, want) != 0 &&
             GetSecurityDescriptorControl(got, &ctrl, &rev) != 0 &&
             (ctrl & SE_DACL_PROTECTED) != 0;
    }
    if (got) LocalFree(got);
    FreeSid(want);
    return ok;
}

/* Creates %ProgramData%\Mivara\Updates and locks both levels down. Locking only
 * ...\Updates would leave the parent under the control of whoever created it
 * first, and a standard user can create folders under ProgramData. */
bool ensureUpdatesDir(std::wstring &dirOut, const char **err)
{
    wchar_t base[MAX_PATH] = {0};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr, 0, base)) || !base[0]) {
        *err = "file_io";
        return false;
    }
    const std::wstring parent = std::wstring(base) + L"\\Mivara";
    const std::wstring dir = parent + L"\\Updates";
    for (const std::wstring &p : {parent, dir}) {
        if (!CreateDirectoryW(p.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
            *err = "file_io";
            return false;
        }
        DWORD attr = GetFileAttributesW(p.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY) ||
            (attr & FILE_ATTRIBUTE_REPARSE_POINT)) {
            *err = "file_io";
            return false;
        }
        /* Fail closed: without a locked-down directory we do not stage a
         * binary we are going to execute. */
        if (!lockDownPath(p)) {
            *err = "file_io";
            return false;
        }
    }
    dirOut = dir;
    return true;
}

/* ── transports ──────────────────────────────────────────────────────────── */
bool checkLatest(std::string &tag_out, const char **err)
{
    HttpChain h;
    if (!httpOpen(h, "/wzy-777/Mivara/releases/latest", false, 2000, err)) return false;

    DWORD status = 0, len = sizeof(status);
    if (!WinHttpQueryHeaders(h.req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX)) {
        *err = "bad_response";
        return false;
    }
    if (status != 301 && status != 302 && status != 303) { *err = "bad_response"; return false; }

    wchar_t loc[1024] = {0};
    len = sizeof(loc);
    if (!WinHttpQueryHeaders(h.req, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                             loc, &len, WINHTTP_NO_HEADER_INDEX) || !loc[0]) {
        *err = "bad_response";
        return false;
    }
    std::string a = w2u8(loc);
    if (a.compare(0, strlen(kTagPrefix), kTagPrefix) != 0) { *err = "bad_response"; return false; }
    if (!updateParseReleaseLocation(a, tag_out)) { *err = "bad_version"; return false; }
    return true;
}

bool fetchAssetToFile(const std::string &assetPath, const std::wstring &fileW,
                      std::string &shaHex, long long &got, const char **err)
{
    const DWORD attr = GetFileAttributesW(fileW.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES) {
        if ((attr & FILE_ATTRIBUTE_REPARSE_POINT) || (attr & FILE_ATTRIBUTE_DIRECTORY) ||
            !DeleteFileW(fileW.c_str())) {
            *err = "file_io";
            return false;
        }
    }
    HANDLE f = CreateFileW(fileW.c_str(), GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { *err = "file_io"; return false; }
    if (!lockDownPath(fileW)) {
        CloseHandle(f);
        DeleteFileW(fileW.c_str());
        *err = "file_io";
        return false;
    }
    const bool ok = fetchAsset(assetPath, f, shaHex, got, err);
    CloseHandle(f);
    return ok;
}

std::string sumsPathFor(const std::string &tag)
{
    return std::string(kPathPrefix) + tag + "/" + kSumsName;
}
std::string assetPathFor(const std::string &tag)
{
    return std::string(kPathPrefix) + tag + "/" + kAssetName;
}

#else /* ── POSIX ────────────────────────────────────────────────────────────── */

/* One curl invocation, compile-time constant command (no user data, no shell
 * interpolation). curl honours http_proxy/https_proxy itself. */
bool checkLatest(std::string &tag_out, const char **err)
{
    std::string cmd = std::string("curl -sS --proto '=https' --connect-timeout 4 --max-time 8 ")
                    + "-o /dev/null -w '%{redirect_url}' -A 'Mivara-UpdateCheck/" MIVARA_VERSION "' "
                    + kLatestUrl + " 2>/dev/null";
    FILE *p = popen(cmd.c_str(), "r");
    if (!p) { *err = "no_curl"; return false; }
    char buf[1024] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, p);
    int rc = pclose(p);
    if (rc == 127 || rc == 32512) { *err = "no_curl"; return false; }
    if (rc != 0) { *err = "net_other"; return false; }
    std::string out(buf, n);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' ||
                            out.back() == ' ' || out.back() == '\t'))
        out.pop_back();
    if (out.empty()) { *err = "bad_response"; return false; }
    if (out.compare(0, strlen(kTagPrefix), kTagPrefix) != 0) { *err = "bad_response"; return false; }
    if (!updateParseReleaseLocation(out, tag_out)) { *err = "bad_version"; return false; }
    return true;
}

#ifdef MIVARA_LINUX_UPDATER
/* ── Linux in-app updater helpers ───────────────────────────────────────── */

/* Absolute path of an executable on PATH (empty when missing). */
std::string findTool(const char *name)
{
    if (!name || !*name) return std::string();
    if (name[0] == '/') return access(name, X_OK) == 0 ? std::string(name) : std::string();
    const char *path = getenv("PATH");
    if (!path || !*path) path = "/usr/local/bin:/usr/bin:/bin";
    const std::string p(path);
    size_t pos = 0;
    while (pos <= p.size()) {
        size_t e = p.find(':', pos);
        if (e == std::string::npos) e = p.size();
        std::string dir = p.substr(pos, e - pos);
        if (dir.empty()) dir = ".";
        const std::string cand = dir + "/" + name;
        if (access(cand.c_str(), X_OK) == 0) return cand;
        pos = e + 1;
    }
    return std::string();
}

/* Run argv[0] with stdout captured (bounded) and stderr discarded. */
bool runCapture(const char *const argv[], std::string &out, int &status)
{
    int fds[2];
    if (pipe(fds) != 0) return false;
    const pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return false; }
    if (pid == 0) {
        close(fds[0]);
        if (dup2(fds[1], STDOUT_FILENO) < 0) _exit(127);
        const int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { (void)dup2(devnull, STDERR_FILENO); close(devnull); }
        close(fds[1]);
        execv(argv[0], const_cast<char *const *>(argv));
        _exit(127);
    }
    close(fds[1]);
    out.clear();
    char buf[4096];
    for (;;) {
        const ssize_t n = read(fds[0], buf, sizeof(buf));
        if (n > 0) {
            if (out.size() < 64 * 1024) out.append(buf, (size_t)n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        break;
    }
    close(fds[0]);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return true;
}

bool sha256FilePosix(const std::string &path, std::string &hex_out)
{
    const std::string sum = findTool("sha256sum");
    if (sum.empty()) return false;
    const char *argv[] = { sum.c_str(), path.c_str(), nullptr };
    std::string out;
    int status = 0;
    if (!runCapture(argv, out, status)) return false;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return false;
    if (out.size() < 64) return false;
    const std::string h = out.substr(0, 64);
    if (!isHex64(h)) return false;
    hex_out = lower(h);
    return true;
}

std::string exePathReal()
{
    char buf[PATH_MAX];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return std::string();
    buf[n] = '\0';
    return std::string(buf);
}

/* The .deb installs app at /usr/lib/mivara/app; install.sh uses
 * <prefix>/lib/mivara/bin/app (even with --prefix /usr), so the exact path
 * distinguishes the two layouts. */
bool isDebLayout(const std::string &exe)
{
    return exe == "/usr/lib/mivara/app";
}

std::string linuxAssetName()
{
    if (isDebLayout(exePathReal()))
        return std::string("Mivara-linux-") + kDebArch + ".deb";
    return std::string("Mivara-linux-") + kLinuxArch + ".tar.gz";
}

/* kPathPrefix is host-relative for the Windows WinHTTP helpers; curl needs
 * the absolute URL. */
std::string linuxDownloadUrl(const std::string &tag, const std::string &name)
{
    return std::string("https://github.com") + kPathPrefix + tag + "/" + name;
}

struct LinuxInstallTarget {
    bool        deb = false;
    std::string prefix;     /* tar installs only */
    std::string relaunch;   /* absolute launcher path */
};

bool linuxInstallTargetFor(LinuxInstallTarget &t)
{
    const std::string exe = exePathReal();
    t.deb = isDebLayout(exe);
    if (t.deb) {
        t.relaunch = access("/usr/bin/mivara", X_OK) == 0 ? "/usr/bin/mivara" : exe;
        return !t.relaunch.empty();
    }
    static const char kSuffix[] = "/lib/mivara/bin/app";
    const size_t sl = sizeof(kSuffix) - 1;
    if (exe.size() > sl && exe.compare(exe.size() - sl, sl, kSuffix) == 0) {
        t.prefix = exe.substr(0, exe.size() - sl);
    } else {
        const char *home = getenv("HOME");
        const std::string h = (home && home[0] == '/') ? std::string(home) : std::string();
        if (!h.empty() &&
            access((h + "/.local/lib/mivara/bin/app").c_str(), X_OK) == 0)
            t.prefix = h + "/.local";
        else if (access("/usr/local/lib/mivara/bin/app", X_OK) == 0)
            t.prefix = "/usr/local";
        else
            t.prefix = h.empty() ? std::string("/usr/local") : h + "/.local";
    }
    if (t.prefix.empty() || t.prefix[0] != '/') return false;
    t.relaunch = t.prefix + "/bin/mivara";
    return true;
}

bool prefixWritable(const std::string &prefix)
{
    if (access(prefix.c_str(), W_OK) == 0) return true;
    const size_t slash = prefix.find_last_of('/');
    const std::string parent = (slash == std::string::npos || slash == 0)
                                   ? std::string("/") : prefix.substr(0, slash);
    return access(parent.c_str(), W_OK) == 0;
}

/* Both directory levels are owned by this user and closed to group/other.
 * A pre-existing level is chmod'ed rather than rejected so an upgrade from
 * an older release keeps working; symlinks and foreign owners fail closed. */
bool privateDirOk(const std::string &p, bool &missing)
{
    struct stat st;
    missing = false;
    if (lstat(p.c_str(), &st) != 0) {
        if (errno == ENOENT) missing = true;
        return false;
    }
    if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode) || st.st_uid != geteuid())
        return false;
    if ((st.st_mode & 077) != 0) {
        if (chmod(p.c_str(), 0700) != 0) return false;
        if (lstat(p.c_str(), &st) != 0) return false;
    }
    return S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode) && (st.st_mode & 077) == 0;
}

bool ensurePrivateDir(const std::string &p, const char **err)
{
    bool missing = false;
    if (privateDirOk(p, missing)) return true;
    if (!missing) { *err = "file_io"; return false; }
    if (mkdir(p.c_str(), 0700) != 0 && errno != EEXIST) {
        *err = "file_io";
        return false;
    }
    if (!privateDirOk(p, missing)) { *err = "file_io"; return false; }
    return true;
}

bool ensureLinuxUpdatesDir(std::string &dirOut, const char **err)
{
    const char *xdg = getenv("XDG_CACHE_HOME");
    std::string base;
    if (xdg && xdg[0] == '/') base = xdg;
    else {
        const char *home = getenv("HOME");
        if (!home || home[0] != '/') { *err = "file_io"; return false; }
        base = std::string(home) + "/.cache";
    }
    struct stat st;
    if (lstat(base.c_str(), &st) != 0) {
        if (errno != ENOENT || (mkdir(base.c_str(), 0700) != 0 && errno != EEXIST)) {
            *err = "file_io";
            return false;
        }
    } else if (!S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
        *err = "file_io";
        return false;
    }
    const std::string parent = base + "/mivara";
    const std::string dir = parent + "/updates";
    if (!ensurePrivateDir(parent, err) || !ensurePrivateDir(dir, err)) return false;
    dirOut = dir;
    return true;
}

/* Download one https URL to a file. Polls the child so 取消下载 and exit do
 * not have to wait for curl's own timeout. Returns 0 on success. */
int curlDownload(const std::string &url, const std::string &dest,
                 long long maxSize, const char **err)
{
    const std::string curl = findTool("curl");
    if (curl.empty()) { *err = "no_curl"; return -1; }
    unlink(dest.c_str());
    const std::string maxStr = std::to_string(maxSize);
    const std::string ua = "Mivara-Updater/" MIVARA_VERSION;
    std::vector<std::string> args = {
        curl,
        "-fsSL",
        "--proto", "=https",
        "--proto-redir", "=https",
        "--connect-timeout", "10",
        "--max-time", "600",
        "--max-filesize", maxStr,
        "-A", ua,
        "-o", dest,
        "-w", "%{http_code}",
        url,
    };
    std::vector<char *> argv;
    argv.reserve(args.size() + 1);
    for (std::string &a : args) argv.push_back(&a[0]);
    argv.push_back(nullptr);

    int fds[2];
    if (pipe(fds) != 0) { *err = "file_io"; return -1; }
    const pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); *err = "file_io"; return -1; }
    if (pid == 0) {
        close(fds[0]);
        if (dup2(fds[1], STDOUT_FILENO) < 0) _exit(127);
        const int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { (void)dup2(devnull, STDERR_FILENO); close(devnull); }
        close(fds[1]);
        execv(curl.c_str(), argv.data());
        _exit(127);
    }
    close(fds[1]);
    int status = 0;
    bool aborted = false;
    for (;;) {
        const pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) break;
        if (r < 0 && errno != EINTR) {
            (void)kill(pid, SIGKILL);
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
            close(fds[0]);
            unlink(dest.c_str());
            *err = "internal";
            return -1;
        }
        if (s_abort) {
            aborted = true;
            (void)kill(pid, SIGTERM);
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
            break;
        }
        const struct timespec ts = {0, 100 * 1000 * 1000};
        nanosleep(&ts, nullptr);
    }
    std::string http;
    char buf[64];
    for (;;) {
        const ssize_t n = read(fds[0], buf, sizeof(buf));
        if (n > 0) { http.append(buf, (size_t)n); continue; }
        if (n < 0 && errno == EINTR) continue;
        break;
    }
    close(fds[0]);
    if (aborted) { unlink(dest.c_str()); *err = "aborted"; return -1; }
    if (!WIFEXITED(status)) { unlink(dest.c_str()); *err = "net_other"; return -1; }
    const int rc = WEXITSTATUS(status);
    if (rc != 0) {
        unlink(dest.c_str());
        switch (rc) {
        case 22: *err = "not_found"; break;      /* -f: HTTP >= 400 */
        case 23: *err = "file_io"; break;
        case 28: *err = "net_timeout"; break;
        case 35: case 60: *err = "net_tls"; break;
        case 6:  *err = "net_dns"; break;
        case 7:  *err = "net_connect"; break;
        case 63: *err = "too_big"; break;
        case 127: *err = "no_curl"; break;
        default: *err = "net_other"; break;
        }
        return -1;
    }
    while (!http.empty() && (http.back() == '\n' || http.back() == '\r' ||
                             http.back() == ' ' || http.back() == '\t'))
        http.pop_back();
    if (http != "200") {
        unlink(dest.c_str());
        *err = "bad_response";
        return -1;
    }
    return 0;
}

bool fetchLinuxSmall(const std::string &dir, const std::string &url,
                     std::string &body, const char **err)
{
    const std::string tmp = dir + "/.sums-" + std::to_string((long)getpid());
    if (curlDownload(url, tmp, kMaxSums, err) != 0) {
        unlink(tmp.c_str());
        return false;
    }
    struct stat st;
    if (lstat(tmp.c_str(), &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_size <= 0 || st.st_size > kMaxSums) {
        unlink(tmp.c_str());
        *err = "bad_response";
        return false;
    }
    FILE *f = std::fopen(tmp.c_str(), "rb");
    if (!f) { unlink(tmp.c_str()); *err = "file_io"; return false; }
    body.clear();
    char buf[4096];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) body.append(buf, n);
    const bool readErr = std::ferror(f) != 0;
    std::fclose(f);
    unlink(tmp.c_str());
    if (readErr) { *err = "file_io"; return false; }
    return true;
}

bool fetchLinuxAssetToFile(const std::string &url, const std::string &file,
                           std::string &shaHex, long long &got, const char **err)
{
    if (curlDownload(url, file, kMaxAsset, err) != 0) return false;
    struct stat st;
    if (lstat(file.c_str(), &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_size <= 0 || st.st_size > kMaxAsset) {
        unlink(file.c_str());
        *err = "bad_response";
        return false;
    }
    got = (long long)st.st_size;
    if (!sha256FilePosix(file, shaHex)) {
        unlink(file.c_str());
        *err = "file_io";
        return false;
    }
    return true;
}

std::string shellQuote(const std::string &s)
{
    std::string out;
    out.reserve(s.size() + 2);
    out += '\'';
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += '\'';
    return out;
}

/* Generates the detached installer. Kept as a pure string builder so the
 * diagnostic CLI can print it and `sh -n` it without installing anything. */
std::string buildLinuxHelperScript(const std::string &pkg, bool deb,
                                   const std::string &prefix,
                                   const std::string &relaunch,
                                   const std::string &log,
                                   const std::string &sha,
                                   const std::string &sumtool,
                                   const std::string &pkexec,
                                   const std::string &aptget,
                                   const std::string &tar)
{
    std::string script;
    script.reserve(4096);
    script += "#!/bin/sh\n";
    script += "# Mivara update helper (generated; safe to delete).\n";
    script += "set -u\n";
    script += "status=1\n";
    script += "pid=\"$1\"\n";
    script += "i=0\n";
    script += "while kill -0 \"$pid\" 2>/dev/null; do\n";
    script += "  i=$((i + 1)); [ \"$i\" -ge 600 ] && break; sleep 0.1\n";
    script += "done\n";
    script += "sleep 0.5\n";
    script += "pkg=" + shellQuote(pkg) + "\n";
    script += "log=" + shellQuote(log) + "\n";
    script += "relaunch=" + shellQuote(relaunch) + "\n";
    script += ": >\"$log\" 2>/dev/null || true\n";
    /* Re-verify right before the privileged step: the app already checked the
     * package, but the helper starts after it exits, so a last check narrows
     * the window in which the file could be swapped. */
    script += "expect=" + shellQuote(sha) + "\n";
    script += "sumline=\"$(" + shellQuote(sumtool) + " \"$pkg\" 2>/dev/null)\"\n";
    script += "got=\"${sumline%% *}\"\n";
    script += "if [ \"$got\" != \"$expect\" ]; then\n";
    script += "  echo 'checksum mismatch; refusing to install' >>\"$log\"\n";
    script += "  exit 1\n";
    script += "fi\n";
    if (deb) {
        script += "aptget=" + shellQuote(aptget) + "\n";
        script += "pkexec=" + shellQuote(pkexec) + "\n";
        script += "if [ \"$(id -u)\" -eq 0 ]; then\n";
        script += "  DEBIAN_FRONTEND=noninteractive \"$aptget\" install -y \"$pkg\" >>\"$log\" 2>&1\n";
        script += "  status=$?\n";
        script += "else\n";
        script += "  \"$pkexec\" /usr/bin/env DEBIAN_FRONTEND=noninteractive \"$aptget\" install -y \"$pkg\" >>\"$log\" 2>&1\n";
        script += "  status=$?\n";
        script += "fi\n";
    } else {
        script += "tar=" + shellQuote(tar) + "\n";
        script += "prefix=" + shellQuote(prefix) + "\n";
        script += "pkexec=" + shellQuote(pkexec) + "\n";
        script += "tmp=\"$(mktemp -d 2>/dev/null)\" || tmp=\"\"\n";
        script += "if [ -n \"$tmp\" ]; then\n";
        script += "  if \"$tar\" -xzf \"$pkg\" -C \"$tmp\" >>\"$log\" 2>&1; then\n";
        script += "    set -- \"$tmp\"/*/install.sh\n";
        script += "    if [ -f \"$1\" ]; then\n";
        script += "      dir=\"${1%/install.sh}\"\n";
        script += "      if [ \"$(id -u)\" -eq 0 ]; then\n";
        script += "        /bin/sh \"$dir/install.sh\" --prefix \"$prefix\" >>\"$log\" 2>&1\n";
        script += "        status=$?\n";
        script += "      elif [ -w \"$prefix\" ] || [ -w \"$(dirname \"$prefix\")\" ]; then\n";
        script += "        /bin/sh \"$dir/install.sh\" --prefix \"$prefix\" >>\"$log\" 2>&1\n";
        script += "        status=$?\n";
        script += "      elif [ -n \"$pkexec\" ]; then\n";
        script += "        \"$pkexec\" /bin/sh \"$dir/install.sh\" --prefix \"$prefix\" >>\"$log\" 2>&1\n";
        script += "        status=$?\n";
        script += "      else\n";
        script += "        echo 'pkexec not found' >>\"$log\"\n";
        script += "      fi\n";
        script += "    else\n";
        script += "      echo 'install.sh not found in archive' >>\"$log\"\n";
        script += "    fi\n";
        script += "  fi\n";
        script += "  rm -rf \"$tmp\"\n";
        script += "fi\n";
    }
    script += "if [ \"$status\" -eq 0 ] && [ -x \"$relaunch\" ]; then\n";
    script += "  nohup \"$relaunch\" </dev/null >/dev/null 2>&1 &\n";
    script += "fi\n";
    script += "exit \"$status\"\n";
    return script;
}

/* The app cannot replace its own executable (ETXTBSY) and apt must run as
 * root, so hand the verified package to a detached helper: it waits for this
 * process to exit, installs, and relaunches Mivara on success. */
bool launchLinuxInstaller()
{
    UpdateInfo ui = updateGetInfo();
    if (ui.state != UpdateState::Downloaded || ui.install_path.empty() ||
        ui.sha256.empty())
        return false;

    std::string dir;
    const char *err = nullptr;
    if (!ensureLinuxUpdatesDir(dir, &err)) { setDetail("file_io"); return false; }

    /* The path must be a direct child of our private directory. */
    const std::string base = dir + "/";
    if (ui.install_path.compare(0, base.size(), base) != 0 ||
        ui.install_path.find('/', base.size()) != std::string::npos) {
        setDetail("file_io");
        return false;
    }
    struct stat st;
    if (lstat(ui.install_path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) ||
        S_ISLNK(st.st_mode) || st.st_uid != geteuid()) {
        setDetail("file_io");
        return false;
    }
    std::string hex;
    if (!sha256FilePosix(ui.install_path, hex) || lower(hex) != lower(ui.sha256)) {
        unlink(ui.install_path.c_str());
        setDetail("hash_mismatch");
        return false;
    }
    const std::string sumtool = findTool("sha256sum");
    if (sumtool.empty()) { setDetail("file_io"); return false; }

    LinuxInstallTarget t;
    if (!linuxInstallTargetFor(t)) { setDetail("file_io"); return false; }

    const bool asRoot = (geteuid() == 0);
    const std::string pkexec = findTool("pkexec");
    const std::string aptget = t.deb ? findTool("apt-get") : std::string();
    const std::string tar = t.deb ? std::string() : findTool("tar");
    if (t.deb && aptget.empty()) { setDetail("no_apt"); return false; }
    if (!t.deb && tar.empty()) { setDetail("no_tar"); return false; }
    const bool needRoot = t.deb || !prefixWritable(t.prefix);
    if (!asRoot && needRoot && pkexec.empty()) { setDetail("no_pkexec"); return false; }

    const std::string helper = dir + "/install-update.sh";
    const std::string log = dir + "/install.log";
    unlink(helper.c_str());
    const int fd = open(helper.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0700);
    if (fd < 0) { setDetail("file_io"); return false; }

    const std::string script = buildLinuxHelperScript(
        ui.install_path, t.deb, t.prefix, t.relaunch, log, lower(ui.sha256),
        sumtool, pkexec, aptget, tar);

    const ssize_t wn = write(fd, script.data(), script.size());
    const int closed = close(fd);
    if (wn != (ssize_t)script.size() || closed != 0) {
        unlink(helper.c_str());
        setDetail("file_io");
        return false;
    }

    /* A CLOEXEC pipe reports EOF once exec succeeds and a byte when it does
     * not, so a missing /bin/sh is reported instead of silently ignored. */
    int fds[2];
    if (pipe(fds) != 0) { unlink(helper.c_str()); setDetail("launch_failed"); return false; }
    (void)fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    char pidbuf[32];
    std::snprintf(pidbuf, sizeof(pidbuf), "%ld", (long)getpid());
    const pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        unlink(helper.c_str());
        setDetail("launch_failed");
        return false;
    }
    if (pid == 0) {
        close(fds[0]);
        (void)setsid();
        const char *argv[] = { "/bin/sh", helper.c_str(), pidbuf, nullptr };
        execv("/bin/sh", const_cast<char *const *>(argv));
        const char c = 1;
        const ssize_t ignored = write(fds[1], &c, 1);
        (void)ignored;
        _exit(127);
    }
    close(fds[1]);
    char c = 0;
    ssize_t rn = 0;
    do { rn = read(fds[0], &c, 1); } while (rn < 0 && errno == EINTR);
    close(fds[0]);
    if (rn == 1) {
        unlink(helper.c_str());
        setDetail("launch_failed");
        return false;
    }
    setDetail("");
    return true;
}

/* Remove packages left by an earlier run. A file younger than a day may be
 * the one the current UI is about to install, so it stays. */
void sweepLinuxUpdates()
{
    std::string dir;
    const char *err = nullptr;
    if (!ensureLinuxUpdatesDir(dir, &err)) return;
    DIR *d = opendir(dir.c_str());
    if (!d) return;
    const time_t now = std::time(nullptr);
    struct dirent *e;
    while ((e = readdir(d)) != nullptr) {
        const std::string name = e->d_name;
        const bool ours = (name.rfind("Mivara-linux-", 0) == 0) ||
                          (name.rfind("mivara_", 0) == 0) ||
                          (name.rfind(".sums-", 0) == 0) ||
                          name == "install-update.sh";
        if (!ours) continue;
        const std::string p = dir + "/" + name;
        struct stat st;
        if (lstat(p.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
        if (now - st.st_mtime < 86400) continue;
        unlink(p.c_str());
    }
    closedir(d);
}
#endif /* MIVARA_LINUX_UPDATER */

#endif /* _WIN32 */

/* ── worker bodies ───────────────────────────────────────────────────────── */
/* One retry, only for errors that are demonstrably transient: measured on this
 * machine, github.com direct reachability fails roughly one request in two
 * during bad windows while succeeding milliseconds later. A bad response or a
 * bad version must NOT be retried - that is a decision, not a hiccup. */
bool transientNetError(const char *err)
{
    if (!err) return false;
    return !strcmp(err, "net_timeout") || !strcmp(err, "net_connect") ||
           !strcmp(err, "net_dns");
}

void workerCheck()
{
    std::string tag;
    const char *err = nullptr;
    bool ok = false;
    for (int attempt = 0; attempt < 2; attempt++) {
        if (s_abort) { finishState(UpdateState::Failed, "aborted", false, ""); return; }
        err = nullptr;
        ok = checkLatest(tag, &err);
        if (ok || !transientNetError(err)) break;
        if (attempt == 0) {
#ifdef _WIN32
            Sleep(500);
#else
            usleep(500000);
#endif
        }
    }
    if (!ok) {
        finishState(UpdateState::Failed, err ? err : "internal", false, "");
        return;
    }
    if (updateVersionGreater(tag, MIVARA_VERSION))
        finishState(UpdateState::Available, "", false, tag);
    else
        finishState(UpdateState::UpToDate, "", false, "");
}

#ifdef _WIN32
void workerDownload(std::string tag)
{
    const char *err = nullptr;

    if (!validTagChars(tag)) {
        finishState(UpdateState::Failed, "bad_version", true, tag);
        return;
    }
    std::wstring dir;
    if (!ensureUpdatesDir(dir, &err)) {
        finishState(UpdateState::Failed, err ? err : "file_io", true, tag);
        return;
    }

    /* 1. hash anchor first — without it we never execute anything */
    std::string sums, expect;
    if (!fetchSmall(sumsPathFor(tag), sums, &err)) {
        /* a cancel that lands here is still a clean cancel, not a failure */
        if (s_abort) { finishState(UpdateState::Available, "cancelled", true, tag); return; }
        /* a release without the checksum file cannot be installed in-app */
        const bool missing = (!err || !strcmp(err, "not_found"));
        finishState(UpdateState::Failed, missing ? "no_sums" : err, true, tag);
        return;
    }
    if (!updateParseSha256Sums(sums, kAssetName, expect)) {
        finishState(UpdateState::Failed, "no_sums", true, tag);
        return;
    }
    if (s_abort) { finishState(UpdateState::Available, "cancelled", true, tag); return; }

    /* 2. download + streaming hash (one retry on a transient network error;
     * no resume, so a retry restarts the transfer - still better than telling
     * the user to start over on a link that is merely flaky) */
    std::wstring fileW = dir + L"\\Mivara-Setup-" + u82w(stripV(tag)) + L".exe";
    std::string sha;
    long long got = 0;
    bool dlOk = false;
    for (int attempt = 0; attempt < 2; attempt++) {
        if (s_abort) { finishState(UpdateState::Available, "cancelled", true, tag); return; }
        err = nullptr;
        sha.clear();
        got = 0;
        dlOk = fetchAssetToFile(assetPathFor(tag), fileW, sha, got, &err);
        if (dlOk || !transientNetError(err)) break;
        DeleteFileW(fileW.c_str());
        if (attempt == 0) {
#ifdef _WIN32
            Sleep(500);
#else
            usleep(500000);
#endif
        }
    }
    if (!dlOk) {
        DeleteFileW(fileW.c_str());
        if (s_abort) finishState(UpdateState::Available, "cancelled", true, tag);
        else         finishState(UpdateState::Failed, err ? err : "internal", true, tag);
        return;
    }

    /* 3. content verification */
    if (lower(sha) != lower(expect)) {
        DeleteFileW(fileW.c_str());
        finishState(UpdateState::Failed, "hash_mismatch", true, tag);
        return;
    }
    finishState(UpdateState::Downloaded, "", true, tag, w2u8(fileW), lower(expect));
}
#elif defined(MIVARA_LINUX_UPDATER)
void workerDownload(std::string tag)
{
    const char *err = nullptr;

    if (!validTagChars(tag)) {
        finishState(UpdateState::Failed, "bad_version", true, tag);
        return;
    }
    std::string dir;
    if (!ensureLinuxUpdatesDir(dir, &err)) {
        finishState(UpdateState::Failed, err ? err : "file_io", true, tag);
        return;
    }

    const std::string assetName = linuxAssetName();
    const std::string sumsUrl = linuxDownloadUrl(tag, kLinuxSumsName);
    const std::string assetUrl = linuxDownloadUrl(tag, assetName);

    /* 1. hash anchor first — without it we never execute anything. The
     * checksum file also tells us whether this architecture's package exists,
     * so an unsupported arch fails before a 25 MB download. */
    std::string sums, expect;
    bool sumsOk = false;
    for (int attempt = 0; attempt < 2; attempt++) {
        if (s_abort) { finishState(UpdateState::Available, "cancelled", true, tag); return; }
        err = nullptr;
        sumsOk = fetchLinuxSmall(dir, sumsUrl, sums, &err);
        if (sumsOk || !transientNetError(err)) break;
        if (attempt == 0) usleep(500000);
    }
    if (!sumsOk) {
        /* a cancel that lands here is still a clean cancel, not a failure */
        if (s_abort) { finishState(UpdateState::Available, "cancelled", true, tag); return; }
        const bool missing = (!err || !strcmp(err, "not_found"));
        finishState(UpdateState::Failed, missing ? "no_sums" : err, true, tag);
        return;
    }
    if (!updateParseSha256Sums(sums, assetName, expect)) {
        finishState(UpdateState::Failed, "no_arch_asset", true, tag);
        return;
    }
    if (s_abort) { finishState(UpdateState::Available, "cancelled", true, tag); return; }

    /* 2. download + hash (one retry on a transient network error) */
    const std::string file = dir + "/" + assetName;
    std::string sha;
    long long got = 0;
    bool dlOk = false;
    for (int attempt = 0; attempt < 2; attempt++) {
        if (s_abort) { finishState(UpdateState::Available, "cancelled", true, tag); return; }
        err = nullptr;
        sha.clear();
        got = 0;
        dlOk = fetchLinuxAssetToFile(assetUrl, file, sha, got, &err);
        if (dlOk || !transientNetError(err)) break;
        unlink(file.c_str());
        if (attempt == 0) usleep(500000);
    }
    if (!dlOk) {
        unlink(file.c_str());
        if (s_abort) finishState(UpdateState::Available, "cancelled", true, tag);
        else         finishState(UpdateState::Failed, err ? err : "internal", true, tag);
        return;
    }

    /* 3. content verification */
    if (lower(sha) != lower(expect)) {
        unlink(file.c_str());
        finishState(UpdateState::Failed, "hash_mismatch", true, tag);
        return;
    }
    if (s_abort) { finishState(UpdateState::Available, "cancelled", true, tag); return; }
    finishState(UpdateState::Downloaded, "", true, tag, file, lower(expect));
}
#endif

void threadMain(bool download, std::string tag)
{
#if defined(_WIN32) || defined(MIVARA_LINUX_UPDATER)
    if (download) { workerDownload(tag); return; }
#endif
    (void)download;
    (void)tag;
    workerCheck();
}

bool startWorker(bool download, const std::string &tag)
{
    bool expected = false;
    if (!s_running.compare_exchange_strong(expected, true)) return false;
    if (s_worker.th.joinable()) s_worker.th.join();
    s_abort = false;
    s_done = false;
    s_percent = -1;
    s_launched = false;
    {
        std::lock_guard<std::mutex> lk(s_mtx);
        s_info = UpdateInfo{};
        s_info.state = download ? UpdateState::Downloading : UpdateState::Checking;
        s_info.is_download = download;
        s_info.remote_tag = tag;
        s_info.version = stripV(tag);
    }
    try {
        s_worker.th = std::thread(threadMain, download, tag);
    } catch (const std::system_error &) {
        {
            std::lock_guard<std::mutex> lk(s_mtx);
            s_info = UpdateInfo{};
            s_info.state = UpdateState::Failed;
            s_info.is_download = download;
            s_info.detail = "internal";
        }
        s_running = false;
        s_done = true;   /* still deliver the failure through updateTake() */
        return false;
    }
    return true;
}

} // namespace

/* ── public API ──────────────────────────────────────────────────────────── */
bool updateDownloadSupported()
{
#if defined(_WIN32) || defined(MIVARA_LINUX_UPDATER)
    return true;
#else
    return false;
#endif
}

std::string updateErrorText(const std::string &detail)
{
    if (detail == "winhttp_init")  return "初始化失败";
    if (detail == "net_dns")       return "DNS 解析失败";
    if (detail == "net_connect")   return "无法连接 github.com";
    if (detail == "net_timeout")   return "请求超时";
    if (detail == "net_tls")       return "安全连接失败";
    if (detail == "net_other")     return "网络错误";
    if (detail == "bad_response")  return "服务器响应异常";
    if (detail == "bad_version")   return "版本号格式异常";
    if (detail == "not_found")     return "下载地址不存在（请用浏览器下载）";
    if (detail == "no_sums")       return "未找到校验文件（请用浏览器下载）";
    if (detail == "no_arch_asset") return "当前架构暂无安装包（请用浏览器下载）";
    if (detail == "size_mismatch") return "下载不完整";
    if (detail == "hash_mismatch") return "校验失败，安装包已删除";
    if (detail == "too_big")       return "响应数据过大";
    if (detail == "file_io")       return "文件读写失败";
    if (detail == "launch_failed") return "启动安装程序失败，请重试";
    if (detail == "no_curl")       return "未找到 curl";
    if (detail == "no_pkexec")     return "未找到 pkexec，无法提权安装";
    if (detail == "no_apt")        return "未找到 apt-get，无法安装";
    if (detail == "no_tar")        return "未找到 tar，无法解压安装包";
    if (detail == "aborted")       return "已取消";
    return "内部错误";
}

bool updateCheckStart()
{
    return startWorker(false, std::string());
}

bool updateDownloadStart()
{
#if defined(_WIN32) || defined(MIVARA_LINUX_UPDATER)
    UpdateInfo ui = updateGetInfo();
    /* Available: the normal path. Failed after a download attempt: 「重试」
     * restarts the download for the same tag instead of forcing a re-check. */
    const bool retry = (ui.state == UpdateState::Failed && ui.is_download);
    if (ui.state != UpdateState::Available && !retry) return false;
    if (!validTagChars(ui.remote_tag)) return false;
    return startWorker(true, ui.remote_tag);
#else
    return false;
#endif
}

void updateDownloadCancel()
{
    s_abort = true;
    cancelActiveRequest();
}

bool updateTake(UpdateInfo &out)
{
    if (!s_done.exchange(false)) return false;
    std::lock_guard<std::mutex> lk(s_mtx);
    out = s_info;
    return true;
}

UpdateInfo updateGetInfo()
{
    std::lock_guard<std::mutex> lk(s_mtx);
    return s_info;
}

int updateProgress()
{
    return s_percent.load();
}

void updateJoin()
{
    s_abort = true;
    cancelActiveRequest();       /* unblock an in-flight receive right away */
    if (s_worker.th.joinable()) s_worker.th.join();
}

void updateStartupSweep()
{
#ifdef _WIN32
    std::string dir = updatesDirUtf8();
    if (dir.empty()) return;
    std::wstring wdir = u82w(dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((wdir + L"\\Mivara-Setup-*.exe").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER nowU, wU;
    nowU.LowPart = now.dwLowDateTime;
    nowU.HighPart = now.dwHighDateTime;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        wU.LowPart = fd.ftLastWriteTime.dwLowDateTime;
        wU.HighPart = fd.ftLastWriteTime.dwHighDateTime;
        /* a fresh file may belong to an installer the user is running now */
        if (nowU.QuadPart >= wU.QuadPart &&
            (nowU.QuadPart - wU.QuadPart) / 10000000ULL < 600)
            continue;
        DeleteFileW((wdir + L"\\" + fd.cFileName).c_str());
    } while (FindNextFileW(h, &fd));
    FindClose(h);
#elif defined(MIVARA_LINUX_UPDATER)
    sweepLinuxUpdates();
#endif
}

bool updateOpenReleasePage()
{
#ifdef _WIN32
    /* This process runs elevated (VDD manifest). Launching the browser
     * directly would hand it the admin token, so hand off through
     * explorer.exe, which runs at medium integrity and forwards to the
     * default browser. The executable is named by its FULL path: a bare
     * "explorer.exe" would be resolved from the current directory first and a
     * planted copy there would run with this process's admin token. The URL is
     * a compile-time constant. */
    wchar_t windir[MAX_PATH] = {0};
    if (!GetWindowsDirectoryW(windir, MAX_PATH) || !windir[0]) return false;
    std::wstring exe = std::wstring(windir) + L"\\explorer.exe";
    std::string args = std::string("\"") + kLatestUrl + "\"";
    std::wstring wargs = u82w(args);
    HINSTANCE r = ShellExecuteW(nullptr, L"open", exe.c_str(), wargs.c_str(), nullptr,
                                SW_SHOWNORMAL);
    return (INT_PTR)r > 32;
#else
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        execlp("xdg-open", "xdg-open", kLatestUrl, (char *)nullptr);
        execlp("open", "open", kLatestUrl, (char *)nullptr);
        _exit(127);
    }
    int st = 0;
    if (waitpid(pid, &st, 0) < 0) return false;
    return WIFEXITED(st) && WEXITSTATUS(st) == 0;
#endif
}

#ifdef _WIN32
static bool hashFileHandle(HANDLE f, std::string &hex_out)
{
    Sha256 h;
    if (!h.init()) return false;
    LARGE_INTEGER zero = {};
    if (!SetFilePointerEx(f, zero, nullptr, FILE_BEGIN)) return false;
    std::vector<unsigned char> buf(64 * 1024);
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(f, buf.data(), (DWORD)buf.size(), &read, nullptr)) return false;
        if (read == 0) break;
        h.update(buf.data(), read);
    }
    return h.finish(hex_out);
}
#endif

bool updateLaunchInstaller()
{
#ifdef _WIN32
    if (s_launched) return false;
    UpdateInfo ui = updateGetInfo();
    if (ui.state != UpdateState::Downloaded || ui.install_path.empty() || ui.sha256.empty())
        return false;
    const std::wstring wpath = u82w(ui.install_path);
    HANDLE f = INVALID_HANDLE_VALUE;
    const auto reject = [&](const char *reason) {
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
        DeleteFileW(wpath.c_str());
        finishState(UpdateState::Failed, reason, true, ui.remote_tag);
        return false;
    };
    const DWORD attr = GetFileAttributesW(wpath.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY) ||
        (attr & FILE_ATTRIBUTE_REPARSE_POINT) || !lockDownPath(wpath))
        return reject("file_io");
    f = CreateFileW(wpath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (f == INVALID_HANDLE_VALUE) return reject("file_io");
    BY_HANDLE_FILE_INFORMATION info = {};
    if (!GetFileInformationByHandle(f, &info) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return reject("file_io");
    std::string hex;
    if (!hashFileHandle(f, hex) || lower(hex) != lower(ui.sha256))
        return reject("hash_mismatch");
    /* The handle denies write/delete sharing until ShellExecute returns, and
       the protected owner/DACL keeps it locked after the hand-over. */
    HINSTANCE r = ShellExecuteW(nullptr, L"open", wpath.c_str(), nullptr, nullptr,
                                SW_SHOWNORMAL);
    CloseHandle(f);
    if ((INT_PTR)r <= 32) {
        setDetail("launch_failed");
        return false;
    }
    s_launched = true;
    setDetail("");
    return true;
#elif defined(MIVARA_LINUX_UPDATER)
    return launchLinuxInstaller();
#else
    return false;
#endif
}

/* ── pure helpers ────────────────────────────────────────────────────────── */
bool updateParseReleaseLocation(const std::string &location, std::string &tag_out)
{
    const std::string prefix = kTagPrefix;
    if (location.size() <= prefix.size() + 2) return false;
    if (location.compare(0, prefix.size(), prefix) != 0) return false;
    std::string rest = location.substr(prefix.size());
    if (rest[0] != 'v') return false;
    std::string ver = rest.substr(1);
    int parts[4], n = 0;
    if (!parseVersion(ver, parts, n)) return false;
    if (!validTagChars(rest)) return false;
    tag_out = rest;
    return true;
}

bool updateVersionGreater(const std::string &remote_tag, const std::string &current)
{
    return cmpVersion(stripV(remote_tag), current) > 0;
}

bool updateParseSha256Sums(const std::string &text, const std::string &filename,
                           std::string &hex_out)
{
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        std::string line = text.substr(pos, eol - pos);
        pos = eol + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' ||
                                 line.back() == '\t'))
            line.pop_back();
        if (line.empty()) continue;
        size_t sp = line.find_first_of(" \t");
        if (sp == std::string::npos) continue;
        std::string hash = line.substr(0, sp);
        size_t s2 = line.find_first_not_of(" \t", sp);
        if (s2 == std::string::npos) continue;
        std::string name = line.substr(s2);
        if (name.size() > 1 && name[0] == '*') name = name.substr(1);
        if (isHex64(hash) && name == filename) {
            hex_out = lower(hash);
            return true;
        }
    }
    return false;
}

bool updateFileSha256(const std::string &path, std::string &hex_out)
{
#ifdef _WIN32
    HANDLE f = CreateFileW(u82w(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    const bool ok = hashFileHandle(f, hex_out);
    CloseHandle(f);
    return ok;
#elif defined(MIVARA_LINUX_UPDATER)
    return sha256FilePosix(path, hex_out);
#else
    /* Other POSIX builds never download an installer, so there is nothing to
     * hash; the launcher path is Windows/Linux-only. */
    (void)path;
    (void)hex_out;
    return false;
#endif
}

/* ── local diagnostic CLI (never built by the makefile) ─────────────────── */
#ifdef UPDATE_TEST_MAIN
static int s_fail = 0;
static void expect(bool ok, const char *what)
{
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) s_fail++;
}
int main(int argc, char **argv)
{
    std::string mode = (argc > 1) ? argv[1] : "unit";
    if (mode == "unit") {
        std::string tag;
        std::string pfx = kTagPrefix;
        expect(updateParseReleaseLocation(pfx + "v0.9.1", tag) && tag == "v0.9.1", "location ok");
        expect(updateParseReleaseLocation(pfx + "v0.9.10", tag) && tag == "v0.9.10", "location two digits");
        expect(!updateParseReleaseLocation("", tag), "location empty");
        expect(!updateParseReleaseLocation("http://github.com/wzy-777/Mivara/releases/tag/v0.9.1", tag), "location http");
        expect(!updateParseReleaseLocation("https://github.com.evil.com/wzy-777/Mivara/releases/tag/v0.9.1", tag), "location lookalike host");
        expect(!updateParseReleaseLocation(pfx + "v0.9.1/", tag), "location trailing slash");
        expect(!updateParseReleaseLocation(pfx + "v0.9.1?x=1", tag), "location query");
        expect(!updateParseReleaseLocation(pfx + "v01.2.3", tag), "location leading zero");
        expect(!updateParseReleaseLocation(pfx + "v1.2.3.4.5", tag), "location five parts");
        expect(!updateParseReleaseLocation(pfx + "v1.2.x", tag), "location junk");
        expect(!updateParseReleaseLocation(pfx + "v", tag), "location bare v");
        expect(updateParseReleaseLocation(pfx + "v1.2", tag), "location two parts tolerated");
        expect(updateParseReleaseLocation(pfx + "v0.9.1-rc1", tag) && tag == "v0.9.1-rc1", "location pre-release");
        expect(updateVersionGreater("v0.9.10", "0.9.9"), "0.9.10 > 0.9.9");
        expect(!updateVersionGreater("v0.9.1", "0.9.1"), "equal is not greater");
        expect(!updateVersionGreater("v0.9.1-rc1", "0.9.1"), "pre-release of same version is not greater");
        expect(updateVersionGreater("v1.0", "0.9.9"), "1.0 > 0.9.9");
        expect(updateVersionGreater("v0.10.0", "0.9.1"), "0.10.0 > 0.9.1");
        expect(!updateVersionGreater("v0.8.9", "0.9.1"), "older is not greater");
        std::string hex;
        std::string h64(64, 'a');
        expect(updateParseSha256Sums(h64 + "  Mivara-Setup.exe\n", "Mivara-Setup.exe", hex) && hex == h64, "sums plain");
        expect(updateParseSha256Sums("dead\r\n" + h64 + " *Mivara-Setup.exe\r\n", "Mivara-Setup.exe", hex), "sums binary marker + crlf");
        expect(!updateParseSha256Sums(h64 + "  Mivara-Setup-0.9.1.exe\n", "Mivara-Setup.exe", hex), "sums other file");
        expect(!updateParseSha256Sums("abc  Mivara-Setup.exe\n", "Mivara-Setup.exe", hex), "sums short hash");
        std::printf(s_fail ? "FAILURES: %d\n" : "ALL PASS\n", s_fail);
        return s_fail;
    }
    if (mode == "check") {
        updateCheckStart();
        for (int i = 0; i < 200; ++i) {
            UpdateInfo ui;
            if (updateTake(ui)) {
                std::printf("state=%d tag=%s version=%s detail=%s\n", (int)ui.state,
                            ui.remote_tag.c_str(), ui.version.c_str(), ui.detail.c_str());
                return ui.state == UpdateState::Failed ? 1 : 0;
            }
#ifdef _WIN32
            Sleep(100);
#else
            usleep(100000);
#endif
        }
        std::printf("timeout\n");
        return 2;
    }
    if (mode == "download" && argc > 2) {
        std::string tag = argv[2];
#ifdef _WIN32
        std::wstring dir;
        const char *err = nullptr;
        /* test-only escape hatch: the production directory is admin-only by
         * design, so an unelevated test run cannot write into it */
        const char *td = getenv("MIVARA_UPD_TEST_DIR");
        if (td && *td) dir = u82w(td);
        else if (!ensureUpdatesDir(dir, &err)) { std::printf("dir failed: %s\n", err); return 1; }
        std::wstring f = dir + L"\\probe-" + u82w(stripV(tag)) + L".exe";
        std::string sha;
        long long got = 0;
        bool ok = fetchAssetToFile(assetPathFor(tag), f, sha, got, &err);
        std::printf("download ok=%d bytes=%lld sha256=%s err=%s\n", (int)ok, got,
                    sha.c_str(), err ? err : "-");
        return ok ? 0 : 1;
#elif defined(MIVARA_LINUX_UPDATER)
        /* `download <tag> [asset]` lets a test fetch any published asset (the
         * running arch's package by default) without touching the GUI state. */
        const char *td = getenv("MIVARA_UPD_TEST_DIR");
        std::string dir;
        const char *err = nullptr;
        if (td && *td) {
            dir = td;
            mkdir(dir.c_str(), 0700);
        } else if (!ensureLinuxUpdatesDir(dir, &err)) {
            std::printf("dir failed: %s\n", err);
            return 1;
        }
        const std::string asset = (argc > 3) ? argv[3] : linuxAssetName();
        const std::string sumsUrl = linuxDownloadUrl(tag, kLinuxSumsName);
        const std::string assetUrl = linuxDownloadUrl(tag, asset);
        std::string sums, expect;
        if (!fetchLinuxSmall(dir, sumsUrl, sums, &err)) {
            std::printf("sums failed: %s\n", err ? err : "-");
            return 1;
        }
        if (!updateParseSha256Sums(sums, asset, expect)) {
            std::printf("asset %s not listed in %s\n", asset.c_str(), kLinuxSumsName);
            return 1;
        }
        std::printf("expected=%s\n", expect.c_str());
        const std::string file = dir + "/" + asset;
        std::string sha;
        long long got = 0;
        if (!fetchLinuxAssetToFile(assetUrl, file, sha, got, &err)) {
            std::printf("download failed: %s\n", err ? err : "-");
            return 1;
        }
        const bool match = lower(sha) == lower(expect);
        std::printf("download ok bytes=%lld sha256=%s match=%d\n", got, sha.c_str(),
                    (int)match);
        return match ? 0 : 1;
#else
        std::printf("download not supported on this platform\n");
        return 1;
#endif
    }
    if (mode == "helper" && argc > 2) {
#if defined(MIVARA_LINUX_UPDATER)
        const bool deb = (std::strcmp(argv[2], "deb") == 0);
        const std::string base = "/tmp/o'brien dir";
        const std::string script = deb
            ? buildLinuxHelperScript(base + "/Mivara-linux-arm64.deb", true, "",
                                     "/usr/bin/mivara", base + "/install.log",
                                     std::string(64, 'a'), "/usr/bin/sha256sum",
                                     "/usr/bin/pkexec", "/usr/bin/apt-get", "")
            : buildLinuxHelperScript(base + "/Mivara-linux-aarch64.tar.gz", false,
                                     base + "/.local", base + "/.local/bin/mivara",
                                     base + "/install.log", std::string(64, 'a'),
                                     "/usr/bin/sha256sum", "/usr/bin/pkexec", "",
                                     "/usr/bin/tar");
        std::printf("%s", script.c_str());
        return 0;
#else
        std::printf("helper not supported on this platform\n");
        return 1;
#endif
    }
    if (mode == "hash" && argc > 2) {
        std::string hex;
        if (!updateFileSha256(argv[2], hex)) { std::printf("hash failed\n"); return 1; }
        std::printf("%s\n", hex.c_str());
        return 0;
    }
    std::printf("usage: %s unit|check|download <tag> [asset]|helper deb|tar|hash <file>\n",
                argv[0]);
    return 2;
}
#endif /* UPDATE_TEST_MAIN */
