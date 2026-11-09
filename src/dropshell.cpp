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
#  include <sddl.h>
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
        /* GetItemObject(SVGIO_BACKGROUND, IID_IFolderView) is the classic
         * route, but Windows 11 26200 returns E_NOINTERFACE for it (and for a
         * QI to IOleWindow on the view). The view object itself implements
         * IFolderView, and IShellView derives from IOleWindow, so both are
         * reachable directly — without this every drop over Explorer silently
         * fell back to Downloads\Mivara. */
        ComPtr<IFolderView> folderView;
        if (FAILED(view->QueryInterface(IID_IFolderView,
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
        /* IShellView inherits IOleWindow; a QI to IID_IOleWindow is refused
         * on Windows 11 26200, the inherited method is not. */
        view->GetWindow(&viewWnd);

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

/* ── icon placement at the release point ─────────────────────────────────── */

/* Grab IFolderView + the view HWND for `folder`, using the desktop's view for
   the Desktop folder and the matching Explorer window otherwise. */
static bool folderViewFor(const std::wstring &folder,
                          IFolderView **outView, HWND *outWnd)
{
    ComPtr<IShellWindows> windows;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr,
                                CLSCTX_LOCAL_SERVER, IID_IShellWindows,
                                (void **)&windows)) || !windows)
        return false;

    ComPtr<IDispatch> disp;
    ComPtr<IShellView> view;
    const std::wstring desktop = knownFolder(FOLDERID_Desktop);

    if (!desktop.empty() && samePath(folder, desktop)) {
        VARIANT loc, root;
        VariantInit(&loc);
        VariantInit(&root);
        LONG hwnd = 0;
        if (FAILED(windows->FindWindowSW(&loc, &root, SWC_DESKTOP, &hwnd,
                                         SWFO_NEEDDISPATCH, &disp)) ||
            !disp)
            return false;
    } else {
        long count = 0;
        if (FAILED(windows->get_Count(&count)) || count <= 0) return false;
        for (long i = 0; i < count && !disp; i++) {
            VARIANT v;
            VariantInit(&v);
            v.vt = VT_I4;
            v.lVal = i;
            ComPtr<IDispatch> cand;
            const HRESULT hr = windows->Item(v, &cand);
            VariantClear(&v);
            if (FAILED(hr) || !cand) continue;
            ComPtr<IWebBrowser2> wb;
            if (FAILED(cand->QueryInterface(IID_IWebBrowser2,
                                            (void **)&wb)) || !wb)
                continue;
            ComPtr<IServiceProvider> sp;
            if (FAILED(wb->QueryInterface(IID_IServiceProvider,
                                          (void **)&sp)) || !sp)
                continue;
            ComPtr<IShellBrowser> browser;
            if (FAILED(sp->QueryService(SID_STopLevelBrowser,
                                        IID_IShellBrowser,
                                        (void **)&browser)) || !browser)
                continue;
            ComPtr<IShellView> sv;
            if (FAILED(browser->QueryActiveShellView(&sv)) || !sv) continue;
            ComPtr<IFolderView> fv;
            if (FAILED(sv->QueryInterface(IID_IFolderView,
                                          (void **)&fv)) || !fv)
                continue;
            ComPtr<IShellFolder> sf;
            if (FAILED(fv->GetFolder(IID_IShellFolder, (void **)&sf)) || !sf)
                continue;
            ComPtr<IPersistFolder2> pf;
            if (FAILED(sf->QueryInterface(IID_IPersistFolder2,
                                          (void **)&pf)) || !pf)
                continue;
            PIDLIST_ABSOLUTE pidl = nullptr;
            if (FAILED(pf->GetCurFolder(&pidl)) || !pidl) continue;
            wchar_t path[32768] = {};
            const bool isFs =
                SHGetPathFromIDListW(pidl, path) != FALSE;
            CoTaskMemFree(pidl);
            if (isFs && samePath(path, folder)) {
                disp = cand;
                view = sv;
            }
        }
        if (!disp) return false;
    }

    ComPtr<IServiceProvider> sp;
    if (FAILED(disp->QueryInterface(IID_IServiceProvider, (void **)&sp)) || !sp)
        return false;
    ComPtr<IShellBrowser> browser;
    if (FAILED(sp->QueryService(SID_STopLevelBrowser, IID_IShellBrowser,
                                (void **)&browser)) || !browser)
        return false;
    if (!view) {
        if (FAILED(browser->QueryActiveShellView(&view)) || !view)
            return false;
    }
    ComPtr<IFolderView> fv;
    if (FAILED(view->QueryInterface(IID_IFolderView, (void **)&fv)) || !fv)
        return false;
    HWND wnd = nullptr;
    view->GetWindow(&wnd);
    if (!wnd) return false;
    *outView = fv.p;
    fv.p->AddRef();
    *outWnd = wnd;
    return true;
}

bool dropPlaceIcons(const std::wstring &dest_folder, int screen_x, int screen_y,
                    const std::vector<std::wstring> &names)
{
    if (dest_folder.empty() || names.empty()) return false;

    ComPtr<IFolderView> view;
    HWND viewWnd = nullptr;
    if (!folderViewFor(dest_folder, &view.p, &viewWnd)) return false;

    /* Auto-arranged folders re-snap every icon to the grid, so a requested
     * position cannot stick. Leave the copy alone and say nothing scary. */
    if (view->GetAutoArrange() == S_OK) return false;

    if (!IsWindowVisible(viewWnd)) return false;
    POINT pt = {screen_x, screen_y};
    if (!ScreenToClient(viewWnd, &pt)) return false;
    /* Keep the icon's top-left inside the view; the point is the drop spot. */
    RECT rc = {};
    GetClientRect(viewWnd, &rc);
    int spacing = 96;
    POINT sp = {};
    if (SUCCEEDED(view->GetSpacing(&sp)) && sp.y > 0) spacing = sp.y;

    int placed = 0;
    for (size_t i = 0; i < names.size(); i++) {
        const std::wstring full = dest_folder + L"\\" + names[i];
        PIDLIST_ABSOLUTE abs = nullptr;
        if (FAILED(SHParseDisplayName(full.c_str(), nullptr, &abs, 0,
                                      nullptr)) || !abs)
            continue;
        PCUITEMID_CHILD child = ILFindLastID(abs);
        POINT at = {pt.x, pt.y + (LONG)placed * spacing};
        if (at.x + 32 > rc.right) at.x = rc.right - 32;
        if (at.y + 32 > rc.bottom) at.y = rc.bottom - 32;
        if (at.x < 0) at.x = 0;
        if (at.y < 0) at.y = 0;
        if (SUCCEEDED(view->SelectAndPositionItems(
                1, &child, &at, SVSI_POSITIONITEM | SVSI_SELECT)))
            placed++;
        CoTaskMemFree(abs);
    }
    return placed > 0;
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
bool dropPlaceIcons(const std::wstring &, int, int,
                    const std::vector<std::wstring> &)
{
    return false;
}

#endif /* _WIN32 */

/* ── controller-side drag-capture helper ─────────────────────────────────── */

#ifdef _WIN32

namespace {

struct HelperState {
    std::mutex m;
    std::mutex write_m;
    /* Two ONE-WAY pipes, not one duplex pipe. A named-pipe handle created
     * without FILE_FLAG_OVERLAPPED is a synchronous file object, and the I/O
     * manager serializes every operation on it: a ReadFile pending on one
     * thread blocks a WriteFile from another thread (and vice versa) until the
     * read completes. The helper always has a read pending for SIDE/QUIT, so
     * a duplex handle would deadlock both directions — the helper could never
     * report DRAG/FILES, and this side could never send SIDE/QUIT. Each handle
     * below carries traffic in exactly one direction, so synchronous I/O is
     * safe. */
    HANDLE pipe = INVALID_HANDLE_VALUE;  /* inbound: helper -> controller */
    HANDLE ctl = INVALID_HANDLE_VALUE;   /* outbound: controller -> helper */
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
    std::atomic<bool> drop_called{false};
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
    std::atomic<uint64_t> drag_generation{0};
    std::atomic<uint64_t> dropped_generation{0};
    std::vector<std::string> files;
    uint64_t files_tick = 0;
    /* Real Explorer drag image from the current gesture (see drop.h). The
     * helper sends IMG_BEGIN/IMG_CHUNK/IMG_END before FILES_END; the reader
     * reconstructs it here and the ghost takes it once. Guarded by `m`. */
    DropDragImage image;
    std::vector<uint8_t> img_pending;
    int img_w = 0, img_h = 0, img_ox = 0, img_oy = 0;
    bool img_active = false;
    std::string pipe_name;
    int side = 1;
};

HelperState g_helper;

/* Optional controller-side pipe trace (field diagnostics for the edge-capture
 * path). MIVARA_DROP_TRACE=1 writes every helper message to
 * %TEMP%\mivara-dropctl-trace.log; off by default. */
void helperTrace(const std::string &msg)
{
    static const bool enabled = [] {
        return GetEnvironmentVariableW(L"MIVARA_DROP_TRACE", nullptr, 0) > 0;
    }();
    if (!enabled) return;
    wchar_t tmp[MAX_PATH] = {};
    if (!GetTempPathW(MAX_PATH, tmp)) return;
    static HANDLE file = CreateFileW(
        (std::wstring(tmp) + L"mivara-dropctl-trace.log").c_str(),
        GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    SetFilePointer(file, 0, nullptr, FILE_END);
    char line[1200];
    const int n = std::snprintf(line, sizeof(line), "[%llu pid=%lu] %s\n",
                                (unsigned long long)GetTickCount64(),
                                GetCurrentProcessId(), msg.c_str());
    if (n <= 0) return;
    DWORD wrote = 0;
    WriteFile(file, line, n < (int)sizeof(line) ? n : (int)sizeof(line) - 1,
              &wrote, nullptr);
}

int hexNibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Append hex bytes to an image accumulator, bounded by `max` bytes. */
bool hexAppend(const char *s, size_t n, std::vector<uint8_t> &out, size_t max)
{
    if (n % 2 != 0 || out.size() + n / 2 > max) return false;
    for (size_t i = 0; i < n; i += 2) {
        const int hi = hexNibble(s[i]);
        const int lo = hexNibble(s[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out.push_back((uint8_t)(hi * 16 + lo));
    }
    return true;
}

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

/* Both server pipes are opened overlapped. A synchronous ConnectNamedPipe /
 * ReadFile cannot be cancelled, so a helper that dies before connecting would
 * wedge dropHelperStop (and the sender's startup/teardown) forever. Overlapped
 * I/O lets the reader wait in short slices, notice g_helper.stop, and cancel. */
bool waitOverlapped(HANDLE h, OVERLAPPED &ov, DWORD &transferred)
{
    for (;;) {
        if (g_helper.stop.load()) {
            CancelIoEx(h, &ov);
            WaitForSingleObject(ov.hEvent, 1000);
            return false;
        }
        const DWORD w = WaitForSingleObject(ov.hEvent, 50);
        if (w == WAIT_OBJECT_0)
            return GetOverlappedResult(h, &ov, &transferred, FALSE) != FALSE;
        if (w != WAIT_TIMEOUT) return false;
    }
}

bool writeLine(const std::string &line)
{
    std::lock_guard<std::mutex> lk(g_helper.write_m);
    if (g_helper.ctl == INVALID_HANDLE_VALUE) return false;
    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) return false;
    DWORD wrote = 0;
    BOOL ok = WriteFile(g_helper.ctl, line.data(), (DWORD)line.size(), &wrote,
                        &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(ov.hEvent, 2000) == WAIT_OBJECT_0)
            ok = GetOverlappedResult(g_helper.ctl, &ov, &wrote, FALSE);
        else {
            CancelIoEx(g_helper.ctl, &ov);
            ok = FALSE;
        }
    }
    CloseHandle(ov.hEvent);
    return ok != FALSE;
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

std::wstring sidStringFromToken(HANDLE tok)
{
    if (!tok) return std::wstring();
    DWORD len = 0;
    GetTokenInformation(tok, TokenUser, nullptr, 0, &len);
    if (!len) return std::wstring();
    std::vector<BYTE> buf(len);
    if (!GetTokenInformation(tok, TokenUser, buf.data(), len, &len))
        return std::wstring();
    LPWSTR s = nullptr;
    if (!ConvertSidToStringSidW(((TOKEN_USER *)buf.data())->User.Sid, &s))
        return std::wstring();
    std::wstring out(s);
    LocalFree(s);
    return out;
}

/* The account the helper will run as: the shell's user (the helper is started
 * with a duplicated shell token), falling back to this process's user. */
std::wstring interactiveUserSid()
{
    HWND shell = GetShellWindow();
    if (shell) {
        DWORD pid = 0;
        GetWindowThreadProcessId(shell, &pid);
        if (pid) {
            HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                      pid);
            if (proc) {
                HANDLE tok = nullptr;
                std::wstring sid;
                if (OpenProcessToken(proc, TOKEN_QUERY, &tok)) {
                    sid = sidStringFromToken(tok);
                    CloseHandle(tok);
                }
                CloseHandle(proc);
                if (!sid.empty()) return sid;
            }
        }
    }
    HANDLE tok = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        const std::wstring sid = sidStringFromToken(tok);
        CloseHandle(tok);
        return sid;
    }
    return std::wstring();
}

/* The sender is elevated (High) while stitchcap runs at the shell's integrity
 * (Medium normally). A named pipe created with default attributes inherits the
 * creator's High mandatory label, and MIC's no-write-up rule then denies the
 * Medium helper's CreateFile(..., GENERIC_WRITE) — dropHelperStart would time
 * out with "did not become ready" and the feature would never work in the real
 * (elevated) app. Give both pipes a Medium label (NW) and a DACL for the
 * interactive user, so the helper can connect while Low-integrity sandboxes
 * still cannot. */
bool makePipeSecurity(SECURITY_ATTRIBUTES &sa, PSECURITY_DESCRIPTOR &sd)
{
    sa = {};
    sd = nullptr;
    const std::wstring sid = interactiveUserSid();
    if (sid.empty()) return false;
    const std::wstring sddl = L"D:(A;;GA;;;" + sid + L")S:(ML;;NW;;;ME)";
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.c_str(), SDDL_REVISION_1, &sd, nullptr))
        return false;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = sd;
    sa.bInheritHandle = FALSE;
    return true;
}

bool launchHelper(const std::wstring &exe, const std::wstring &dataPipe,
                  const std::wstring &ctlPipe, int side, HANDLE &proc,
                  int &shell_il)
{
    const std::wstring cmd = L"\"" + exe + L"\" --pipe \"" + dataPipe +
                             L"\" --ctl \"" + ctlPipe + L"\" --side " +
                             std::to_wstring(side);
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
    HANDLE ctl = g_helper.ctl;
    if (pipe == INVALID_HANDLE_VALUE || ctl == INVALID_HANDLE_VALUE) return;

    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) {
        g_helper.ready = false;
        return;
    }
    /* Reusing an OVERLAPPED requires clearing its status fields; only hEvent
     * survives (and is reset) between operations. */
    const auto prepare = [&ov] {
        const HANDLE ev = ov.hEvent;
        std::memset(&ov, 0, sizeof(ov));
        ov.hEvent = ev;
        ResetEvent(ev);
    };
    /* ConnectNamedPipe on an overlapped handle completes through the event;
     * ERROR_PIPE_CONNECTED means a client is already there. */
    const auto connect = [&](HANDLE h) -> bool {
        prepare();
        if (ConnectNamedPipe(h, &ov)) return true;
        const DWORD err = GetLastError();
        if (err == ERROR_PIPE_CONNECTED) return true;
        if (err != ERROR_IO_PENDING) return false;
        DWORD transferred = 0;
        return waitOverlapped(h, ov, transferred);
    };
    if (!connect(pipe)) {
        g_helper.ready = false;
        CloseHandle(ov.hEvent);
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
        CloseHandle(ov.hEvent);
        return;
    }
    /* The control pipe is the other direction of the same connection; check
     * that it is the same launched process too (it only carries SIDE/QUIT,
     * but a stray local client must not be fed the session's commands). */
    if (!connect(ctl)) {
        g_helper.ready = false;
        DisconnectNamedPipe(pipe);
        CloseHandle(ov.hEvent);
        return;
    }
    ULONG ctl_pid = 0;
    if (!GetNamedPipeClientProcessId(ctl, &ctl_pid) || !g_helper.proc ||
        ctl_pid != GetProcessId(g_helper.proc)) {
        g_helper.ready = false;
        DisconnectNamedPipe(pipe);
        DisconnectNamedPipe(ctl);
        CloseHandle(ov.hEvent);
        return;
    }
    std::vector<std::string> pending;
    std::vector<char> buf(1024 * 1024);
    std::string rx;
    /* The pipe is byte-mode: one ReadFile can return part of a line or
     * several complete lines (the real-drag-image transport writes many large
     * chunks back to back). Buffer by newline instead of assuming one read ==
     * one message, which would also corrupt a FILES line that coalesced with
     * a following IMG_END. */
    auto handleLine = [&](const std::string &msg) -> bool {
        if (msg == "DRAG") {
            g_helper.drag_generation.fetch_add(1);
            g_helper.drag_seen = true;
            g_helper.drag_pending = true;
            g_helper.drop_called = false;
            {
                std::lock_guard<std::mutex> lk(g_helper.m);
                g_helper.image = DropDragImage{};
                g_helper.img_pending.clear();
                g_helper.img_active = false;
            }
        } else if (msg == "DROPPED") {
            g_helper.dropped_generation = g_helper.drag_generation.load();
            g_helper.drop_called = true;
        } else if (msg.rfind("READY\t", 0) == 0) {
            const int il = (int)strtol(msg.c_str() + 6, nullptr, 0);
            g_helper.il = il;
            /* Publish ready BEFORE hello: dropHelperStart treats
             * "hello && !ready" as a final integrity verdict. Setting hello
             * first would let its 20 ms poll catch a valid helper in the
             * (hello=true, ready=false) window and refuse it. */
            g_helper.ready = (il <= g_helper.shell_il.load());
            g_helper.hello = true;
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
            if (g_helper.drop_called.load() &&
                g_helper.dropped_generation.load() ==
                    g_helper.drag_generation.load())
                return true;
            {
                std::lock_guard<std::mutex> lk(g_helper.m);
                g_helper.files.clear();
                g_helper.image = DropDragImage{};
                g_helper.img_pending.clear();
                g_helper.img_active = false;
                pending.clear();
            }
            g_helper.drag_live = false;
            g_helper.drop_called = false;
            g_helper.dropped_generation = 0;
            g_helper.drag_pending = false;
        } else if (msg.rfind("IMG_BEGIN\t", 0) == 0) {
            int w = 0, h = 0, ox = 0, oy = 0;
            const bool parsed =
                std::sscanf(msg.c_str(), "IMG_BEGIN\t%d\t%d\t%d\t%d",
                            &w, &h, &ox, &oy) == 4;
            std::lock_guard<std::mutex> lk(g_helper.m);
            g_helper.img_pending.clear();
            g_helper.img_w = w;
            g_helper.img_h = h;
            g_helper.img_ox = ox;
            g_helper.img_oy = oy;
            g_helper.img_active =
                parsed && w > 0 && h > 0 &&
                (uint64_t)w * (uint64_t)h <= DROP_DRAG_IMAGE_MAX_PIXELS;
        } else if (msg.rfind("IMG_CHUNK\t", 0) == 0) {
            std::lock_guard<std::mutex> lk(g_helper.m);
            if (g_helper.img_active) {
                const size_t max =
                    (size_t)g_helper.img_w * (size_t)g_helper.img_h * 4;
                if (!hexAppend(msg.c_str() + 10, msg.size() - 10,
                               g_helper.img_pending, max))
                    g_helper.img_active = false;
            }
        } else if (msg == "IMG_END") {
            std::lock_guard<std::mutex> lk(g_helper.m);
            const size_t need =
                (size_t)g_helper.img_w * (size_t)g_helper.img_h * 4;
            if (g_helper.img_active && need > 0 &&
                g_helper.img_pending.size() == need) {
                g_helper.image.w = g_helper.img_w;
                g_helper.image.h = g_helper.img_h;
                g_helper.image.ox = g_helper.img_ox;
                g_helper.image.oy = g_helper.img_oy;
                g_helper.image.px.swap(g_helper.img_pending);
            }
            g_helper.img_pending.clear();
            g_helper.img_active = false;
        } else if (msg == "BYE" || msg == "QUIT") {
            return false;
        }
        return true;
    };
    bool reader_done = false;
    while (!g_helper.stop.load() && !reader_done) {
        DWORD got = 0;
        prepare();
        if (!ReadFile(pipe, buf.data(), (DWORD)buf.size(), &got, &ov)) {
            if (GetLastError() != ERROR_IO_PENDING) break;
            if (!waitOverlapped(pipe, ov, got)) break;
        }
        if (got == 0) continue;
        rx.append(buf.data(), got);
        size_t nl;
        while ((nl = rx.find('\n')) != std::string::npos) {
            std::string msg = rx.substr(0, nl);
            rx.erase(0, nl + 1);
            if (!msg.empty() && msg.back() == '\r') msg.pop_back();
            if (msg.empty()) continue;
            if (msg.rfind("FILES\t", 0) == 0)
                helperTrace("rx< FILES (" + std::to_string(msg.size()) +
                            " bytes)");
            else if (msg.rfind("IMG_CHUNK\t", 0) == 0)
                helperTrace("rx< IMG_CHUNK (" + std::to_string(msg.size()) +
                            " chars)");
            else
                helperTrace("rx< " + msg);
            if (!handleLine(msg)) {
                reader_done = true;
                break;
            }
        }
        if (rx.size() > 4 * 1024 * 1024) rx.clear();
    }
    CloseHandle(ov.hEvent);
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
    const std::string base =
        "\\\\.\\pipe\\MivaraStitchCap-" + std::to_string(GetCurrentProcessId()) +
        "-" + std::to_string(GetTickCount64());
    const std::string ctl_name = base + "-ctl";
    const std::wstring wname(base.begin(), base.end());
    const std::wstring wctl(ctl_name.begin(), ctl_name.end());
    /* PIPE_REJECT_REMOTE_CLIENTS belongs to dwPipeMode, not dwOpenMode. Some
     * Windows builds silently ignore the misplaced bit, but Windows 11
     * 10.0.26200 rejects it with ERROR_INVALID_PARAMETER (87) — which used to
     * make dropHelperStart fail with "cannot create the capture pipe" and
     * disabled the whole drag-to-copy feature on that build.
     *
     * Each pipe carries one direction only (see HelperState): a synchronous
     * duplex handle would deadlock, because the helper's always-pending read
     * blocks its writes and vice versa. */
    SECURITY_ATTRIBUTES sa = {};
    PSECURITY_DESCRIPTOR sd = nullptr;
    const bool have_sa = makePipeSecurity(sa, sd);
    LPSECURITY_ATTRIBUTES lpsa = have_sa ? &sa : nullptr;
    HANDLE pipe = CreateNamedPipeW(
        wname.c_str(), PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT |
            PIPE_REJECT_REMOTE_CLIENTS,
        1, 1024 * 1024, 1024 * 1024, 0, lpsa);
    if (pipe == INVALID_HANDLE_VALUE) {
        if (sd) LocalFree(sd);
        err = "cannot create the capture pipe";
        return false;
    }
    HANDLE ctl = CreateNamedPipeW(
        wctl.c_str(), PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
        1024 * 1024, 1024 * 1024, 0, lpsa);
    if (sd) LocalFree(sd);
    if (ctl == INVALID_HANDLE_VALUE) {
        CloseHandle(pipe);
        err = "cannot create the capture control pipe";
        return false;
    }
    g_helper.stop = false;
    g_helper.ready = false;
    g_helper.hello = false;
    g_helper.pipe = pipe;
    g_helper.ctl = ctl;
    g_helper.pipe_name = base;
    g_helper.side = side;

    HANDLE proc = nullptr;
    int shell_il = 0x2000;
    if (!launchHelper(exe, wname, wctl, side, proc, shell_il)) {
        CloseHandle(pipe);
        CloseHandle(ctl);
        g_helper.pipe = INVALID_HANDLE_VALUE;
        g_helper.ctl = INVALID_HANDLE_VALUE;
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
    /* QUIT first (bounded: the ctl pipe is overlapped), then let the reader
     * notice the stop flag and cancel its pending overlapped I/O before the
     * handles are closed underneath it. */
    if (g_helper.ctl != INVALID_HANDLE_VALUE) writeLine("QUIT\n");
    if (g_helper.reader.joinable()) {
        if (g_helper.reader.get_id() != std::this_thread::get_id())
            g_helper.reader.join();
        else
            g_helper.reader.detach();
    }
    if (g_helper.ctl != INVALID_HANDLE_VALUE) {
        DisconnectNamedPipe(g_helper.ctl);
        CloseHandle(g_helper.ctl);
        g_helper.ctl = INVALID_HANDLE_VALUE;
    }
    if (g_helper.pipe != INVALID_HANDLE_VALUE) {
        DisconnectNamedPipe(g_helper.pipe);
        CloseHandle(g_helper.pipe);
        g_helper.pipe = INVALID_HANDLE_VALUE;
    }
    if (g_helper.proc) {
        if (WaitForSingleObject(g_helper.proc, 500) == WAIT_TIMEOUT)
            TerminateProcess(g_helper.proc, 0);
        CloseHandle(g_helper.proc);
        g_helper.proc = nullptr;
    }
    g_helper.ready = false;
    g_helper.drag_live = false;
    g_helper.drop_called = false;
    g_helper.drag_generation = 0;
    g_helper.dropped_generation = 0;
    g_helper.drag_seen = false;
    g_helper.drag_pending = false;
    std::lock_guard<std::mutex> lk(g_helper.m);
    g_helper.files.clear();
    g_helper.image = DropDragImage{};
    g_helper.img_pending.clear();
    g_helper.img_active = false;
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

bool dropHelperTakeDragImage(DropDragImage &out)
{
    std::lock_guard<std::mutex> lk(g_helper.m);
    if (!g_helper.image.valid()) return false;
    out = std::move(g_helper.image);
    g_helper.image = DropDragImage{};
    return true;
}

void dropHelperDiscard()
{
    {
        std::lock_guard<std::mutex> lk(g_helper.m);
        g_helper.files.clear();
        g_helper.image = DropDragImage{};
        g_helper.img_pending.clear();
        g_helper.img_active = false;
    }
    g_helper.drag_live = false;
    g_helper.drop_called = false;
    g_helper.drag_generation.fetch_add(1);
    g_helper.dropped_generation = 0;
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

size_t dropHelperFileCount()
{
    std::lock_guard<std::mutex> lk(g_helper.m);
    return g_helper.files.size();
}

bool dropHelperFirstFileName(std::string &out)
{
    std::lock_guard<std::mutex> lk(g_helper.m);
    if (g_helper.files.empty()) return false;
    const std::string &path = g_helper.files.front();
    size_t end = path.size();
    while (end > 0 && (path[end - 1] == '/' || path[end - 1] == '\\')) end--;
    const size_t sep = path.find_last_of("/\\", end ? end - 1 : 0);
    out = path.substr(sep == std::string::npos ? 0 : sep + 1,
                      end - (sep == std::string::npos ? 0 : sep + 1));
    return !out.empty();
}

bool dropHelperFirstFilePath(std::string &out)
{
    std::lock_guard<std::mutex> lk(g_helper.m);
    if (g_helper.files.empty()) return false;
    out = g_helper.files.front();
    return !out.empty();
}

bool dropHelperDropped()
{
    return g_helper.drop_called.load() &&
           g_helper.dropped_generation.load() ==
               g_helper.drag_generation.load();
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
size_t dropHelperFileCount() { return 0; }
bool dropHelperFirstFileName(std::string &) { return false; }
bool dropHelperFirstFilePath(std::string &) { return false; }
bool dropHelperTakeDragImage(DropDragImage &) { return false; }
bool dropHelperDropped() { return false; }
void dropHelperSetSide(int) {}

#endif /* _WIN32 */
