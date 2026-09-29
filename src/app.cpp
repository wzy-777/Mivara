/**
 * APP.CPP – TwinView GUI LAUNCHER (SDL2 + SDL2_ttf)
 *
 * Screens:
 *   HOME       – choose 接收 / 发送
 *   RECEIVING  – "正在接收" + local IP / ports; spawns bin/receiver
 *   SEARCHING  – "搜索中" (SSDP discovery on a background thread)
 *   FOUND      – machine list, each row offers 投屏 (mirror) / 扩展 (extend)
 *   NOT_FOUND  – 继续搜索 / 手动输入ip / 退出
 *   MANUAL_IP  – type a receiver IP, then the same mode choice
 *   STREAMING  – session running (bin/sender <ip> <port> --mode ...)
 *
 * The launcher spawns sender/receiver as child processes. Stopping the
 * sender is always graceful (Ctrl+C / SIGINT) so vddShutdown() detaches
 * the virtual display – never TerminateProcess as the first resort.
 */
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "discover.h"
#include "vdd.h"

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <unistd.h>
#  include <sys/types.h>
#  include <sys/wait.h>
#  include <signal.h>
#  include <limits.h>
#endif

#define WIN_W 820
#define WIN_H 600

/* ── palette ─────────────────────────────────────────────────────────────── */
static const SDL_Color COL_BG      = {22, 22, 34, 255};
static const SDL_Color COL_PANEL   = {32, 34, 52, 255};
static const SDL_Color COL_TEXT    = {230, 232, 240, 255};
static const SDL_Color COL_DIM     = {150, 155, 175, 255};
static const SDL_Color COL_BLUE    = {46, 91, 171, 255};
static const SDL_Color COL_GREEN   = {34, 139, 84, 255};
static const SDL_Color COL_GRAY    = {70, 74, 92, 255};
static const SDL_Color COL_RED     = {160, 54, 60, 255};
static const SDL_Color COL_INPUT   = {18, 18, 28, 255};

/* ── screens ─────────────────────────────────────────────────────────────── */
enum Screen {
    SCR_HOME,
    SCR_RECEIVING,
    SCR_SEARCHING,
    SCR_FOUND,
    SCR_NOT_FOUND,
    SCR_MANUAL_IP,
    SCR_STREAMING,
};

struct Button {
    SDL_Rect rect;
    std::string label;
    int id;
    SDL_Color color;
};

struct ChildProc {
#ifdef _WIN32
    PROCESS_INFORMATION pi{};
    bool active = false;
#else
    pid_t pid = -1;
    bool active = false;
#endif
};

/* ── globals ─────────────────────────────────────────────────────────────── */
static SDL_Window   *g_win   = nullptr;
static SDL_Renderer *g_ren   = nullptr;
static TTF_Font     *g_font  = nullptr;   /* body / buttons */
static TTF_Font     *g_fontL = nullptr;   /* headings */

static Screen                g_screen = SCR_HOME;
static std::vector<Button>   g_buttons;
static std::string           g_manual_ip;
static std::string           g_status_note;      /* extra line on status screens */
static std::string           g_child_label;      /* "投屏" / "扩展" / "接收" */
static bool                  g_child_extend = false;  /* sender in extend mode? */

static std::vector<DiscoveredDevice> g_devices;
static std::mutex                  g_dev_mtx;
static std::atomic<bool>           g_searching{false};
static std::atomic<bool>           g_search_done{false};
static std::atomic<int>            g_found_count{0};

static ChildProc g_child;

/* ── paths & fonts ───────────────────────────────────────────────────────── */
static std::string exeDir()
{
#ifdef _WIN32
    char buf[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string p(buf);
    size_t s = p.find_last_of("\\/");
    return (s == std::string::npos) ? std::string(".") : p.substr(0, s);
#else
    char buf[PATH_MAX] = {};
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = 0;
        std::string p(buf);
        size_t s = p.find_last_of('/');
        return (s == std::string::npos) ? std::string(".") : p.substr(0, s);
    }
    return ".";
#endif
}

static TTF_Font *openFont(int size)
{
    static const char *candidates[] = {
        "C:/Windows/Fonts/msyh.ttc",       /* Microsoft YaHei       */
        "C:/Windows/Fonts/msyhbd.ttc",
        "C:/Windows/Fonts/simhei.ttf",     /* SimHei                */
        "C:/Windows/Fonts/simsun.ttc",     /* SimSun                */
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/opentype/noto/NotoSansCJK.ttc",
        "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
        "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/System/Library/Fonts/PingFang.ttc",
        "/System/Library/Fonts/STHeiti Light.ttc",
        nullptr
    };
    for (int i = 0; candidates[i]; i++) {
        TTF_Font *f = TTF_OpenFont(candidates[i], size);
        if (f) return f;
    }
    return nullptr;
}

/* ── text drawing ────────────────────────────────────────────────────────── */
static void drawText(const std::string &s, int x, int y, SDL_Color c,
                     TTF_Font *font, bool center = false)
{
    if (!font || s.empty()) return;
    SDL_Surface *surf = TTF_RenderUTF8_Blended(font, s.c_str(), c);
    if (!surf) return;
    SDL_Texture *tex = SDL_CreateTextureFromSurface(g_ren, surf);
    SDL_Rect dst{x, y, surf->w, surf->h};
    if (center) dst.x = x - surf->w / 2;
    SDL_FreeSurface(surf);
    if (tex) {
        SDL_RenderCopy(g_ren, tex, nullptr, &dst);
        SDL_DestroyTexture(tex);
    }
}

static void drawButton(const Button &b, int mousex, int mousey)
{
    bool hover = (mousex >= b.rect.x && mousex < b.rect.x + b.rect.w &&
                  mousey >= b.rect.y && mousey < b.rect.y + b.rect.h);
    SDL_Color c = b.color;
    if (hover) {
        c.r = (Uint8)std::min(255, c.r + 22);
        c.g = (Uint8)std::min(255, c.g + 22);
        c.b = (Uint8)std::min(255, c.b + 22);
    }
    SDL_SetRenderDrawColor(g_ren, c.r, c.g, c.b, 255);
    SDL_RenderFillRect(g_ren, &b.rect);
    SDL_SetRenderDrawColor(g_ren, 12, 12, 20, 255);
    SDL_RenderDrawRect(g_ren, &b.rect);

    int tw = 0, th = 0;
    if (g_font) TTF_SizeUTF8(g_font, b.label.c_str(), &tw, &th);
    drawText(b.label, b.rect.x + b.rect.w / 2, b.rect.y + (b.rect.h - th) / 2,
             COL_TEXT, g_font, true);
}

static void addButton(int x, int y, int w, int h,
                      const std::string &label, int id, SDL_Color color)
{
    g_buttons.push_back({{x, y, w, h}, label, id, color});
}

/* ── child process helpers ───────────────────────────────────────────────── */
static std::string sibling(const char *name)
{
    return exeDir() + "/" + name;
}

static bool spawnChild(const std::string &path, const std::vector<std::string> &args)
{
    if (g_child.active) return false;
#ifdef _WIN32
    std::string cmd = "\"" + path + "\"";
    for (auto &a : args) cmd += " " + a;
    std::vector<char> buf(cmd.begin(), cmd.end());
    buf.push_back('\0');

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    ZeroMemory(&g_child.pi, sizeof(g_child.pi));
    /* Sender is a console app: give it its own console so logs are visible
     * and the graceful Ctrl+C stop path (AttachConsole) can reach it. */
    DWORD flags = CREATE_NEW_CONSOLE;
    if (!CreateProcessA(path.c_str(), buf.data(), nullptr, nullptr, FALSE,
                        flags, nullptr, nullptr, &si, &g_child.pi)) {
        return false;
    }
    g_child.active = true;
    return true;
#else
    pid_t pid = fork();
    if (pid == 0) {
        std::vector<char *> argv;
        std::vector<std::string> store;
        store.push_back(path);
        for (auto &a : args) store.push_back(a);
        for (auto &s : store) argv.push_back(const_cast<char *>(s.c_str()));
        argv.push_back(nullptr);
        execv(path.c_str(), argv.data());
        _exit(127);
    }
    if (pid < 0) return false;
    g_child.pid = pid;
    g_child.active = true;
    return true;
#endif
}

/* Graceful stop: Ctrl+C (Windows) / SIGINT (POSIX) so the sender runs its
 * cleanup path including vddShutdown(). Falls back to hard kill after 8 s
 * (vddShutdown + SetDisplayConfig can take a few seconds), then force-
 * detaches the virtual display so it can never linger after a stop. */
static void stopChild()
{
    if (!g_child.active) return;
#ifdef _WIN32
    FreeConsole();
    SetConsoleCtrlHandler(nullptr, TRUE);          /* GUI ignores Ctrl+C */
    bool hard_kill = false;
    if (AttachConsole(g_child.pi.dwProcessId)) {
        GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0);
        if (WaitForSingleObject(g_child.pi.hProcess, 8000) == WAIT_TIMEOUT) {
            TerminateProcess(g_child.pi.hProcess, 1);
            hard_kill = true;
        }
        FreeConsole();
    } else {
        TerminateProcess(g_child.pi.hProcess, 1);
        hard_kill = true;
    }
    CloseHandle(g_child.pi.hProcess);
    CloseHandle(g_child.pi.hThread);
    g_child.active = false;
    /* Safety net: a hard-killed extend sender never reached its own
     * vddShutdown() – detach the virtual display from here instead. */
    if (hard_kill && g_child_extend) {
        std::cout << "强制停止：正在移除虚拟显示器...\n";
        if (vddForceDetach())
            std::cout << "虚拟显示器已移除\n";
        else
            std::cout << "虚拟显示器移除失败（可能本就未激活）\n";
    }
    g_child_extend = false;
#else
    kill(g_child.pid, SIGINT);
    for (int i = 0; i < 30; i++) {
        int st = 0;
        if (waitpid(g_child.pid, &st, WNOHANG) == g_child.pid) {
            g_child.active = false;
            g_child_extend = false;
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    kill(g_child.pid, SIGKILL);
    waitpid(g_child.pid, nullptr, 0);
    g_child.active = false;
    g_child_extend = false;
#endif
}

static bool childExited()
{
    if (!g_child.active) return true;
#ifdef _WIN32
    if (WaitForSingleObject(g_child.pi.hProcess, 0) == WAIT_OBJECT_0) {
        CloseHandle(g_child.pi.hProcess);
        CloseHandle(g_child.pi.hThread);
        g_child.active = false;
        return true;
    }
    return false;
#else
    int st = 0;
    if (waitpid(g_child.pid, &st, WNOHANG) == g_child.pid) {
        g_child.active = false;
        return true;
    }
    return false;
#endif
}

static bool binaryExists(const std::string &p)
{
    FILE *f = fopen(p.c_str(), "rb");
    if (f) { fclose(f); return true; }
    return false;
}

/* ── actions ─────────────────────────────────────────────────────────────── */
static void startSearch()
{
    if (g_searching.exchange(true)) return;      /* already running */
    g_search_done = false;
    std::thread([] {
        auto found = discoverReceivers(5);
        {
            std::lock_guard<std::mutex> lk(g_dev_mtx);
            g_devices = found;
        }
        g_found_count = (int)found.size();
        g_searching = false;
        g_search_done = true;
    }).detach();
    g_screen = SCR_SEARCHING;
}

static void launchStream(const std::string &ip, int port, bool extend)
{
    std::string path = sibling("sender.exe");
#ifndef _WIN32
    path = sibling("sender");
#endif
    if (!binaryExists(path)) {
        g_status_note = "找不到 sender 程序，请先运行 make";
        g_screen = SCR_NOT_FOUND;
        return;
    }
    std::vector<std::string> args = {
        ip, std::to_string(port),
        "--mode", extend ? "extend" : "mirror"
    };
    if (!spawnChild(path, args)) {
        g_status_note = "启动 sender 失败";
        g_screen = SCR_NOT_FOUND;
        return;
    }
    g_child_extend = extend;
    g_child_label = extend ? "扩展" : "投屏";
    g_status_note = "目标: " + ip + ":" + std::to_string(port);
    g_screen = SCR_STREAMING;
}

static void launchReceiver()
{
    std::string path = sibling("receiver.exe");
#ifndef _WIN32
    path = sibling("receiver");
#endif
    if (!binaryExists(path)) {
        g_status_note = "找不到 receiver 程序，请先运行 make";
        g_screen = SCR_HOME;
        return;
    }
    if (!spawnChild(path, {})) {
        g_status_note = "启动 receiver 失败";
        g_screen = SCR_HOME;
        return;
    }
    g_child_extend = false;
    g_child_label = "接收";
    g_status_note.clear();
    g_screen = SCR_RECEIVING;
}

/* ── screen building ─────────────────────────────────────────────────────── */
static void buildButtons()
{
    g_buttons.clear();
    const int cx = WIN_W / 2;

    switch (g_screen) {
    case SCR_HOME:
        addButton(cx - 300, 240, 280, 84, "接 收", 100, COL_GREEN);
        addButton(cx + 20,  240, 280, 84, "发 送", 101, COL_BLUE);
        addButton(cx - 90,  380, 180, 52, "退 出", 102, COL_GRAY);
        break;

    case SCR_RECEIVING:
        addButton(cx - 90, 470, 180, 52, "停止接收", 200, COL_RED);
        break;

    case SCR_SEARCHING:
        addButton(cx - 90, 470, 180, 52, "取 消", 300, COL_GRAY);
        break;

    case SCR_FOUND: {
        size_t n;
        {
            std::lock_guard<std::mutex> lk(g_dev_mtx);
            n = g_devices.size();
        }
        int y = 150;
        for (size_t i = 0; i < n && i < 6; i++, y += 56) {
            addButton(430, y + 4, 110, 44, "投屏", (int)i * 10 + 1, COL_GREEN);
            addButton(555, y + 4, 110, 44, "扩展", (int)i * 10 + 2, COL_BLUE);
        }
        addButton(280, 510, 160, 48, "重新搜索", 400, COL_GRAY);
        addButton(460, 510, 160, 48, "手动输入ip", 401, COL_GRAY);
        break;
    }

    case SCR_NOT_FOUND:
        addButton(cx - 320, 300, 200, 56, "继续搜索", 500, COL_GREEN);
        addButton(cx - 100, 300, 200, 56, "手动输入ip", 501, COL_BLUE);
        addButton(cx + 120, 300, 200, 56, "退 出", 502, COL_RED);
        break;

    case SCR_MANUAL_IP:
        addButton(cx - 180, 330, 160, 52, "连 接", 600, COL_GREEN);
        addButton(cx + 20,  330, 160, 52, "返 回", 601, COL_GRAY);
        break;

    case SCR_STREAMING:
        addButton(cx - 90, 470, 180, 52, "停 止", 700, COL_RED);
        break;
    }
}

static void renderScreen(int mousex, int mousey)
{
    SDL_SetRenderDrawColor(g_ren, COL_BG.r, COL_BG.g, COL_BG.b, 255);
    SDL_RenderClear(g_ren);

    const int cx = WIN_W / 2;

    /* header */
    SDL_Rect hdr{0, 0, WIN_W, 64};
    SDL_SetRenderDrawColor(g_ren, COL_PANEL.r, COL_PANEL.g, COL_PANEL.b, 255);
    SDL_RenderFillRect(g_ren, &hdr);
    drawText("TwinView  幻屏", cx, 18, COL_TEXT, g_fontL, true);

    switch (g_screen) {
    case SCR_HOME:
        drawText("零配置局域网投屏 / 无线扩展屏", cx, 120, COL_DIM, g_font, true);
        break;

    case SCR_RECEIVING: {
        drawText("正在接收", cx, 110, COL_GREEN, g_fontL, true);
        std::string ip = getLocalIPAddress();
        drawText("本机 IP：  " + ip, cx, 190, COL_TEXT, g_font, true);
        drawText("视频端口：8081      计算端口：8082      端口服务：8083",
                 cx, 236, COL_DIM, g_font, true);
        drawText("发现服务：UDP 1900", cx, 274, COL_DIM, g_font, true);
        drawText("状态：等待发送端连接…", cx, 330, COL_TEXT, g_font, true);
        drawText("发送端可运行  sender " + ip, cx, 376, COL_DIM, g_font, true);
        break;
    }

    case SCR_SEARCHING: {
        int dots = ((int)(SDL_GetTicks() / 400)) % 4;
        drawText("搜索中" + std::string(dots, '.'), cx, 220, COL_TEXT, g_fontL, true);
        drawText("正在通过 SSDP 查找局域网内的接收端", cx, 290, COL_DIM, g_font, true);
        break;
    }

    case SCR_FOUND: {
        std::vector<DiscoveredDevice> devs;
        {
            std::lock_guard<std::mutex> lk(g_dev_mtx);
            devs = g_devices;
        }
        drawText("发现 " + std::to_string(devs.size()) + " 台设备",
                 cx, 88, COL_TEXT, g_fontL, true);
        int y = 150;
        for (size_t i = 0; i < devs.size() && i < 6; i++, y += 56) {
            SDL_Rect row{70, y, 680, 52};
            SDL_SetRenderDrawColor(g_ren, COL_PANEL.r, COL_PANEL.g, COL_PANEL.b, 255);
            SDL_RenderFillRect(g_ren, &row);
            drawText(devs[i].toString(), 95, y + 15, COL_TEXT, g_font);
        }
        if (devs.size() > 6)
            drawText("仅显示前 6 台", cx, y + 8, COL_DIM, g_font, true);
        break;
    }

    case SCR_NOT_FOUND:
        drawText("未找到可用设备", cx, 180, COL_TEXT, g_fontL, true);
        drawText("请确认接收端已启动，且两台机器在同一局域网",
                 cx, 240, COL_DIM, g_font, true);
        if (!g_status_note.empty())
            drawText(g_status_note, cx, 410, COL_RED, g_font, true);
        break;

    case SCR_MANUAL_IP:
        drawText("手动输入接收端 IP", cx, 140, COL_TEXT, g_fontL, true);
        {
            SDL_Rect box{cx - 200, 220, 400, 60};
            SDL_SetRenderDrawColor(g_ren, COL_INPUT.r, COL_INPUT.g, COL_INPUT.b, 255);
            SDL_RenderFillRect(g_ren, &box);
            SDL_SetRenderDrawColor(g_ren, COL_BLUE.r, COL_BLUE.g, COL_BLUE.b, 255);
            SDL_RenderDrawRect(g_ren, &box);
            std::string shown = g_manual_ip.empty() ? "例如 192.168.1.105" : g_manual_ip;
            SDL_Color c = g_manual_ip.empty() ? COL_DIM : COL_TEXT;
            drawText(shown, cx, 238, c, g_font, true);
        }
        if (!g_status_note.empty())
            drawText(g_status_note, cx, 420, COL_RED, g_font, true);
        break;

    case SCR_STREAMING:
        drawText("正在" + g_child_label, cx, 130, COL_GREEN, g_fontL, true);
        drawText(g_status_note, cx, 210, COL_TEXT, g_font, true);
        drawText("推流中… 关闭请点下方按钮", cx, 260, COL_DIM, g_font, true);
        break;
    }

    for (auto &b : g_buttons) drawButton(b, mousex, mousey);
    SDL_RenderPresent(g_ren);
}

/* ── button actions ──────────────────────────────────────────────────────── */
static void onButton(int id)
{
    switch (id) {
    case 100:                                   /* 接收 */
        launchReceiver();
        break;
    case 101:                                   /* 发送 */
        g_status_note.clear();
        startSearch();
        break;
    case 102: {                                 /* 退出 */
        SDL_Event e; e.type = SDL_QUIT; SDL_PushEvent(&e);
        break;
    }

    case 200:                                   /* 停止接收 */
        stopChild();
        g_screen = SCR_HOME;
        break;

    case 300:                                   /* 取消搜索 */
        g_searching = false;
        g_screen = SCR_HOME;
        break;

    case 400:                                   /* 重新搜索 */
        g_status_note.clear();
        startSearch();
        break;
    case 401:                                   /* 手动输入ip */
        g_manual_ip.clear();
        g_status_note.clear();
        SDL_StartTextInput();
        g_screen = SCR_MANUAL_IP;
        break;

    case 500:                                   /* 继续搜索 */
        g_status_note.clear();
        startSearch();
        break;
    case 501:                                   /* 手动输入ip */
        g_manual_ip.clear();
        g_status_note.clear();
        SDL_StartTextInput();
        g_screen = SCR_MANUAL_IP;
        break;
    case 502: {                                 /* 退出 */
        SDL_Event e2; e2.type = SDL_QUIT; SDL_PushEvent(&e2);
        break;
    }

    case 600: {                                 /* 连接 (manual IP) */
        std::string ip = g_manual_ip;
        if (ip.empty()) { g_status_note = "请输入 IP 地址"; break; }
        if (!testTcpConnection(ip, 8081, 1500)) {
            g_status_note = "无法连接 " + ip + ":8081";
            break;
        }
        /* reuse the found-list screen with this single device */
        {
            std::lock_guard<std::mutex> lk(g_dev_mtx);
            g_devices.clear();
            g_devices.emplace_back(ip, 8081);
        }
        SDL_StopTextInput();
        g_status_note.clear();
        g_screen = SCR_FOUND;
        break;
    }
    case 601:                                   /* 返回 */
        SDL_StopTextInput();
        g_status_note.clear();
        g_screen = SCR_HOME;
        break;

    case 700:                                   /* 停止推流 */
        stopChild();
        g_screen = SCR_HOME;
        break;

    default:
        /* device row buttons: id = index*10 + 1(投屏) / +2(扩展) */
        if (id >= 1 && id <= 99) {
            int idx = id / 10;
            int kind = id % 10;
            std::vector<DiscoveredDevice> devs;
            {
                std::lock_guard<std::mutex> lk(g_dev_mtx);
                devs = g_devices;
            }
            if (idx >= 0 && idx < (int)devs.size())
                launchStream(devs[idx].ip_address, devs[idx].tcp_port,
                             kind == 2);
        }
        break;
    }
}

/* ── main ────────────────────────────────────────────────────────────────── */
int main(int argc, char *argv[])
{
    (void)argc; (void)argv;
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        std::cerr << "SDL_Init: " << SDL_GetError() << "\n";
        return 1;
    }
    if (TTF_Init() < 0) {
        std::cerr << "TTF_Init: " << TTF_GetError() << "\n";
        SDL_Quit();
        return 1;
    }
    g_font  = openFont(22);
    g_fontL = openFont(34);
    if (!g_font || !g_fontL) {
        std::cerr << "找不到中文字体（msyh/simhei/NotoSansCJK）\n";
        TTF_Quit(); SDL_Quit();
        return 1;
    }

    g_win = SDL_CreateWindow("TwinView", SDL_WINDOWPOS_CENTERED,
                             SDL_WINDOWPOS_CENTERED, WIN_W, WIN_H, 0);
    g_ren = SDL_CreateRenderer(g_win, -1,
                               SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!g_win || !g_ren) {
        std::cerr << "SDL_CreateWindow/Renderer: " << SDL_GetError() << "\n";
        return 1;
    }

    initSockets();

    bool running = true;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) running = false;
            else if (ev.type == SDL_MOUSEBUTTONDOWN &&
                     ev.button.button == SDL_BUTTON_LEFT) {
                for (auto &b : g_buttons) {
                    SDL_Point p{ev.button.x, ev.button.y};
                    if (SDL_PointInRect(&p, &b.rect)) { onButton(b.id); break; }
                }
            }
            else if (ev.type == SDL_TEXTINPUT) {
                if (g_screen == SCR_MANUAL_IP) {
                    for (const char *c = ev.text.text; *c; c++) {
                        if ((isdigit((unsigned char)*c) || *c == '.') &&
                            g_manual_ip.size() < 15)
                            g_manual_ip += *c;
                    }
                }
            }
            else if (ev.type == SDL_KEYDOWN) {
                if (ev.key.keysym.sym == SDLK_ESCAPE) {
                    if (g_screen == SCR_MANUAL_IP) {
                        SDL_StopTextInput();
                        g_screen = SCR_HOME;
                    } else if (g_screen != SCR_HOME) {
                        stopChild();
                        g_screen = SCR_HOME;
                    } else running = false;
                }
                else if (g_screen == SCR_MANUAL_IP &&
                         ev.key.keysym.sym == SDLK_BACKSPACE) {
                    if (!g_manual_ip.empty()) g_manual_ip.pop_back();
                }
                else if (g_screen == SCR_MANUAL_IP &&
                         ev.key.keysym.sym == SDLK_RETURN) {
                    onButton(600);
                }
            }
        }

        /* async search completion */
        if (g_search_done.exchange(false))
            g_screen = (g_found_count > 0) ? SCR_FOUND : SCR_NOT_FOUND;

        /* child exit tracking */
        if (g_child.active && childExited()) {
            if (g_screen == SCR_RECEIVING || g_screen == SCR_STREAMING) {
                g_status_note = "会话已结束";
                g_screen = SCR_HOME;
            }
        }

        int mx, my;
        SDL_GetMouseState(&mx, &my);
        buildButtons();
        renderScreen(mx, my);
    }

    stopChild();
    cleanupSockets();
    if (g_font)  TTF_CloseFont(g_font);
    if (g_fontL) TTF_CloseFont(g_fontL);
    TTF_Quit();
    SDL_DestroyRenderer(g_ren);
    SDL_DestroyWindow(g_win);
    SDL_Quit();
    return 0;
}
