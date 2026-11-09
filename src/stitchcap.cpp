/**
 * STITCHCAP.CPP – Medium-integrity edge drop target for 拼接 file drops.
 *
 * The sender/controller is High integrity (requireAdministrator). UIPI stops a
 * Medium Explorer from dropping onto a High window, so the controller launches
 * this tiny asInvoker helper with the shell token. It owns a 3-pixel topmost
 * strip on the shared edge, registers IDropTarget, captures CF_HDROP on
 * DragEnter and forwards the path list over a named pipe. DragEnter/DragOver
 * advertise DROPEFFECT_COPY so OLE delivers Drop (rather than DragLeave) on
 * release; Drop itself returns DROPEFFECT_NONE, so it never performs a local
 * copy. The controller does the network transfer and the target commits at
 * the release point.
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

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "drop.h"

namespace {

/* Two one-way pipes: a duplex handle created without FILE_FLAG_OVERLAPPED is
 * a synchronous file object, and Windows serializes every operation on it — a
 * pending ReadFile blocks a WriteFile from another thread. This helper always
 * has a read pending for SIDE/QUIT, so a duplex handle would deadlock: it
 * could never report DRAG/FILES. g_tx carries paths to the controller, g_rx
 * carries the controller's commands. */
HANDLE g_tx = INVALID_HANDLE_VALUE;
HANDLE g_rx = INVALID_HANDLE_VALUE;
HWND   g_hwnd = nullptr;
int    g_side = 1;
std::mutex g_pipe_mtx;

const UINT WM_CAP_SIDE = WM_APP + 1;

int g_ref = 1;

/* Optional file trace for field debugging of the OLE capture path. Enabled by
 * setting MIVARA_STITCHCAP_TRACE=1 in the helper's environment; writes
 * %TEMP%\mivara-stitchcap-trace.log. Off (and free) by default. */
bool traceEnabled()
{
    static const bool enabled = [] {
        return GetEnvironmentVariableW(L"MIVARA_STITCHCAP_TRACE", nullptr, 0) > 0;
    }();
    return enabled;
}

void trace(const std::string &msg)
{
    if (!traceEnabled()) return;
    wchar_t tmp[MAX_PATH] = {};
    if (!GetTempPathW(MAX_PATH, tmp)) return;
    static HANDLE file = CreateFileW(
        (std::wstring(tmp) + L"mivara-stitchcap-trace.log").c_str(),
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

void sendLine(const std::string &line)
{
    std::lock_guard<std::mutex> lk(g_pipe_mtx);
    if (g_tx == INVALID_HANDLE_VALUE) return;
    trace("tx> " + line);
    DWORD wrote = 0;
    if (!WriteFile(g_tx, line.data(), (DWORD)line.size(), &wrote, nullptr))
        trace("tx failed err=" + std::to_string(GetLastError()));
}

/* Image chunks are large; never copy them into the trace log. */
void sendLineQuiet(const std::string &line)
{
    std::lock_guard<std::mutex> lk(g_pipe_mtx);
    if (g_tx == INVALID_HANDLE_VALUE) return;
    DWORD wrote = 0;
    if (!WriteFile(g_tx, line.data(), (DWORD)line.size(), &wrote, nullptr))
        trace("tx image failed err=" + std::to_string(GetLastError()));
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

/* Feasibility probe for the source's real drag image (trace builds only):
 * enumerate the formats Explorer exposes and try the two known names for the
 * SHDRAGIMAGE blob. The HBITMAP inside is process-local; the trace tells us
 * whether OLE actually marshals it across to this helper. */
std::string clipboardFormatName(CLIPFORMAT cf)
{
    wchar_t name[256] = {};
    const int n = GetClipboardFormatNameW(cf, name, 256);
    if (n > 0) return wideToUtf8(std::wstring(name, (size_t)n));
    return "cf=" + std::to_string((unsigned)cf);
}

void probeDragImage(IDataObject *data)
{
    if (!traceEnabled() || !data) return;

    IEnumFORMATETC *en = nullptr;
    if (SUCCEEDED(data->EnumFormatEtc(DATADIR_GET, &en)) && en) {
        FORMATETC fe = {};
        ULONG got = 0;
        while (en->Next(1, &fe, &got) == S_OK && got == 1) {
            trace("fmt " + clipboardFormatName(fe.cfFormat) +
                  " tymed=" + std::to_string((unsigned)fe.tymed));
            fe = {};
        }
        en->Release();
    }

    const wchar_t *names[] = {L"DragImageBits", L"ShellGetDragImage"};
    for (const wchar_t *name : names) {
        const CLIPFORMAT cf = (CLIPFORMAT)RegisterClipboardFormatW(name);
        FORMATETC fmt = {cf, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        STGMEDIUM stg = {};
        const HRESULT hr = data->GetData(&fmt, &stg);
        trace(std::string("dragimage ") + wideToUtf8(name) +
              " hr=" + std::to_string((long)hr));
        if (SUCCEEDED(hr)) {
            if (stg.tymed == TYMED_HGLOBAL && stg.hGlobal) {
                const SIZE_T sz = GlobalSize(stg.hGlobal);
                void *p = GlobalLock(stg.hGlobal);
                trace("  hglobal=" + std::to_string((unsigned long long)sz) +
                      " lock=" + (p ? "ok" : "null"));
                if (p && sz >= sizeof(SHDRAGIMAGE)) {
                    SHDRAGIMAGE sdi = {};
                    std::memcpy(&sdi, p, sizeof(sdi));
                    char head[3 * 40 + 1] = {};
                    const unsigned char *hb = (const unsigned char *)p;
                    for (int i = 0; i < 40 && (SIZE_T)(i + 1) <= sz; i++)
                        std::snprintf(head + i * 3, 4, "%02X ", hb[i]);
                    trace(std::string("  head=") + head);
                    /* The test source draws a green/red 64x64 square; its
                     * first pixel is 00 DC 00 FF in BGRA. Finding it tells us
                     * where the packed pixels start in the marshaled blob. */
                    for (int off = 0; off + 4 <= 128 &&
                                      (SIZE_T)(off + 4) <= sz; off++) {
                        if (std::memcmp(hb + off, "\x00\xDC\x00\xFF", 4) == 0)
                            trace("  testpixel@" + std::to_string(off));
                    }
                    trace("  size=" +
                          std::to_string((long)sdi.sizeDragImage.cx) + "x" +
                          std::to_string((long)sdi.sizeDragImage.cy) +
                          " off=" + std::to_string((long)sdi.ptOffset.x) +
                          "," + std::to_string((long)sdi.ptOffset.y) +
                          " hbmp=" +
                          std::to_string((unsigned long long)(uintptr_t)
                                             sdi.hbmpDragImage) +
                          " key=" +
                          std::to_string((unsigned long)sdi.crColorKey));
                    BITMAP bm = {};
                    if (sdi.hbmpDragImage &&
                        GetObjectW(sdi.hbmpDragImage, sizeof(bm), &bm)) {
                        trace("  bitmap=" + std::to_string(bm.bmWidth) + "x" +
                              std::to_string(bm.bmHeight) + " bpp=" +
                              std::to_string(bm.bmBitsPixel));
                    } else {
                        trace("  bitmap invalid err=" +
                              std::to_string(GetLastError()));
                    }
                }
                GlobalUnlock(stg.hGlobal);
            } else {
                trace("  tymed=" + std::to_string((unsigned)stg.tymed));
            }
            ReleaseStgMedium(&stg);
        }
    }
}

/* Downscale a packed premultiplied BGRA image to the protocol cap. Averaging
 * premultiplied channels is linear and keeps alpha edges correct. */
bool fitDragImage(std::vector<uint8_t> &px, int &w, int &h, int &ox, int &oy)
{
    if (w <= 0 || h <= 0) return false;
    const int64_t maxpx = (int64_t)DROP_DRAG_IMAGE_MAX_PIXELS;
    if ((int64_t)w * h <= maxpx) return true;
    const double scale = std::sqrt((double)maxpx / ((double)w * h));
    int nw = (int)(w * scale);
    int nh = (int)(h * scale);
    if (nw < 1) nw = 1;
    if (nh < 1) nh = 1;
    while ((int64_t)nw * nh > maxpx) {
        if (nw >= nh) nw--;
        else nh--;
    }
    if (nw < 1 || nh < 1) return false;
    std::vector<uint8_t> out((size_t)nw * nh * 4, 0);
    for (int y = 0; y < nh; y++) {
        const int y0 = (int)((long long)y * h / nh);
        int y1 = (int)((long long)(y + 1) * h / nh);
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < nw; x++) {
            const int x0 = (int)((long long)x * w / nw);
            int x1 = (int)((long long)(x + 1) * w / nw);
            if (x1 <= x0) x1 = x0 + 1;
            unsigned long long a = 0, r = 0, g = 0, b = 0;
            unsigned n = 0;
            for (int sy = y0; sy < y1; sy++) {
                const uint8_t *row = &px[((size_t)sy * w + x0) * 4];
                for (int sx = x0; sx < x1; sx++, row += 4) {
                    b += row[0];
                    g += row[1];
                    r += row[2];
                    a += row[3];
                    n++;
                }
            }
            uint8_t *d = &out[((size_t)y * nw + x) * 4];
            d[0] = (uint8_t)(b / n);
            d[1] = (uint8_t)(g / n);
            d[2] = (uint8_t)(r / n);
            d[3] = (uint8_t)(a / n);
        }
    }
    ox = (int)std::lround((double)ox * nw / w);
    oy = (int)std::lround((double)oy * nh / h);
    px.swap(out);
    w = nw;
    h = nh;
    return true;
}

/* Send the real Explorer drag image over the pipe as hex chunks. The blob is
 * already premultiplied BGRA; the controller/target blit it as-is. */
void sendDragImage(const std::vector<uint8_t> &px, int w, int h, int ox,
                   int oy)
{
    sendLine("IMG_BEGIN\t" + std::to_string(w) + "\t" + std::to_string(h) +
             "\t" + std::to_string(ox) + "\t" + std::to_string(oy) + "\n");
    static const char hex[] = "0123456789ABCDEF";
    const size_t chunk = 12000;   /* 24 KB of hex per pipe write */
    for (size_t off = 0; off < px.size(); off += chunk) {
        const size_t n = std::min(chunk, px.size() - off);
        std::string line = "IMG_CHUNK\t";
        line.reserve(10 + n * 2 + 1);
        for (size_t i = 0; i < n; i++) {
            const uint8_t v = px[off + i];
            line.push_back(hex[v >> 4]);
            line.push_back(hex[v & 0x0F]);
        }
        line.push_back('\n');
        sendLineQuiet(line);
    }
    sendLine("IMG_END\n");
}

/* Recover the source Explorer drag image from the marshaled DragImageBits
 * blob. OLE replaces the process-local HBITMAP with the packed pixels after a
 * 16-byte {w,h,offset} header and a 4/8-byte handle slot. */
void captureDragImage(IDataObject *data)
{
    static const CLIPFORMAT cf =
        (CLIPFORMAT)RegisterClipboardFormatW(L"DragImageBits");
    if (!cf) return;
    FORMATETC fmt = {cf, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM stg = {};
    if (FAILED(data->GetData(&fmt, &stg))) return;
    if (stg.tymed == TYMED_HGLOBAL && stg.hGlobal) {
        const SIZE_T sz = GlobalSize(stg.hGlobal);
        const uint8_t *p = (const uint8_t *)GlobalLock(stg.hGlobal);
        if (p && sz >= 16) {
            int w = 0, h = 0, ox = 0, oy = 0;
            std::memcpy(&w, p, 4);
            std::memcpy(&h, p + 4, 4);
            std::memcpy(&ox, p + 8, 4);
            std::memcpy(&oy, p + 12, 4);
            const int64_t pixels = (int64_t)w * h;
            /* Allow up to 4x the wire cap so a high-DPI image can be
             * downscaled; anything larger is left to the icon fallback. */
            if (w > 0 && h > 0 && pixels > 0 &&
                pixels <= 4 * (int64_t)DROP_DRAG_IMAGE_MAX_PIXELS) {
                const size_t bytes = (size_t)pixels * 4;
                const size_t header = sz >= bytes ? sz - bytes : 0;
                /* 16 bytes of {w,h,off} + a 4/8-byte HBITMAP slot is the
                 * layout we verified; accept a little slack for a color key
                 * or alignment padding and keep the pixel tail. */
                if (header >= 16 && header <= 64) {
                    std::vector<uint8_t> px(p + (sz - bytes), p + sz);
                    /* The marshaled DIB keeps the bitmap's bottom-up row
                     * order (verified with an asymmetric source: the first
                     * row of the blob is the source's bottom row). Convert to
                     * top-down once, before the wire, so the peer can blit
                     * the bytes directly. Swap whole rows only — reversing
                     * the buffer would also mirror every row and produce a
                     * 180-degree rotation. */
                    const size_t stride = (size_t)w * 4;
                    for (int y = 0; y < h / 2; y++) {
                        std::swap_ranges(
                            px.begin() + (size_t)y * stride,
                            px.begin() + (size_t)(y + 1) * stride,
                            px.begin() + (size_t)(h - 1 - y) * stride);
                    }
                    if (fitDragImage(px, w, h, ox, oy)) {
                        trace("dragimage captured " + std::to_string(w) + "x" +
                              std::to_string(h) + " off=" +
                              std::to_string(ox) + "," + std::to_string(oy));
                        sendDragImage(px, w, h, ox, oy);
                    }
                }
            }
        }
        GlobalUnlock(stg.hGlobal);
    }
    ReleaseStgMedium(&stg);
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
        /* Advertise COPY while the drag is over the strip: OLE only calls
         * IDropTarget::Drop when the last DragOver effect is not NONE. With
         * NONE it calls DragLeave on release, which would erase the captured
         * paths exactly when the controller needs them. Drop itself still
         * returns NONE, so nothing is ever copied locally. */
        if (effect) *effect = DROPEFFECT_COPY;
        if (!data) return S_OK;
        trace("DragEnter");
        /* Announce the gesture before the (potentially slower) path
         * extraction, so the controller can tell a file drag from an ordinary
         * click even when FILES_END is still in flight at release. */
        sendLine("DRAG\n");
        probeDragImage(data);
        FORMATETC fmt = {(CLIPFORMAT)CF_HDROP, nullptr, DVASPECT_CONTENT, -1,
                         TYMED_HGLOBAL};
        STGMEDIUM stg = {};
        const HRESULT get_hr = data->GetData(&fmt, &stg);
        trace("GetData hr=" + std::to_string((long)get_hr));
        if (FAILED(get_hr)) {
            /* DRAG already announced a gesture: never leave the controller
             * waiting out its 2 s hard cap for a list that will not come. */
            sendLine("NODRAG\n");
            return S_OK;
        }
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
        trace("DragEnter paths=" + std::to_string(paths.size()));
        if (paths.empty()) {
            /* Not an HDROP drag (text, an image, a failed render): no list is
             * coming, so clear the controller's "capture in flight" state at
             * once instead of making it wait for the hard cap. */
            sendLine("NODRAG\n");
        } else {
            /* The image must precede FILES_END: the controller takes both as
             * soon as the file list is published. */
            captureDragImage(data);
            sendFiles(paths);
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD *effect) override
    {
        /* Must stay non-NONE or OLE turns the release into DragLeave instead
         * of Drop (see DragEnter). */
        static bool first = true;
        if (first) { first = false; trace("DragOver"); }
        if (effect) *effect = DROPEFFECT_COPY;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override
    {
        trace("DragLeave");
        sendLine("NODRAG\n");
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject *, DWORD, POINTL,
                                   DWORD *effect) override
    {
        trace("Drop");
        if (effect) *effect = DROPEFFECT_NONE;
        sendLine("DROPPED\n");
        trace("Drop done");
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
    while (g_rx != INVALID_HANDLE_VALUE) {
        DWORD got = 0;
        if (!ReadFile(g_rx, buf.data(), (DWORD)buf.size(), &got, nullptr)) {
            /* The control pipe is the only lifeline: a closed server handle
             * means the controller crashed or exited. Do not leave an
             * invisible topmost 3-pixel drop target behind. */
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

    std::wstring pipeName;   /* tx: paths -> controller */
    std::wstring ctlName;    /* rx: controller commands (SIDE/QUIT) */
    int side = 1;
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        for (int i = 1; i + 1 < argc; i++) {
            if (wcscmp(argv[i], L"--pipe") == 0) pipeName = argv[i + 1];
            else if (wcscmp(argv[i], L"--ctl") == 0) ctlName = argv[i + 1];
            else if (wcscmp(argv[i], L"--side") == 0) side = _wtoi(argv[i + 1]);
        }
        LocalFree(argv);
    }
    if (pipeName.empty() || ctlName.empty()) return 2;
    if (side < 0 || side > 3) side = 1;

    /* Retry while the controller is still creating the server ends. */
    for (int i = 0; i < 100 && (g_tx == INVALID_HANDLE_VALUE ||
                                g_rx == INVALID_HANDLE_VALUE);
         i++) {
        if (g_tx == INVALID_HANDLE_VALUE)
            g_tx = CreateFileW(pipeName.c_str(), GENERIC_WRITE, 0, nullptr,
                               OPEN_EXISTING, 0, nullptr);
        if (g_rx == INVALID_HANDLE_VALUE)
            g_rx = CreateFileW(ctlName.c_str(), GENERIC_READ, 0, nullptr,
                               OPEN_EXISTING, 0, nullptr);
        if (g_tx == INVALID_HANDLE_VALUE || g_rx == INVALID_HANDLE_VALUE)
            Sleep(50);
    }
    if (g_tx == INVALID_HANDLE_VALUE || g_rx == INVALID_HANDLE_VALUE) {
        if (g_tx != INVALID_HANDLE_VALUE) CloseHandle(g_tx);
        if (g_rx != INVALID_HANDLE_VALUE) CloseHandle(g_rx);
        return 3;
    }
    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(g_rx, &mode, nullptr, nullptr);

    if (FAILED(OleInitialize(nullptr))) {
        CloseHandle(g_tx);
        CloseHandle(g_rx);
        return 4;
    }

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"MivaraStitchCap";
    if (!RegisterClassExW(&wc)) {
        OleUninitialize();
        CloseHandle(g_tx);
        CloseHandle(g_rx);
        return 5;
    }
    g_hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_LAYERED,
        wc.lpszClassName, L"Mivara Stitch Capture", WS_POPUP,
        0, 0, 1, 1, nullptr, nullptr, inst, nullptr);
    if (!g_hwnd) {
        OleUninitialize();
        CloseHandle(g_tx);
        CloseHandle(g_rx);
        return 6;
    }
    SetLayeredWindowAttributes(g_hwnd, 0, 1, LWA_ALPHA);
    ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
    g_side = side;
    placeWindow(g_side);
    if (FAILED(RegisterDragDrop(g_hwnd, &g_dropTarget))) {
        DestroyWindow(g_hwnd);
        OleUninitialize();
        CloseHandle(g_tx);
        CloseHandle(g_rx);
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
    if (g_tx != INVALID_HANDLE_VALUE) {
        CloseHandle(g_tx);
        g_tx = INVALID_HANDLE_VALUE;
    }
    if (g_rx != INVALID_HANDLE_VALUE) {
        CloseHandle(g_rx);
        g_rx = INVALID_HANDLE_VALUE;
    }
    return 0;
}
