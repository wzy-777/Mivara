/**
 * CAPTURE_DDA.CPP – DXGI Desktop Duplication capture (see capture_dda.h).
 *
 * Windows only. Every entry point is a safe no-op stub on other platforms,
 * so the sender can build this translation unit everywhere.
 */
#include "capture_dda.h"

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

struct DdaState {
    IDXGIFactory1          *factory = nullptr;
    ID3D11Device           *device  = nullptr;
    ID3D11DeviceContext    *ctx     = nullptr;
    IDXGIAdapter1          *adapter = nullptr;
    IDXGIOutput1           *output  = nullptr;
    IDXGIOutputDuplication *dup     = nullptr;
    ID3D11Texture2D        *staging = nullptr;
    int w = 0, h = 0;
    RECT out_rect = {0, 0, 0, 0};

    /* Clean desktop image (no cursor) and the returned cursor-composited copy. */
    std::vector<uint8_t> bg;
    HDC     mdc = nullptr;
    HBITMAP dib = nullptr;
    HGDIOBJ old_bmp = nullptr;
    uint8_t *out_bits = nullptr;

    bool    have_frame = false;
    bool    dirty = false;   /* bg or cursor changed since the last compose */
    ULONGLONG open_tick = 0;
    POINT   cursor_pos = {0, 0};
    HCURSOR cursor = nullptr;
    bool    cursor_in_output = false;   /* hotspot may still touch the output */
    HCURSOR cursor_meta_handle = nullptr;
    POINT   cursor_hotspot = {0, 0};
    ULONGLONG cursor_meta_tick = 0;
    int     cursor_w = 0, cursor_h = 0;

    void close()
    {
        if (mdc && old_bmp) SelectObject(mdc, old_bmp);
        if (dib) DeleteObject(dib);
        if (mdc) DeleteDC(mdc);
        if (staging) { staging->Release(); staging = nullptr; }
        if (dup)     { dup->Release(); dup = nullptr; }
        if (output)  { output->Release(); output = nullptr; }
        if (adapter) { adapter->Release(); adapter = nullptr; }
        if (ctx)     { ctx->Release(); ctx = nullptr; }
        if (device)  { device->Release(); device = nullptr; }
        if (factory) { factory->Release(); factory = nullptr; }
        bg.clear();
        bg.shrink_to_fit();
        mdc = nullptr;
        dib = nullptr;
        old_bmp = nullptr;
        out_bits = nullptr;
        w = h = 0;
        out_rect = {0, 0, 0, 0};
        have_frame = false;
        dirty = false;
        open_tick = 0;
        cursor = nullptr;
        cursor_pos = {0, 0};
        cursor_in_output = false;
        cursor_meta_handle = nullptr;
        cursor_hotspot = {0, 0};
        cursor_meta_tick = 0;
        cursor_w = cursor_h = 0;
    }
};

DdaState g;

std::string hresultText(HRESULT hr)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%08lx", (unsigned long)hr);
    return buf;
}

bool sameDeviceName(const wchar_t *a, const char *b)
{
    wchar_t want[64] = L"";
    if (MultiByteToWideChar(CP_UTF8, 0, b, -1, want, 64) <= 0) return false;
    return wcscmp(a, want) == 0;
}

/* Creates a D3D11 device on the adapter that owns the output. A device on the
 * wrong adapter can never duplicate this output on a hybrid system. */
bool makeDevice(IDXGIAdapter1 *adapter, std::string *error)
{
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_9_1;
    HRESULT hr = D3D11CreateDevice(
        adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
        &g.device, &level, &g.ctx);
    if (FAILED(hr)) {
        if (error) *error = "D3D11CreateDevice " + hresultText(hr);
        g.device = nullptr;
        g.ctx = nullptr;
        return false;
    }
    return true;
}

/* Finds (and duplicates) the requested output. Tries every adapter/output
 * pair whose GDI name matches; the first that accepts duplication wins. */
bool openOutput(const char *device_name, std::string *error)
{
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                    (void **)&g.factory);
    if (FAILED(hr)) {
        if (error) *error = "CreateDXGIFactory1 " + hresultText(hr);
        return false;
    }

    std::string last_error = "no matching output";
    for (UINT ai = 0;; ai++) {
        IDXGIAdapter1 *adapter = nullptr;
        const HRESULT ahr = g.factory->EnumAdapters1(ai, &adapter);
        /* A driver reset / removed adapter can fail with anything else; a
         * nullptr must never be dereferenced. */
        if (ahr == DXGI_ERROR_NOT_FOUND || FAILED(ahr) || !adapter) break;
        for (UINT oi = 0;; oi++) {
            IDXGIOutput *output = nullptr;
            const HRESULT ohr = adapter->EnumOutputs(oi, &output);
            if (ohr == DXGI_ERROR_NOT_FOUND || FAILED(ohr) || !output) break;

            DXGI_OUTPUT_DESC desc = {};
            output->GetDesc(&desc);
            const bool match = device_name && device_name[0]
                                   ? sameDeviceName(desc.DeviceName, device_name)
                                   : (desc.DesktopCoordinates.left == 0 &&
                                      desc.DesktopCoordinates.top == 0);
            if (!match || !(desc.AttachedToDesktop)) {
                output->Release();
                continue;
            }

            IDXGIOutput1 *output1 = nullptr;
            if (FAILED(output->QueryInterface(__uuidof(IDXGIOutput1),
                                              (void **)&output1))) {
                output->Release();
                last_error = "IDXGIOutput1 unavailable";
                continue;
            }
            output->Release();

            if (!makeDevice(adapter, &last_error)) {
                output1->Release();
                continue;
            }
            hr = output1->DuplicateOutput(g.device, &g.dup);
            if (FAILED(hr)) {
                last_error = "DuplicateOutput " + hresultText(hr);
                if (g.ctx) { g.ctx->Release(); g.ctx = nullptr; }
                if (g.device) { g.device->Release(); g.device = nullptr; }
                output1->Release();
                continue;
            }

            DXGI_OUTDUPL_DESC dd = {};
            g.dup->GetDesc(&dd);
            const DXGI_FORMAT fmt = dd.ModeDesc.Format;
            if ((fmt != DXGI_FORMAT_B8G8R8A8_UNORM &&
                 fmt != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) ||
                dd.ModeDesc.Width == 0 || dd.ModeDesc.Height == 0) {
                last_error = "unsupported output format";
                g.dup->Release();
                g.dup = nullptr;
                if (g.ctx) { g.ctx->Release(); g.ctx = nullptr; }
                if (g.device) { g.device->Release(); g.device = nullptr; }
                output1->Release();
                continue;
            }

            /* EnumAdapters1 handed us one reference; transfer it. */
            g.adapter = adapter;
            g.output = output1;
            g.w = (int)dd.ModeDesc.Width;
            g.h = (int)dd.ModeDesc.Height;
            g.out_rect = desc.DesktopCoordinates;
            return true;
        }
        adapter->Release();
    }
    if (error) *error = last_error;
    return false;
}

/* Cursor-composited output surface (32-bit top-down DIB section). */
bool createSurface(std::string *error)
{
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = g.w;
    bi.bmiHeader.biHeight = -g.h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    HDC screen = GetDC(nullptr);
    if (!screen) {
        if (error) *error = "GetDC failed";
        return false;
    }
    g.mdc = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    if (!g.mdc) {
        if (error) *error = "CreateCompatibleDC failed";
        return false;
    }
    g.dib = CreateDIBSection(g.mdc, &bi, DIB_RGB_COLORS,
                             (void **)&g.out_bits, nullptr, 0);
    if (!g.dib || !g.out_bits) {
        if (error) *error = "CreateDIBSection failed";
        return false;
    }
    g.old_bmp = SelectObject(g.mdc, g.dib);
    if (!g.old_bmp || g.old_bmp == HGDI_ERROR) {
        g.old_bmp = nullptr;   /* never restore an invalid handle */
        if (error) *error = "SelectObject(DIB) failed";
        return false;
    }
    g.bg.assign((size_t)g.w * g.h * 4, 0);
    g.cursor_w = GetSystemMetrics(SM_CXCURSOR);
    g.cursor_h = GetSystemMetrics(SM_CYCURSOR);
    return true;
}

/* True when the cursor image can touch the captured output. The hotspot may
 * sit up to one cursor size outside while the image still overlaps, so use
 * that as the margin. */
bool cursorTouchesOutput(const CURSORINFO &ci)
{
    if (!(ci.flags & CURSOR_SHOWING)) return false;
    const int x = ci.ptScreenPos.x - (int)g.out_rect.left;
    const int y = ci.ptScreenPos.y - (int)g.out_rect.top;
    return x + g.cursor_w > 0 && x - g.cursor_w < g.w &&
           y + g.cursor_h > 0 && y - g.cursor_h < g.h;
}

bool updateCursor()
{
    CURSORINFO ci = {};
    ci.cbSize = sizeof(ci);
    if (!GetCursorInfo(&ci)) return false;

    const bool shown = (ci.flags & CURSOR_SHOWING) != 0;
    const bool in_output = shown && cursorTouchesOutput(ci);
    const HCURSOR handle = shown ? ci.hCursor : nullptr;
    const bool left_output = g.cursor_in_output && !in_output;
    const bool moved = handle != g.cursor ||
                       ci.ptScreenPos.x != g.cursor_pos.x ||
                       ci.ptScreenPos.y != g.cursor_pos.y;

    g.cursor = handle;
    g.cursor_pos = ci.ptScreenPos;
    g.cursor_in_output = in_output;

    if (left_output) {
        /* Erase the cursor from the cached background exactly once. */
        g.dirty = true;
        return true;
    }
    /* Moving on another monitor, or with the cursor hidden, must never
     * produce frames: the captured output is unchanged. */
    if (!in_output) return false;

    /* The hotspot only depends on the shape. A fast mouse changes position
     * hundreds of times per second; GetIconInfo allocates two bitmaps, so it
     * must not run for a pure move. Refresh on a shape change and once a
     * second, because Windows may reuse an HCURSOR value for a new shape. */
    const ULONGLONG now_tick = GetTickCount64();
    const bool refresh = handle != g.cursor_meta_handle ||
                         now_tick - g.cursor_meta_tick >= 1000;
    bool hotspot_changed = false;
    if (refresh) {
        g.cursor_meta_handle = handle;
        g.cursor_meta_tick = now_tick;
        POINT hs = {0, 0};
        if (handle) {
            ICONINFO ii = {};
            if (GetIconInfo(handle, &ii)) {
                hs = {(int)ii.xHotspot, (int)ii.yHotspot};
                if (ii.hbmMask) DeleteObject(ii.hbmMask);
                if (ii.hbmColor) DeleteObject(ii.hbmColor);
            }
        }
        hotspot_changed = (hs.x != g.cursor_hotspot.x ||
                           hs.y != g.cursor_hotspot.y);
        g.cursor_hotspot = hs;
    }
    if (!moved && !hotspot_changed) return false;
    g.dirty = true;
    return true;
}

void composeCursor()
{
    const size_t row = (size_t)g.w * 4;
    std::memcpy(g.out_bits, g.bg.data(), row * (size_t)g.h);
    if (g.cursor && g.cursor_in_output) {
        const int cx = g.cursor_pos.x - (int)g.out_rect.left - g.cursor_hotspot.x;
        const int cy = g.cursor_pos.y - (int)g.out_rect.top - g.cursor_hotspot.y;
        /* A hotspot just outside the output can still have its image overlap
         * it; GDI clips the rest. */
        if (cx < g.w && cy < g.h &&
            cx + g.cursor_w > 0 && cy + g.cursor_h > 0) {
            DrawIconEx(g.mdc, cx, cy, g.cursor,
                       g.cursor_w, g.cursor_h, 0, nullptr, DI_NORMAL);
        }
    }
    /* DrawIconEx is a GDI call; flush before the CPU reads the DIB bits. */
    GdiFlush();
    g.dirty = false;
}

/* Copies the acquired frame into the clean background buffer. */
bool copyFrame(IDXGIResource *res)
{
    ID3D11Texture2D *tex = nullptr;
    if (FAILED(res->QueryInterface(__uuidof(ID3D11Texture2D),
                                   (void **)&tex)))
        return false;

    D3D11_TEXTURE2D_DESC td = {};
    tex->GetDesc(&td);
    /* A rotated output can deliver a texture whose dimensions differ from
     * ModeDesc; the sender's capture rect would be wrong, so refuse and let
     * the caller fall back to GDI (which handles rotation). */
    if ((int)td.Width != g.w || (int)td.Height != g.h ||
        (td.Format != DXGI_FORMAT_B8G8R8A8_UNORM &&
         td.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)) {
        tex->Release();
        return false;
    }

    bool ok = true;
    if (!g.staging) {
        D3D11_TEXTURE2D_DESC sd = td;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        sd.MiscFlags = 0;
        sd.ArraySize = 1;
        sd.MipLevels = 1;
        sd.SampleDesc.Count = 1;
        sd.SampleDesc.Quality = 0;
        if (FAILED(g.device->CreateTexture2D(&sd, nullptr, &g.staging))) {
            tex->Release();
            return false;
        }
    }

    g.ctx->CopyResource(g.staging, tex);
    D3D11_MAPPED_SUBRESOURCE map = {};
    if (SUCCEEDED(g.ctx->Map(g.staging, 0, D3D11_MAP_READ, 0, &map))) {
        const size_t row = (size_t)g.w * 4;
        for (int y = 0; y < g.h; y++)
            std::memcpy(g.bg.data() + (size_t)y * row,
                        (const uint8_t *)map.pData + (size_t)y * map.RowPitch,
                        row);
        g.ctx->Unmap(g.staging, 0);
        g.have_frame = true;
        g.dirty = true;
    } else {
        ok = false;
    }
    tex->Release();
    return ok;
}

/* One GDI readback of the output, used only to seed the initial background.
 * On a static desktop the duplication session may deliver nothing but empty
 * frames (LastPresentTime == 0) until something changes, which used to leave
 * the session black until the watchdog fell back to GDI. BitBlt never
 * includes the cursor, so this is a clean background. Best effort: failure
 * leaves have_frame == false and the existing watchdog applies. */
bool seedFromGDI()
{
    HDC sdc = GetDC(nullptr);
    if (!sdc) return false;
    HDC mdc = CreateCompatibleDC(sdc);
    if (!mdc) { ReleaseDC(nullptr, sdc); return false; }

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = g.w;
    bi.bmiHeader.biHeight = -g.h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void *bits = nullptr;
    HBITMAP bmp = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool ok = false;
    if (bmp && bits) {
        HGDIOBJ old = SelectObject(mdc, bmp);
        if (old && old != HGDI_ERROR) {
            ok = BitBlt(mdc, 0, 0, g.w, g.h, sdc,
                        (int)g.out_rect.left, (int)g.out_rect.top,
                        SRCCOPY | CAPTUREBLT) != FALSE;
            if (ok) {
                GdiFlush();
                std::memcpy(g.bg.data(), bits, (size_t)g.w * g.h * 4);
            }
            SelectObject(mdc, old);
        }
        DeleteObject(bmp);
    }
    DeleteDC(mdc);
    ReleaseDC(nullptr, sdc);
    if (ok) {
        g.have_frame = true;
        g.dirty = true;
    }
    return ok;
}

int positiveMs(std::chrono::steady_clock::time_point deadline)
{
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                          deadline - std::chrono::steady_clock::now())
                          .count();
    return left < 0 ? 0 : (int)left;
}

} /* namespace */

bool ddaOpen(const char *device_name, std::string *error)
{
    ddaClose();
    std::string why;
    if (!openOutput(device_name, &why)) {
        if (error) *error = why;
        ddaClose();
        return false;
    }
    if (!createSurface(&why)) {
        if (error) *error = why;
        ddaClose();
        return false;
    }
    g.open_tick = GetTickCount64();
    /* Best effort: gives the session its first image even when the desktop
     * is static and DWM has nothing to present yet. */
    seedFromGDI();
    return true;
}

bool ddaIsOpen() { return g.dup != nullptr; }

int ddaWait(int timeout_ms)
{
    if (!g.dup) return -1;
    using clock = std::chrono::steady_clock;
    const auto deadline =
        clock::now() + std::chrono::milliseconds(timeout_ms < 0 ? 0 : timeout_ms);

    /* A new duplication session can start with an empty frame (LastPresentTime
     * == 0, black texture). If no real image ever arrives the caller must fall
     * back to GDI instead of presenting black forever. */
    if (!g.have_frame && GetTickCount64() - g.open_tick > 3000) return -1;

    for (;;) {
        /* A cursor-only change is a new frame too: the duplicated image never
         * contains the cursor, so without this the remote pointer would freeze
         * on a static desktop. Check it before the (possibly long) acquire.
         * Composition is deferred to ddaPixels(): a fast mouse can change the
         * cursor several times before the caller encodes a frame, and each
         * redundant compose is a full-frame copy. */
        if (updateCursor() && g.have_frame) return 1;

        const int left = positiveMs(deadline);
        if (left == 0 && timeout_ms <= 0) {
            /* Non-blocking poll: report pending desktop content if any. */
            DXGI_OUTDUPL_FRAME_INFO info = {};
            IDXGIResource *res = nullptr;
            const HRESULT hr = g.dup->AcquireNextFrame(0, &info, &res);
            if (hr == DXGI_ERROR_WAIT_TIMEOUT) return 0;
            if (FAILED(hr)) return -1;
            const bool has_image = info.LastPresentTime.QuadPart != 0;
            const bool ok = !has_image || copyFrame(res);
            res->Release();
            g.dup->ReleaseFrame();
            if (!ok) return -1;
            return has_image ? 1 : 0;
        }
        if (left == 0) return 0;

        DXGI_OUTDUPL_FRAME_INFO info = {};
        IDXGIResource *res = nullptr;
        const int slice = left < 8 ? left : 8;
        const HRESULT hr = g.dup->AcquireNextFrame((UINT)slice, &info, &res);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            if (positiveMs(deadline) == 0) return 0;
            continue;
        }
        if (FAILED(hr)) return -1;   /* ACCESS_LOST / mode change / fatal */

        /* Pointer-only update or the empty initial frame: no desktop image.
         * Never map it (the texture may be black), keep waiting. */
        if (info.LastPresentTime.QuadPart == 0) {
            res->Release();
            g.dup->ReleaseFrame();
            if (positiveMs(deadline) == 0) return 0;
            continue;
        }

        const bool ok = copyFrame(res);
        res->Release();
        g.dup->ReleaseFrame();
        if (!ok) return -1;
        return 1;
    }
}

const uint8_t *ddaPixels()
{
    if (!g.have_frame) return nullptr;
    if (g.dirty) composeCursor();
    return g.out_bits;
}

int ddaStride() { return g.w * 4; }
int ddaWidth() { return g.w; }
int ddaHeight() { return g.h; }

void ddaClose() { g.close(); }

#else   /* !_WIN32 – sender builds this TU on every platform */

bool ddaOpen(const char *, std::string *error)
{
    if (error) *error = "unsupported platform";
    return false;
}
bool ddaIsOpen() { return false; }
int ddaWait(int) { return -1; }
const uint8_t *ddaPixels() { return nullptr; }
int ddaStride() { return 0; }
int ddaWidth() { return 0; }
int ddaHeight() { return 0; }
void ddaClose() {}

#endif /* _WIN32 */
