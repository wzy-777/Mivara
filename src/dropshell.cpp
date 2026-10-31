/**
 * DROPSHELL.CPP – Windows Shell/OLE half of the 8085 drop channel.
 *
 * Target side: resolve the folder under the release point (Explorer window or
 * desktop), validate it, and copy the staged top-level items there with the
 * shell's copy engine (copy-only, never overwrite).
 *
 * Controller side: launch the small asInvoker `stitchcap.exe` helper with the
 * shell token so its 1-pixel OLE drop target runs at Medium integrity. Explorer
 * (Medium) then delivers CF_HDROP to it; a High-integrity window would be
 * blocked by UIPI. The helper forwards the path list over a named pipe and
 * never copies anything locally.
 */
#include "drop.h"
#include "dropshell.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <shlobj.h>
#  include <shlwapi.h>
#  include <shobjidl.h>
#  include <exdisp.h>
#  include <shlguid.h>
#  include <shellapi.h>
#  include <knownfolders.h>
#endif

#ifdef _WIN32

namespace {

const std::atomic<bool> *g_drop_run = nullptr;

bool dropCancelled()
{
    return g_drop_run && !g_drop_run->load();
}

template <typename T>
struct ComPtr {
    T *p = nullptr;
    ~ComPtr() { reset(); }
    void reset()
    {
        if (p) { p->Release(); p = nullptr; }
    }
    T **operator&() { return &p; }
    T *operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

std::wstring lowerPath(std::wstring s)
{
    for (wchar_t &c : s)
        if (c >= L'A' && c <= L'Z') c = (wchar_t)(c - L'A' + L'a');
    while (!s.empty() && (s.back() == L'\\' || s.back() == L'/'))
        s.pop_back();
    if (s.rfind(L"\\\\?\\", 0) == 0) s.erase(0, 4);
    for (wchar_t &c : s)
        if (c == L'/') c = L'\\';
    return s;
}

bool samePath(const std::wstring &a, const std::wstring &b)
{
    return _wcsicmp(lowerPath(a).c_str(), lowerPath(b).c_str()) == 0;
}

bool underPath(const std::wstring &path, const std::wstring &root)
{
    if (root.empty()) return false;
    const std::wstring p = lowerPath(path);
    const std::wstring r = lowerPath(root);
    if (p == r) return true;
    return p.size() > r.size() && p.compare(0, r.size(), r) == 0 &&
           p[r.size()] == L'\\';
}

std::wstring knownFolder(REFKNOWNFOLDERID id)
{
    PWSTR p = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &p)) || !p)
        return std::wstring();
    std::wstring s(p);
    CoTaskMemFree(p);
    return s;
}

bool makeDirs(const std::wstring &path)
{
    if (path.empty()) return false;
    DWORD a = GetFileAttributesW(path.c_str());
    if (a != INVALID_FILE_ATTRIBUTES)
        return (a & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
               !(a & FILE_ATTRIBUTE_REPARSE_POINT);
    const size_t sep = path.find_last_of(L"\\/");
    if (sep != std::wstring::npos && sep > 2) {
        if (!makeDirs(path.substr(0, sep))) return false;
    }
    if (!CreateDirectoryW(path.c_str(), nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS)
        return false;
    a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) &&
           !(a & FILE_ATTRIBUTE_REPARSE_POINT);
}

/* Reject protected/self-loading locations even if the user could write them:
 * a remote session must never be able to drop an executable into a startup
 * path. The check is prefix-based with a separator boundary. */
bool blockedFolder(const std::wstring &path)
{
    static const KNOWNFOLDERID *const list[] = {
        &FOLDERID_Startup, &FOLDERID_CommonStartup, &FOLDERID_Windows,
        &FOLDERID_ProgramFiles, &FOLDERID_ProgramFilesX86,
        &FOLDERID_ProgramData, &FOLDERID_Public, &FOLDERID_PublicDesktop
    };
    for (const KNOWNFOLDERID *id : list) {
        const std::wstring root = knownFolder(*id);
        if (!root.empty() && underPath(path, root)) return true;
    }
    const std::wstring p = lowerPath(path);
    if (p.find(L"\\$recycle.bin") != std::wstring::npos ||
        p.find(L"\\system volume information") != std::wstring::npos)
        return true;
    return false;
}

bool validateFolder(const std::wstring &folder, std::string &why)
{
    if (folder.empty()) { why = "empty folder"; return false; }
    const DWORD attrs = GetFileAttributesW(folder.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES ||
        !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        why = "not a folder";
        return false;
    }
    if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) {
        why = "reparse point";
        return false;
    }
    if (lowerPath(folder).size() <= 2) {
        why = "volume root";
        return false;
    }
    wchar_t root[4] = {folder[0], L':', L'\\', 0};
    if (folder.size() < 3 || folder[1] != L':' ||
        GetDriveTypeW(root) != DRIVE_FIXED) {
        why = "not a local fixed drive";
        return false;
    }
    if (blockedFolder(folder)) {
        why = "protected location";
        return false;
    }
    HANDLE h = CreateFileW(folder.c_str(), FILE_READ_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE |
                           FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS |
                           FILE_FLAG_OPEN_REPARSE_POINT,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        why = "cannot open folder";
        return false;
    }
    FILE_ATTRIBUTE_TAG_INFO tag = {};
    const bool reparse =
        GetFileInformationByHandleEx(h, FileAttributeTagInfo, &tag,
                                     sizeof(tag)) &&
        (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
    wchar_t finalPath[32768] = {};
    const DWORD n = GetFinalPathNameByHandleW(
        h, finalPath, (DWORD)(sizeof(finalPath) / sizeof(finalPath[0])),
        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(h);
    if (reparse) { why = "reparse point"; return false; }
    if (n == 0) { why = "cannot resolve folder"; return false; }
    if (!samePath(folder, finalPath)) {
        /* A junction/symlink in any component changes the final path. */
        why = "folder resolves through a link";
        return false;
    }
    return true;
}

bool desktopFolder(std::wstring &out, std::string &why)
{
    out = knownFolder(FOLDERID_Desktop);
    if (out.empty()) { why = "desktop folder unavailable"; return false; }
    return validateFolder(out, why);
}

/* Find the Explorer browser whose view contains `screen`, else the browser
 * matching the top-level window. Handles Windows 11 tabs by preferring the
 * visible view that actually contains the point. */
bool explorerFolder(HWND root, POINT screen, HWND hit, std::wstring &out,
                    std::string &why)
{
    ComPtr<IShellWindows> windows;
    HRESULT hr = CoCreateInstance(CLSID_ShellWindows, nullptr,
                                  CLSCTX_LOCAL_SERVER, IID_IShellWindows,
                                  (void **)&windows);
    if (FAILED(hr) || !windows) { why = "shell windows unavailable"; return false; }

    long count = 0;
    if (FAILED(windows->get_Count(&count)) || count <= 0) {
        why = "no shell window";
        return false;
    }
    std::wstring fallback;
    bool haveFallback = false;
    for (long i = 0; i < count; i++) {
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_I4;
        v.lVal = i;
        ComPtr<IDispatch> disp;
        hr = windows->Item(v, &disp);
        VariantClear(&v);
        if (FAILED(hr) || !disp) continue;
        ComPtr<IWebBrowser2> wb;
        if (FAILED(disp->QueryInterface(IID_IWebBrowser2, (void **)&wb)) || !wb)
            continue;
        SHANDLE_PTR handle = 0;
        if (FAILED(wb->get_HWND(&handle)) || !handle) continue;
        if (GetAncestor((HWND)handle, GA_ROOT) != root) continue;

        ComPtr<IServiceProvider> sp;
        if (FAILED(wb->QueryInterface(IID_IServiceProvider, (void **)&sp)) || !sp)
            continue;
        ComPtr<IShellBrowser> browser;
        if (FAILED(sp->QueryService(SID_STopLevelBrowser, IID_IShellBrowser,
                                    (void **)&browser)) || !browser)
            continue;
        ComPtr<IShellView> view;
        if (FAILED(browser->QueryActiveShellView(&view)) || !view) continue;
        ComPtr<IFolderView> folderView;
        if (FAILED(view->GetItemObject(SVGIO_BACKGROUND, IID_IFolderView,
                                       (void **)&folderView)) || !folderView)
            continue;
        ComPtr<IShellFolder> shellFolder;
        if (FAILED(folderView->GetFolder(IID_IShellFolder,
                                         (void **)&shellFolder)) || !shellFolder)
            continue;
        ComPtr<IPersistFolder2> persist;
        if (FAILED(shellFolder->QueryInterface(IID_IPersistFolder2,
                                               (void **)&persist)) || !persist)
            continue;
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (FAILED(persist->GetCurFolder(&pidl)) || !pidl) continue;

        wchar_t current[32768] = {};
        const bool currentIsFs = SHGetPathFromIDListW(pidl, current) != FALSE;

        HWND viewWnd = nullptr;
        ComPtr<IOleWindow> oleWindow;
        if (SUCCEEDED(view->QueryInterface(IID_IOleWindow,
                                           (void **)&oleWindow)) && oleWindow)
            oleWindow->GetWindow(&viewWnd);

        /* Win11 tabs share one top-level HWND: the hidden tab's view keeps a
         * rect over the same screen area, so geometry alone can pick the wrong
         * tab. Require a visible view, and prefer the one that actually
         * contains the WindowFromPoint hit. */
        const bool viewVisible = viewWnd && IsWindowVisible(viewWnd);
        const bool underPointer =
            viewWnd && hit && (hit == viewWnd || IsChild(viewWnd, hit));
        bool used = false;
        if (viewWnd) {
            RECT rc = {};
            GetWindowRect(viewWnd, &rc);
            if (viewVisible && (underPointer || PtInRect(&rc, screen))) {
                /* A subfolder icon under the pointer wins over the current
                 * folder; a file or blank area keeps the current folder.
                 * MinGW's IFolderView has no ItemAt(), so hit-test the item
                 * cell from GetItemPosition + GetSpacing instead. */
                POINT client = screen;
                ScreenToClient(viewWnd, &client);
                int count = 0;
                if (SUCCEEDED(folderView->ItemCount(SVGIO_ALLVIEW, &count)) &&
                    count > 0) {
                    POINT spacing = {96, 96};
                    POINT sp = {};
                    if (SUCCEEDED(folderView->GetSpacing(&sp)) &&
                        sp.x > 0 && sp.y > 0)
                        spacing = sp;
                    for (int i = 0; i < count; i++) {
                        if (dropCancelled()) {
                            why = "cancelled";
                            return false;
                        }
                        PITEMID_CHILD child = nullptr;
                        if (FAILED(folderView->Item(i, &child)) || !child)
                            continue;
                        POINT pos = {};
                        const bool hit =
                            SUCCEEDED(folderView->GetItemPosition(child, &pos)) &&
                            client.x >= pos.x &&
                            client.x < pos.x + spacing.x &&
                            client.y >= pos.y &&
                            client.y < pos.y + spacing.y;
                        if (hit) {
                            PIDLIST_ABSOLUTE full = ILCombine(pidl, child);
                            wchar_t sub[32768] = {};
                            if (full && SHGetPathFromIDListW(full, sub)) {
                                const DWORD a = GetFileAttributesW(sub);
                                if (a != INVALID_FILE_ATTRIBUTES &&
                                    (a & FILE_ATTRIBUTE_DIRECTORY) &&
                                    !(a & FILE_ATTRIBUTE_REPARSE_POINT)) {
                                    out = sub;
                                    used = true;
                                }
                            }
                            if (full) ILFree(full);
                            CoTaskMemFree(child);
                            if (used) break;
                            continue;
                        }
                        CoTaskMemFree(child);
                    }
                }
                if (!used && currentIsFs) {
                    out = current;
                    used = true;
                }
            }
        }
        if (currentIsFs && viewVisible && !haveFallback) {
            fallback = current;
            haveFallback = true;
        }
        CoTaskMemFree(pidl);
        if (used) {
            if (!validateFolder(out, why)) return false;
            return true;
        }
    }
    if (haveFallback) {
        out = fallback;
        return validateFolder(out, why);
    }
    why = "no filesystem folder under the pointer";
    return false;
}

/* ── path uniqueness / shell copy ────────────────────────────────────────── */

std::wstring uniqueName(const std::wstring &dest, const std::wstring &name)
{
    const std::wstring candidate = dest + L"\\" + name;
    if (GetFileAttributesW(candidate.c_str()) == INVALID_FILE_ATTRIBUTES)
        return name;
    const wchar_t *extPtr = PathFindExtensionW(name.c_str());
    std::wstring base = name;
    std::wstring ext;
    if (extPtr && *extPtr) {
        ext = extPtr;
        base.resize((size_t)(extPtr - name.c_str()));
    }
    for (int i = 1; i < 1000; i++) {
        const std::wstring tryName =
            base + L" (" + std::to_wstring(i) + L")" + ext;
        if (GetFileAttributesW((dest + L"\\" + tryName).c_str()) ==
            INVALID_FILE_ATTRIBUTES)
            return tryName;
    }
    return base + L" (copy)" + ext;
}

bool shellCopy(const std::wstring &source, const std::wstring &target)
{
    std::vector<wchar_t> from(source.begin(), source.end());
    from.push_back(0); from.push_back(0);
    std::vector<wchar_t> to(target.begin(), target.end());
    to.push_back(0); to.push_back(0);
    SHFILEOPSTRUCTW op = {};
    op.wFunc = FO_COPY;
    op.pFrom = from.data();
    op.pTo = to.data();
    op.fFlags = FOF_SILENT | FOF_NOCONFIRMATION | FOF_NOERRORUI |
                FOF_NOCONFIRMMKDIR | FOF_RENAMEONCOLLISION;
    const int rc = SHFileOperationW(&op);
    return rc == 0 && !op.fAnyOperationsAborted;
}

} /* namespace */

void dropSetRunFlag(const std::atomic<bool> *running)
{
    g_drop_run = running;
}

/* ── public target-side API ──────────────────────────────────────────────── */

bool dropResolveFolder(int screen_x, int screen_y, std::wstring &folder,
                       std::string &why)
{
    POINT pt = {screen_x, screen_y};
    HWND hit = WindowFromPoint(pt);
    HWND root = hit ? GetAncestor(hit, GA_ROOT) : nullptr;
    if (!root) {
        /* No window at the point (unusual): treat it as the desktop. */
        return desktopFolder(folder, why);
    }
    wchar_t cls[64] = {};
    GetClassNameW(root, cls, (int)(sizeof(cls) / sizeof(cls[0])));
    if (_wcsicmp(cls, L"Progman") == 0 || _wcsicmp(cls, L"WorkerW") == 0)
        return desktopFolder(folder, why);
    if (_wcsicmp(cls, L"CabinetWClass") == 0 ||
        _wcsicmp(cls, L"ExploreWClass") == 0)
        return explorerFolder(root, pt, hit, folder, why);
    why = "the window under the pointer is not Explorer or the desktop";
    return false;
}

bool dropDefaultRecvDir(std::wstring &folder)
{
    std::wstring base = knownFolder(FOLDERID_Downloads);
    if (!base.empty()) {
        wchar_t root[4] = {base[0], L':', L'\\', 0};
        if (base.size() >= 3 && base[1] == L':' &&
            GetDriveTypeW(root) == DRIVE_FIXED) {
            folder = base + L"\\Mivara";
            if (makeDirs(folder)) return true;
        }
    }
    const std::wstring local = knownFolder(FOLDERID_LocalAppData);
    if (local.empty()) return false;
    folder = local + L"\\Mivara\\Received";
    return makeDirs(folder);
}

std::wstring dropStagingBase()
{
    const std::wstring local = knownFolder(FOLDERID_LocalAppData);
    if (local.empty()) return std::wstring();
    const std::wstring base = local + L"\\Mivara\\Drop";
    return makeDirs(base) ? base : std::wstring();
}

std::wstring dropStagingRoot(uint32_t transfer_id)
{
    const std::wstring base = dropStagingBase();
    if (base.empty()) return std::wstring();
    static volatile LONG seq = 0;
    const LONG n = InterlockedIncrement(&seq);
    const std::wstring root = base + L"\\" + std::to_wstring(transfer_id) +
                              L"-" + std::to_wstring(GetTickCount64()) +
                              L"-" + std::to_wstring((long long)n);
    return makeDirs(root) ? root : std::wstring();
}

bool dropCommitItems(const std::wstring &staging_root,
                     const std::wstring &dest_folder,
                     const std::vector<std::wstring> &top_level_names,
                     uint32_t &committed, uint32_t &failed,
                     std::string &message)
{
    committed = 0;
    failed = 0;
    /* Re-check at commit time: the folder may have been replaced by a link
     * or moved into a protected location while a large transfer was running. */
    std::string why;
    if (!validateFolder(dest_folder, why)) {
        failed = (uint32_t)top_level_names.size();
        message = "destination is no longer safe: " + why;
        return false;
    }
    /* Pin the validated directory for the whole copy: without delete sharing
     * it cannot be renamed/replaced by a junction after the check. (A parent
     * component can still be swapped; SHFileOperation cannot write relative to
     * a handle, so that residual TOCTOU is documented rather than claimed
     * closed.) */
    struct HandleGuard {
        HANDLE h = INVALID_HANDLE_VALUE;
        ~HandleGuard() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
    } guard;
    guard.h = CreateFileW(dest_folder.c_str(), FILE_READ_ATTRIBUTES,
                          FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                          OPEN_EXISTING,
                          FILE_FLAG_BACKUP_SEMANTICS |
                          FILE_FLAG_OPEN_REPARSE_POINT,
                          nullptr);
    if (guard.h == INVALID_HANDLE_VALUE) {
        failed = (uint32_t)top_level_names.size();
        message = "cannot pin destination";
        return false;
    }
    FILE_ATTRIBUTE_TAG_INFO tag = {};
    if (GetFileInformationByHandleEx(guard.h, FileAttributeTagInfo, &tag,
                                     sizeof(tag)) &&
        (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        failed = (uint32_t)top_level_names.size();
        message = "destination became a link";
        return false;
    }
    for (size_t i = 0; i < top_level_names.size(); i++) {
        if (dropCancelled()) {
            failed += (uint32_t)(top_level_names.size() - i);
            message = "cancelled";
            break;
        }
        const std::wstring &name = top_level_names[i];
        const std::wstring source = staging_root + L"\\" + name;
        if (GetFileAttributesW(source.c_str()) == INVALID_FILE_ATTRIBUTES) {
            failed++;
            if (message.empty()) message = "staged item missing";
            continue;
        }
        const std::wstring unique = uniqueName(dest_folder, name);
        const std::wstring target = dest_folder + L"\\" + unique;
        if (shellCopy(source, target)) {
            committed++;
        } else {
            failed++;
            /* Deliberately do not delete the target: FOF_RENAMEONCOLLISION
             * may have redirected the copy, and a concurrent process could
             * own a name that looks like ours. A partial file is reported,
             * never removed by guessing. */
            if (message.empty()) message = "copy failed";
        }
    }
    return committed > 0;
}

#else /* !_WIN32 */

bool dropResolveFolder(int, int, std::wstring &, std::string &why)
{
    why = "file drop is Windows only";
    return false;
}
bool dropDefaultRecvDir(std::wstring &) { return false; }
std::wstring dropStagingBase() { return std::wstring(); }
std::wstring dropStagingRoot(uint32_t) { return std::wstring(); }
bool dropCommitItems(const std::wstring &, const std::wstring &,
                     const std::vector<std::wstring> &,
                     uint32_t &committed, uint32_t &failed,
                     std::string &message)
{
    committed = failed = 0;
    message = "file drop is Windows only";
    return false;
}

#endif /* _WIN32 */

/* ── controller-side drag-capture helper ─────────────────────────────────── */

#ifdef _WIN32

namespace {

struct HelperState {
    std::mutex m;
    std::mutex write_m;
    HANDLE pipe = INVALID_HANDLE_VALUE;
    HANDLE proc = nullptr;
    std::thread reader;
    std::atomic<bool> stop{false};
    std::atomic<bool> ready{false};
    std::atomic<bool> hello{false};
    std::atomic<int> il{0x3000};
    std::atomic<int> shell_il{0x2000};
    /* True between the helper's FILES_END and the next NODRAG. The edge
     * release handler consults this so an Esc-cancelled drag is not turned
     * into a copy. Written by helperReader, read from the hook thread. */
    std::atomic<bool> drag_live{false};
    /* True from the helper's DragEnter ("DRAG") until the next left-button
     * down. Lets a release inside the capture grace window tell a real file
     * drag (wait for FILES_END) from an ordinary click (complete it now),
     * even when the captured list has not arrived yet. */
    std::atomic<bool> drag_seen{false};
    /* The helper announced a drag ("DRAG") but FILES_END has not arrived
     * yet. A slow CF_HDROP render (cloud/virtual files, a large selection)
     * can take far longer than the grace window, so the controller keeps the
     * press un-armed while this is set (bounded by a hard cap). Cleared by
     * FILES_END, NODRAG, discard and stop. */
    std::atomic<bool> drag_pending{false};
    std::vector<std::string> files;
    uint64_t files_tick = 0;
    std::string pipe_name;
    int side = 1;
};

HelperState g_helper;

int tokenIntegrity(HANDLE tok)
{
    if (!tok) return 0x2000;
    DWORD size = 0;
    GetTokenInformation(tok, TokenIntegrityLevel, nullptr, 0, &size);
    if (!size) return 0x2000;
    std::vector<BYTE> buf(size);
    if (!GetTokenInformation(tok, TokenIntegrityLevel, buf.data(), size,
                             &size))
        return 0x2000;
    auto *label = (TOKEN_MANDATORY_LABEL *)buf.data();
    if (!label->Label.Sid) return 0x2000;
    const DWORD count = *GetSidSubAuthorityCount(label->Label.Sid);
    if (!count) return 0x2000;
    return (int)*GetSidSubAuthority(label->Label.Sid, count - 1);
}

bool writeLine(const std::string &line)
{
    std::lock_guard<std::mutex> lk(g_helper.write_m);
    if (g_helper.pipe == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    return WriteFile(g_helper.pipe, line.data(), (DWORD)line.size(), &wrote,
                     nullptr) != FALSE;
}

std::wstring helperExePath()
{
    wchar_t path[32768] = {};
    DWORD n = GetModuleFileNameW(nullptr, path,
                                 (DWORD)(sizeof(path) / sizeof(path[0])));
    if (n == 0 || n >= sizeof(path) / sizeof(path[0])) return std::wstring();
    std::wstring s(path, n);
    const size_t sep = s.find_last_of(L"\\/");
    if (sep == std::wstring::npos) return std::wstring();
    return s.substr(0, sep + 1) + L"stitchcap.exe";
}

bool launchHelper(const std::wstring &exe, const std::wstring &pipe,
                  int side, HANDLE &proc, int &shell_il)
{
    const std::wstring cmd = L"\"" + exe + L"\" --pipe \"" + pipe +
                             L"\" --side " + std::to_wstring(side);
    std::wstring mutableCmd = cmd;
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};

    /* Duplicate the shell's token so the helper runs at Medium integrity.
     * The sender itself is High (requireAdministrator), and UIPI would hide
     * every Medium Explorer drop from a High drop target. */
    HANDLE shellProcess = nullptr;
    HANDLE shellToken = nullptr;
    HANDLE primary = nullptr;
    HWND shell = GetShellWindow();
    if (shell) {
        DWORD pid = 0;
        GetWindowThreadProcessId(shell, &pid);
        if (pid) {
            shellProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                                       FALSE, pid);
            if (shellProcess &&
                OpenProcessToken(shellProcess,
                                 TOKEN_DUPLICATE | TOKEN_QUERY |
                                 TOKEN_ASSIGN_PRIMARY,
                                 &shellToken)) {
                DuplicateTokenEx(shellToken, MAXIMUM_ALLOWED, nullptr,
                                 SecurityImpersonation, TokenPrimary,
                                 &primary);
            }
        }
    }
    /* The helper may drop onto whatever the shell can drop onto: Medium when
     * UAC is on, High when Explorer itself is elevated. */
    shell_il = tokenIntegrity(primary ? primary : shellToken);
    BOOL ok = FALSE;
    if (primary) {
        ok = CreateProcessWithTokenW(primary, LOGON_WITH_PROFILE, exe.c_str(),
                                     &mutableCmd[0],
                                     CREATE_UNICODE_ENVIRONMENT |
                                     CREATE_NO_WINDOW,
                                     nullptr, nullptr, &si, &pi);
    }
    if (!ok) {
        /* Fallback: run it, but it will be High and report that; the
         * controller then disables drag capture instead of advertising a
         * target Explorer can never reach. */
        std::wstring cmd2 = cmd;
        ok = CreateProcessW(exe.c_str(), &cmd2[0], nullptr, nullptr, FALSE,
                            CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW,
                            nullptr, nullptr, &si, &pi);
    }
    if (primary) CloseHandle(primary);
    if (shellToken) CloseHandle(shellToken);
    if (shellProcess) CloseHandle(shellProcess);
    if (!ok) return false;
    CloseHandle(pi.hThread);
    proc = pi.hProcess;
    return true;
}

void helperReader()
{
    HANDLE pipe = g_helper.pipe;
    if (pipe == INVALID_HANDLE_VALUE) return;
    const BOOL connected = ConnectNamedPipe(pipe, nullptr);
    if (!connected && GetLastError() != ERROR_PIPE_CONNECTED) {
        g_helper.ready = false;
        return;
    }
    /* Same-user Medium processes can enumerate the pipe name and race the
     * helper. The kernel tells us the client PID; only the process we launched
     * may speak on this pipe. */
    ULONG client_pid = 0;
    if (!GetNamedPipeClientProcessId(pipe, &client_pid) || !g_helper.proc ||
        client_pid != GetProcessId(g_helper.proc)) {
        g_helper.ready = false;
        DisconnectNamedPipe(pipe);
        return;
    }
    std::vector<std::string> pending;
    std::vector<char> buf(1024 * 1024);
    while (!g_helper.stop.load()) {
        DWORD got = 0;
        if (!ReadFile(pipe, buf.data(), (DWORD)buf.size(), &got, nullptr))
            break;
        if (got == 0) continue;
        std::string msg(buf.data(), got);
        while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r'))
            msg.pop_back();
        if (msg == "DRAG") {
            g_helper.drag_seen = true;
            g_helper.drag_pending = true;
        } else if (msg.rfind("READY\t", 0) == 0) {
            const int il = (int)strtol(msg.c_str() + 6, nullptr, 0);
            g_helper.il = il;
            g_helper.hello = true;
            g_helper.ready = (il <= g_helper.shell_il.load());
            writeLine("SIDE\t" + std::to_string(g_helper.side) + "\n");
        } else if (msg == "FILES_BEGIN") {
            pending.clear();
        } else if (msg.rfind("FILES\t", 0) == 0) {
            size_t start = 6;
            while (start <= msg.size()) {
                const size_t tab = msg.find('\t', start);
                const std::string p = msg.substr(
                    start, tab == std::string::npos ? std::string::npos
                                                    : tab - start);
                if (!p.empty()) pending.push_back(p);
                if (tab == std::string::npos) break;
                start = tab + 1;
            }
        } else if (msg == "FILES_END") {
            {
                std::lock_guard<std::mutex> lk(g_helper.m);
                g_helper.files = pending;
                g_helper.files_tick = GetTickCount64();
                pending.clear();
            }
            g_helper.drag_live = true;
            g_helper.drag_seen = true;
            g_helper.drag_pending = false;
        } else if (msg == "NODRAG") {
            {
                std::lock_guard<std::mutex> lk(g_helper.m);
                g_helper.files.clear();
                pending.clear();
            }
            g_helper.drag_live = false;
            g_helper.drag_pending = false;
        } else if (msg == "BYE" || msg == "QUIT") {
            break;
        }
    }
    g_helper.ready = false;
}

} /* namespace */

bool dropHelperStart(int side, std::string &err)
{
    if (g_helper.ready.load()) {
        dropHelperSetSide(side);
        return true;
    }
    const std::wstring exe = helperExePath();
    if (exe.empty() || GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        err = "stitchcap.exe is missing";
        return false;
    }
    const std::string name =
        "\\\\.\\pipe\\MivaraStitchCap-" + std::to_string(GetCurrentProcessId()) +
        "-" + std::to_string(GetTickCount64());
    const std::wstring wname(name.begin(), name.end());
    HANDLE pipe = CreateNamedPipeW(
        wname.c_str(), PIPE_ACCESS_DUPLEX | PIPE_REJECT_REMOTE_CLIENTS,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT, 1,
        1024 * 1024, 1024 * 1024, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        err = "cannot create the capture pipe";
        return false;
    }
    g_helper.stop = false;
    g_helper.ready = false;
    g_helper.hello = false;
    g_helper.pipe = pipe;
    g_helper.pipe_name = name;
    g_helper.side = side;

    HANDLE proc = nullptr;
    int shell_il = 0x2000;
    if (!launchHelper(exe, wname, side, proc, shell_il)) {
        CloseHandle(pipe);
        g_helper.pipe = INVALID_HANDLE_VALUE;
        err = "cannot launch stitchcap.exe";
        return false;
    }
    g_helper.proc = proc;
    g_helper.shell_il = shell_il;
    g_helper.reader = std::thread(helperReader);

    const DWORD start = GetTickCount();
    while ((DWORD)(GetTickCount() - start) < 3000) {
        if (g_helper.ready.load()) return true;
        if (g_helper.hello.load() && !g_helper.ready.load()) {
            err = "stitchcap.exe runs above the shell's integrity level";
            break;
        }
        Sleep(20);
    }
    if (!g_helper.ready.load() && err.empty())
        err = "stitchcap.exe did not become ready";
    dropHelperStop();
    return false;
}

void dropHelperStop()
{
    g_helper.stop = true;
    if (g_helper.pipe != INVALID_HANDLE_VALUE) {
        writeLine("QUIT\n");
        DisconnectNamedPipe(g_helper.pipe);
        CloseHandle(g_helper.pipe);
        g_helper.pipe = INVALID_HANDLE_VALUE;
    }
    if (g_helper.reader.joinable()) {
        if (g_helper.reader.get_id() != std::this_thread::get_id())
            g_helper.reader.join();
        else
            g_helper.reader.detach();
    }
    if (g_helper.proc) {
        if (WaitForSingleObject(g_helper.proc, 500) == WAIT_TIMEOUT)
            TerminateProcess(g_helper.proc, 0);
        CloseHandle(g_helper.proc);
        g_helper.proc = nullptr;
    }
    g_helper.ready = false;
    g_helper.drag_live = false;
    g_helper.drag_seen = false;
    g_helper.drag_pending = false;
    std::lock_guard<std::mutex> lk(g_helper.m);
    g_helper.files.clear();
}

bool dropHelperReady()
{
    return g_helper.ready.load();
}

bool dropHelperTakeFiles(std::vector<std::string> &out)
{
    std::lock_guard<std::mutex> lk(g_helper.m);
    if (g_helper.files.empty()) return false;
    out.swap(g_helper.files);
    g_helper.files.clear();
    return !out.empty();
}

void dropHelperDiscard()
{
    {
        std::lock_guard<std::mutex> lk(g_helper.m);
        g_helper.files.clear();
    }
    g_helper.drag_live = false;
    g_helper.drag_seen = false;
    g_helper.drag_pending = false;
}

bool dropHelperDragLive()
{
    return g_helper.drag_live.load();
}

bool dropHelperDragSeen()
{
    return g_helper.drag_seen.load();
}

bool dropHelperDragPending()
{
    return g_helper.drag_pending.load();
}

void dropHelperSetSide(int side)
{
    if (side < 0 || side > 3) return;
    g_helper.side = side;
    writeLine("SIDE\t" + std::to_string(side) + "\n");
}

#else /* !_WIN32 */

void dropSetRunFlag(const std::atomic<bool> *) {}
bool dropHelperStart(int, std::string &err)
{
    err = "file drop is Windows only";
    return false;
}
void dropHelperStop() {}
bool dropHelperReady() { return false; }
bool dropHelperTakeFiles(std::vector<std::string> &) { return false; }
void dropHelperDiscard() {}
bool dropHelperDragLive() { return false; }
bool dropHelperDragSeen() { return false; }
bool dropHelperDragPending() { return false; }
void dropHelperSetSide(int) {}

#endif /* _WIN32 */
