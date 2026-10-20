/**
 * THEME.CPP - light / dark / follow-system palette resolution for the GUI.
 *
 * Two hand-tuned palettes, one live Theme instance, and a platform hook for the
 * OS preference. See theme.h for provenance and the contrast budget.
 *
 * Polling policy: only Windows exposes a cheap "the user flipped the OS theme"
 * signal (one registry DWORD), so only Windows is polled from themeTick(). On
 * macOS/Linux the preference is read once — via `defaults`/`gsettings` — and
 * changing the OS theme takes effect on the next launch rather than spawning a
 * helper process every frame.
 */
#include "theme.h"

#include <cstdio>
#include <cstring>
#include <string>

/* ── palettes ─────────────────────────────────────────────────────────────
 * Contrast ratios below are text-on-its-own-background, measured against the
 * WCAG 2.1 relative-luminance formula. AA needs 4.5:1 for body text, 3:1 for
 * large text and for the edges that delimit interactive controls.
 */
static SDL_Color rgb(unsigned hex)
{
    return SDL_Color{(Uint8)(hex >> 16), (Uint8)(hex >> 8), (Uint8)hex, 255};
}

static SDL_Color rgba(unsigned hex, Uint8 a)
{
    SDL_Color c = rgb(hex);
    c.a = a;
    return c;
}

static Theme lightPalette()
{
    Theme t;
    t.bg         = rgb(0xF2F2F7);   /* page                                     */
    t.surface    = rgb(0xFFFFFF);   /* card                                     */
    t.surfaceAlt = rgb(0xECECF1);   /* inset fill: segments, value boxes         */
    t.hover      = rgb(0xE6E6EC);
    t.press      = rgb(0xDCDCE4);
    t.primary    = rgb(0x2F6FEC);   /* white on it        4.72:1  AA            */
    t.primaryHover = rgb(0x1F5CD8);
    t.onPrimary  = rgb(0xFFFFFF);
    t.accent     = rgb(0x2F6FEC);   /* on surface         4.62:1  AA            */
    t.accentSoft = rgba(0x2F6FEC, 0x26);
    t.success    = rgb(0x0F7A40);   /* on surface         5.17:1  AA            */
    t.successSoft = rgba(0x0F7A40, 0x26);
    /* Red used as FOREGROUND (the 删除/清空 links). Darker than DANGER_SOLID,
     * which is a filled button's background: the link also gets drawn on the
     * row hover fill, where #C62A3A falls to 4.45:1. */
    t.danger     = rgb(0xB32430);   /* on hover fill      5.26:1  AA            */
    t.dangerSoft = rgba(0xB32430, 0x26);
    t.warning    = rgb(0x9A5200);   /* on surface         5.45:1  AA            */
    t.text       = rgb(0x2F2F30);   /* on surface        13.33:1  AAA           */
    /* textDim is used on surfaceAlt too (the idle status chip) — at #6E6E73
     * that pair is only 4.31:1, so it is darkened until it clears AA on the
     * darker of the two backgrounds it is actually drawn on. */
    t.textDim    = rgb(0x68686D);   /* on surfaceAlt      4.71:1  AA            */
    t.inputBg    = rgb(0xF2F2F7);
    /* Border is the sole edge of controls that sit directly on the page (the
     * manual-IP field, the secondary buttons), so it has to clear 3:1 against
     * bg (#f2f2f7), not just against surface — at #8E8E93 that pair is 2.92:1. */
    t.border     = rgb(0x88888D);   /* on bg              3.16:1  AA (UI edge)  */
    t.shadow     = rgba(0x5C618F, 0x33);
    /* opaque tint chips: fill pre-composited against the white surface */
    t.primaryTint = rgb(0xE8EFFE);  t.onPrimaryTint = rgb(0x1C4FBF); /* 6.29:1 */
    t.successTint = rgb(0xE8F7EE);  t.onSuccessTint = rgb(0x0F7A40); /* 4.86:1 */
    t.dangerTint  = rgb(0xFDECEC);  t.onDangerTint  = rgb(0xC62A3A); /* 5.24:1 */
    t.warningTint = rgb(0xFFF4DD);  t.onWarningTint = rgb(0x8A4B00); /* 5.51:1 */
    t.radius = 9;
    t.radiusCard = 16;
    return t;
}

static Theme darkPalette()
{
    Theme t;
    t.bg         = rgb(0x09090A);
    t.surface    = rgb(0x121213);
    t.surfaceAlt = rgb(0x1C1C1E);
    t.hover      = rgb(0x232326);
    t.press      = rgb(0x2A2A2E);
    t.primary    = rgb(0x2F6FEC);   /* white on it        4.72:1  AA            */
    t.primaryHover = rgb(0x2662E0);
    t.onPrimary  = rgb(0xFFFFFF);
    t.accent     = rgb(0x2F6FEC);
    t.accentSoft = rgba(0x2F6FEC, 0x33);
    t.success    = rgb(0x3DDC84);   /* on surface         9.88:1  AAA           */
    t.successSoft = rgba(0x3DDC84, 0x26);
    t.danger     = rgb(0xFF5A5F);   /* on surface         5.74:1  AA            */
    t.dangerSoft = rgba(0xFF5A5F, 0x26);
    t.warning    = rgb(0xFFB200);   /* on surface        10.18:1  AAA           */
    t.text       = rgb(0xF2F2F7);   /* on surface        17.11:1  AAA           */
    t.textDim    = rgb(0x9A9AA2);   /* on surface         7.11:1  AA            */
    t.inputBg    = rgb(0x0E0E0F);
    t.border     = rgb(0x6A6A72);   /* on surface         3.63:1  AA (UI edge)  */
    t.shadow     = rgba(0x000000, 0x59);
    t.primaryTint = rgb(0x16233D);  t.onPrimaryTint = rgb(0x7BA7FF); /* 6.07:1 */
    t.successTint = rgb(0x12321F);  t.onSuccessTint = rgb(0x3DDC84); /* 7.33:1 */
    t.dangerTint  = rgb(0x3A1416);  t.onDangerTint  = rgb(0xFF5A5F); /* 5.63:1 */
    t.warningTint = rgb(0x33270A);  t.onWarningTint = rgb(0xFFB200); /* 8.20:1 */
    t.radius = 9;
    t.radiusCard = 16;
    return t;
}

/* ── state ──────────────────────────────────────────────────────────────── */
static Theme      s_theme;
static ThemeMode  s_mode      = ThemeMode::System;
static bool       s_sys_dark  = false;
static bool       s_resolved  = false;
static Uint32     s_last_poll = 0;

/* ── OS preference ──────────────────────────────────────────────────────── */
#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>

bool themeSystemDark()
{
    /* AppsUseLightTheme: 0 = dark apps, 1 = light apps. A missing key/value
     * means "never customised" → light, which is also the safe fallback. */
    HKEY k = nullptr;
    if (RegOpenKeyExA(HKEY_CURRENT_USER,
                      "Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                      0, KEY_READ, &k) != ERROR_SUCCESS)
        return false;
    DWORD val = 1, sz = sizeof(val), type = 0;
    bool dark = false;
    if (RegQueryValueExA(k, "AppsUseLightTheme", nullptr, &type,
                         reinterpret_cast<LPBYTE>(&val), &sz) == ERROR_SUCCESS &&
        type == REG_DWORD)
        dark = (val == 0);
    RegCloseKey(k);
    return dark;
}

void themeTick(Uint32 now_ms)
{
    if (s_mode != ThemeMode::System) return;
    /* The registry read is cheap but not free — 2 s is far below the time it
     * takes a human to flip the setting and notice. */
    if (s_last_poll != 0 && (Uint32)(now_ms - s_last_poll) < 2000) return;
    s_last_poll = now_ms;
    const bool dark = themeSystemDark();
    if (dark == s_sys_dark && s_resolved) return;
    s_sys_dark = dark;
    s_resolved = true;
    s_theme = dark ? darkPalette() : lightPalette();
}

#else /* ── macOS / Linux ─────────────────────────────────────────────────── */

/* One-shot shell query. Failure is not an error: an unreadable preference just
 * means we stay on light. */
static bool queryCmd(const char *cmd, const char *needle)
{
    FILE *p = popen(cmd, "r");
    if (!p) return false;
    char buf[256] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, p);
    pclose(p);
    buf[n] = 0;
    return n > 0 && std::strstr(buf, needle) != nullptr;
}

bool themeSystemDark()
{
    /* fork+exec on the GUI thread: resolve() calls this from themeSetMode(),
     * which main() runs before the window exists and the appearance switcher
     * runs on every click. A slow or D-Bus-blocked gsettings would freeze the
     * UI, so the sample is taken exactly once per process — which is also what
     * theme.h documents for these platforms. */
    static int cached = -1;             /* -1 unknown, 0 light, 1 dark */
    if (cached < 0) {
#  ifdef __APPLE__
        cached = queryCmd("defaults read -g AppleInterfaceStyle 2>/dev/null", "Dark") ? 1 : 0;
#  else
        /* GNOME/GTK; other desktops simply report light. */
        cached = queryCmd("gsettings get org.gnome.desktop.interface color-scheme 2>/dev/null",
                          "dark") ? 1 : 0;
#  endif
    }
    return cached == 1;
}

void themeTick(Uint32 now_ms)
{
    /* No cheap change signal outside Windows: themeSystemDark() samples once
     * per process, so there is nothing to poll. */
    (void)now_ms;
}

#endif

/* ── public API ─────────────────────────────────────────────────────────── */
static void resolve()
{
    /* Always re-sample in System mode so the value themeTick() compares
     * against is the current one, and so a stale sample can never be reused
     * after the user switches back to System. */
    if (s_mode == ThemeMode::System) s_sys_dark = themeSystemDark();
    const bool dark = (s_mode == ThemeMode::Dark) ||
                      (s_mode == ThemeMode::System && s_sys_dark);
    s_theme = dark ? darkPalette() : lightPalette();
    s_resolved = true;
}

Theme &theme()
{
    if (!s_resolved) resolve();
    return s_theme;
}

ThemeMode themeMode()
{
    return s_mode;
}

void themeSetMode(ThemeMode mode)
{
    s_mode = mode;
    resolve();
}

const char *themeModeKey(ThemeMode mode)
{
    switch (mode) {
    case ThemeMode::Light: return "light";
    case ThemeMode::Dark:  return "dark";
    case ThemeMode::System:
    default:               return "system";
    }
}

bool themeModeFromKey(const char *key, ThemeMode &out)
{
    if (!key) return false;
    if (std::strcmp(key, "light") == 0)  { out = ThemeMode::Light;  return true; }
    if (std::strcmp(key, "dark") == 0)   { out = ThemeMode::Dark;   return true; }
    if (std::strcmp(key, "system") == 0) { out = ThemeMode::System; return true; }
    return false;
}
