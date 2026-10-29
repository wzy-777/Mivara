/**
 * STITCHCAP.CPP – Medium-integrity edge drop target for 拼接 file drops.
 *
 * The sender/controller is High integrity (requireAdministrator). UIPI stops a
 * Medium Explorer from dropping onto a High window, so the controller launches
 * this tiny asInvoker helper with the shell token. It owns a 3-pixel topmost
 * strip on the shared edge, registers IDropTarget, captures CF_HDROP on
 * DragEnter and forwards the path list over a named pipe. It returns
 * DROPEFFECT_NONE, so it never performs a local copy; the controller does the
 * network transfer and the target commits at the release point.
 *
 * No file data is read here. Only the dragged paths are forwarded.
 */
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include <ole2.h>
#include <shellapi.h>
#include <shlobj.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

HANDLE g_pipe = INVALID_HANDLE_VALUE;
HWND   g_hwnd = nullptr;
int    g_side = 1;
std::mutex g_pipe_mtx;

const UINT WM_CAP_SIDE = WM_APP + 1;

int g_ref = 1;

void sendLine(const std::string &line)
{
    std::lock_guard<std::mutex> lk(g_pipe_mtx);
    if (g_pipe == INVALID_HANDLE_VALUE) return;
    DWORD wrote = 0;
    WriteFile(g_pipe, line.data(), (DWORD)line.size(), &wrote, nullptr);
}

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

void placeWindow(int side)
{
    if (!g_hwnd) return;
    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    /* 3 px, not 1: the extra lead lets OLE deliver DragEnter before the
     * controller's edge overshoot triggers enterRemote (no blocking wait is
     * allowed on the hook thread). */
    const int band = 3;
    int x = vx, y = vy, w = band, h = vh;
    switch (side) {
    case 0: x = vx;                     y = vy; w = band; h = vh; break;
    case 2: x = vx;                     y = vy; w = vw; h = band; break;
    case 3: x = vx;                     y = vy + vh - band; w = vw; h = band; break;
    default: x = vx + vw - band;        y = vy; w = band; h = vh; break;
    }
    SetWindowPos(g_hwnd, HWND_TOPMOST, x, y, w, h,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void sendFiles(const std::vector<std::wstring> &paths)
{
    if (paths.empty()) return;
    /* One message per ~32 KiB chunk: a 1000-path selection must not become a
     * single message larger than the controller's pipe read buffer. */
    sendLine("FILES_BEGIN\n");
    std::string chunk;
    for (const std::wstring &p : paths) {
        const std::string u = wideToUtf8(p);
        if (u.empty()) continue;
        if (!chunk.empty() && chunk.size() + u.size() + 1 > 32000) {
            sendLine("FILES\t" + chunk + "\n");
            chunk.clear();
        }
        if (!chunk.empty()) chunk.push_back('\t');
        chunk += u;
    }
    if (!chunk.empty()) sendLine("FILES\t" + chunk + "\n");
    sendLine("FILES_END\n");
}

class DropTarget : public IDropTarget {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override
    {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDropTarget) {
            *ppv = static_cast<IDropTarget *>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return (ULONG)InterlockedIncrement((LONG *)&g_ref);
    }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const LONG n = InterlockedDecrement((LONG *)&g_ref);
        return (ULONG)n;
    }

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject *data, DWORD, POINTL,
                                        DWORD *effect) override
    {
        if (effect) *effect = DROPEFFECT_NONE;
        if (!data) return S_OK;
        FORMATETC fmt = {(CLIPFORMAT)CF_HDROP, nullptr, DVASPECT_CONTENT, -1,
                         TYMED_HGLOBAL};
        STGMEDIUM stg = {};
        if (FAILED(data->GetData(&fmt, &stg))) return S_OK;
        std::vector<std::wstring> paths;
        if (stg.tymed == TYMED_HGLOBAL && stg.hGlobal) {
            HDROP drop = (HDROP)GlobalLock(stg.hGlobal);
            if (drop) {
                const UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
                for (UINT i = 0; i < n && i < 1000; i++) {
                    wchar_t buf[32768] = {};
                    const UINT len = DragQueryFileW(
                        drop, i, buf, (UINT)(sizeof(buf) / sizeof(buf[0])));
                    if (len > 0) paths.emplace_back(buf, len);
                }
                GlobalUnlock(stg.hGlobal);
            }
        }
        ReleaseStgMedium(&stg);
        if (!paths.empty()) sendFiles(paths);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD *effect) override
    {
        if (effect) *effect = DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override
    {
        sendLine("NODRAG\n");
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject *, DWORD, POINTL,
                                   DWORD *effect) override
    {
        if (effect) *effect = DROPEFFECT_NONE;
        /* Drop is not always followed by DragLeave; clear the captured list
         * so a release on the strip cannot leak into the next gesture. */
        sendLine("NODRAG\n");
        return S_OK;
    }
};

DropTarget g_dropTarget;

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CAP_SIDE:
        g_side = (int)wp;
        placeWindow(g_side);
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

DWORD integrityLevel()
{
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok))
        return 0x3000;
    DWORD size = 0;
    GetTokenInformation(tok, TokenIntegrityLevel, nullptr, 0, &size);
    std::vector<BYTE> buf(size);
    DWORD il = 0x3000;
    if (size && GetTokenInformation(tok, TokenIntegrityLevel, buf.data(),
                                    size, &size)) {
        auto *label = (TOKEN_MANDATORY_LABEL *)buf.data();
        if (label->Label.Sid) {
            const DWORD count = *GetSidSubAuthorityCount(label->Label.Sid);
            if (count > 0)
                il = *GetSidSubAuthority(label->Label.Sid, count - 1);
        }
    }
    CloseHandle(tok);
    return il;
}

void pipeReader()
{
    std::vector<char> buf(4096);
    while (g_pipe != INVALID_HANDLE_VALUE) {
        DWORD got = 0;
        if (!ReadFile(g_pipe, buf.data(), (DWORD)buf.size(), &got, nullptr)) {
            /* The pipe is the only lifeline: a closed server handle means the
             * controller crashed or exited. Do not leave an invisible topmost
             * 3-pixel drop target behind. */
            if (g_hwnd) PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
            break;
        }
        if (got == 0) continue;
        std::string msg(buf.data(), got);
        while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r'))
            msg.pop_back();
        if (msg.rfind("SIDE\t", 0) == 0) {
            const int side = atoi(msg.c_str() + 5);
            if (g_hwnd) PostMessageW(g_hwnd, WM_CAP_SIDE, (WPARAM)side, 0);
        } else if (msg == "QUIT" || msg == "BYE") {
            if (g_hwnd) PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
            break;
        }
    }
}

void enableDpiAwareness()
{
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) return;
    typedef BOOL(WINAPI *SetCtxFn)(void *);
    auto fn = (SetCtxFn)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
    if (fn) { fn((void *)(intptr_t)-4); return; }
    typedef BOOL(WINAPI *SetDpiFn)(void);
    auto old = (SetDpiFn)GetProcAddress(user32, "SetProcessDPIAware");
    if (old) old();
}

} /* namespace */

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int)
{
    enableDpiAwareness();

    std::wstring pipeName;
    int side = 1;
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        for (int i = 1; i + 1 < argc; i++) {
            if (wcscmp(argv[i], L"--pipe") == 0) pipeName = argv[i + 1];
            else if (wcscmp(argv[i], L"--side") == 0) side = _wtoi(argv[i + 1]);
        }
        LocalFree(argv);
    }
    if (pipeName.empty()) return 2;
    if (side < 0 || side > 3) side = 1;

    /* Retry while the controller is still creating the server end. */
    for (int i = 0; i < 100 && g_pipe == INVALID_HANDLE_VALUE; i++) {
        g_pipe = CreateFileW(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                             nullptr, OPEN_EXISTING, 0, nullptr);
        if (g_pipe == INVALID_HANDLE_VALUE) Sleep(50);
    }
    if (g_pipe == INVALID_HANDLE_VALUE) return 3;
    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(g_pipe, &mode, nullptr, nullptr);

    if (FAILED(OleInitialize(nullptr))) {
        CloseHandle(g_pipe);
        return 4;
    }

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"MivaraStitchCap";
    if (!RegisterClassExW(&wc)) {
        OleUninitialize();
        CloseHandle(g_pipe);
        return 5;
    }
    g_hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_LAYERED,
        wc.lpszClassName, L"Mivara Stitch Capture", WS_POPUP,
        0, 0, 1, 1, nullptr, nullptr, inst, nullptr);
    if (!g_hwnd) {
        OleUninitialize();
        CloseHandle(g_pipe);
        return 6;
    }
    SetLayeredWindowAttributes(g_hwnd, 0, 1, LWA_ALPHA);
    ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
    g_side = side;
    placeWindow(g_side);
    if (FAILED(RegisterDragDrop(g_hwnd, &g_dropTarget))) {
        DestroyWindow(g_hwnd);
        OleUninitialize();
        CloseHandle(g_pipe);
        return 7;
    }

    char ready[64];
    std::snprintf(ready, sizeof(ready), "READY\t0x%X\n",
                  (unsigned)integrityLevel());
    sendLine(ready);

    std::thread reader(pipeReader);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    RevokeDragDrop(g_hwnd);
    if (g_hwnd) DestroyWindow(g_hwnd);
    OleUninitialize();
    if (reader.joinable()) reader.detach();
    if (g_pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(g_pipe);
        g_pipe = INVALID_HANDLE_VALUE;
    }
    return 0;
}
