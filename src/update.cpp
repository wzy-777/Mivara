/**
 * UPDATE.CPP – in-app update check + one-click updater (GUI app only)
 *
 * Design notes (see AGENTS.md / README):
 *  - The check uses the web redirect https://github.com/wzy-777/Mivara/releases/latest
 *    and reads the tag out of the 302 Location header. api.github.com is
 *    deliberately NOT used: unauthenticated requests are limited to 60/h per IP.
 *  - Windows downloads the installer itself; POSIX (dev builds) only checks and
 *    hands the release page to the desktop browser.
 *  - Trust chain for the executed installer: HTTPS with TLS verification +
 *    no https->http downgrade; the final URL after redirects must be on a
 *    GitHub-owned host or the body is never read; the tag is validated down to
 *    a fixed charset before it is used to build a URL or file name; the
 *    download must match Content-Length and the SHA-256 published in the
 *    release's SHA256SUMS.txt (no sums file => no in-app install), and the hash
 *    is re-verified on disk immediately before the installer is launched.
 *    The installer is written into %ProgramData%\Mivara\Updates with a DACL
 *    that denies write access to non-administrators, so a medium-integrity
 *    process cannot plant or swap the binary we execute.
 *  - Only github.com/wzy-777/Mivara is ever contacted; only a User-Agent is
 *    sent; nothing is uploaded.
 *
 * Local diagnostics (never built by the makefile, cf. ports.cpp -DPORTS_MAIN):
 *   g++ -std=c++17 -DWIN32_LEAN_AND_MEAN -DUPDATE_TEST_MAIN src/update.cpp \
 *       -o bin/upd_test.exe -lwinhttp -lshell32 -lbcrypt -ladvapi32
 *   bin/upd_test.exe unit | check | download <tag> | hash <file>
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
#  include <sys/wait.h>
#  include <unistd.h>
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
#ifdef _WIN32
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
#endif

void threadMain(bool download, std::string tag)
{
#ifdef _WIN32
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
#ifdef _WIN32
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
    if (detail == "size_mismatch") return "下载不完整";
    if (detail == "hash_mismatch") return "校验失败，安装包已删除";
    if (detail == "too_big")       return "响应数据过大";
    if (detail == "file_io")       return "文件读写失败";
    if (detail == "launch_failed") return "启动安装程序失败";
    if (detail == "no_curl")       return "未找到 curl";
    if (detail == "aborted")       return "已取消";
    return "内部错误";
}

bool updateCheckStart()
{
    return startWorker(false, std::string());
}

bool updateDownloadStart()
{
#ifdef _WIN32
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
#else
    /* POSIX builds never download an installer, so there is nothing to hash;
     * the launcher path is Windows-only. */
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
#else
        std::printf("download not supported on this platform\n");
        return 1;
#endif
    }
    if (mode == "hash" && argc > 2) {
        std::string hex;
        if (!updateFileSha256(argv[2], hex)) { std::printf("hash failed\n"); return 1; }
        std::printf("%s\n", hex.c_str());
        return 0;
    }
    std::printf("usage: %s unit|check|download <tag>|hash <file>\n", argv[0]);
    return 2;
}
#endif /* UPDATE_TEST_MAIN */
