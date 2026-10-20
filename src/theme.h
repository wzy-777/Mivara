/**
 * THEME.H - Mivara GUI colour system (light / dark / follow-system)
 *
 * The GUI is a hand-rolled SDL2 immediate-mode renderer, so there is no CSS:
 * every widget reads its colours from the single Theme instance returned by
 * theme(). Nothing in app.cpp may hardcode a colour.
 *
 * Palette provenance: the tokens are derived from NetEase's UU远程 product
 * palette (uuyc.163.com) — light page #f2f2f7, near-black ink #2f2f30, a single
 * confident blue, generous rounding, quiet borders. Contrast was then tuned so
 * every text/background pair used by the UI clears WCAG AA (4.5:1 for body
 * text, 3:1 for large text and UI edges) in BOTH modes; see the ratios recorded
 * next to each palette below.
 *
 * "Follow system" reads the OS light/dark preference. Only Windows offers a
 * cheap change signal (a registry DWORD), so that is the only platform polled
 * at runtime; macOS/Linux resolve once at startup and never block the UI if
 * detection fails (failure ⇒ light).
 */
#ifndef MIVARA_THEME_H
#define MIVARA_THEME_H

#include <SDL2/SDL.h>

/**
 * Semantic colour tokens. This is a design system, not a per-screen shopping
 * list: a few tokens are currently unused (inputBg, successSoft, dangerSoft,
 * the danger/warning tints) and are kept because they complete a set whose
 * contrast has been validated — add a chip by picking the tint pair, do not
 * re-derive one. Every token that IS used is contrast-checked in both modes;
 * see the ratios recorded next to the palettes in theme.cpp.
 */
struct Theme
{
    /* surfaces */
    SDL_Color bg;          /* page background                          */
    SDL_Color surface;     /* card / panel                             */
    SDL_Color surfaceAlt;  /* inset fill: segments, value boxes, chips  */
    SDL_Color hover;       /* pointer-over fill for neutral surfaces   */
    SDL_Color press;       /* neutral (secondary) button fill          */
    /* accents */
    SDL_Color primary;     /* filled primary button                    */
    SDL_Color primaryHover;
    SDL_Color onPrimary;   /* content drawn on a primary fill          */
    SDL_Color accent;      /* the blue used as foreground (active nav) */
    SDL_Color accentSoft;  /* translucent accent wash (radar rings)    */
    SDL_Color success;     /* green as foreground (status text)        */
    SDL_Color successSoft; /* translucent green wash                   */
    SDL_Color danger;      /* red as foreground (links, error text)    */
    SDL_Color dangerSoft;  /* translucent red wash                     */
    SDL_Color warning;     /* amber as foreground                      */
    /* text */
    SDL_Color text;
    SDL_Color textDim;
    SDL_Color inputBg;
    SDL_Color border;      /* sole edge of controls sitting on bg      */
    SDL_Color shadow;      /* translucent; painted as a stacked soft shadow */
    /* Opaque tint pairs: a chip's fill and the text that sits on it. The
     * translucent *Soft tokens above blend into whatever is behind them and so
     * cannot be contrast-checked; these are pre-composited against the surface
     * and are the ones to use when text is drawn on the tint. */
    SDL_Color primaryTint, onPrimaryTint;
    SDL_Color successTint, onSuccessTint;
    SDL_Color dangerTint,  onDangerTint;
    SDL_Color warningTint, onWarningTint;
    int radius;            /* control radius (buttons, inputs, chips)   */
    int radiusCard;        /* card / panel radius — larger than a control's */
};

/* Mode-independent constants. These are deliberately not palette tokens: a
 * destructive button is the same red in both modes, and it is dark enough for
 * white text (5.54:1) without a second pair of colours to keep in sync. */
#define DANGER_SOLID ((SDL_Color){0xC6, 0x2A, 0x3A, 0xFF})  /* white on it 5.54:1 */
#define SWITCH_ON    ((SDL_Color){0x15, 0x80, 0x3D, 0xFF})  /* white knob   5.01:1 */

enum class ThemeMode { Light, Dark, System };

/** The live palette. app.cpp aliases this as THEME. */
Theme &theme();

/** Configured mode (what the user picked, not what is currently painted). */
ThemeMode themeMode();
/** Switch mode; re-resolves the palette immediately. */
void themeSetMode(ThemeMode mode);
/** Re-resolve "follow system"; cheap, call once per frame. Rate-limits itself.
 *  Polls only on Windows — see the note above about the other platforms. */
void themeTick(Uint32 now_ms);
/** Raw OS preference (true = dark). Exposed for logging/diagnostics. */
bool themeSystemDark();

/** "light" | "dark" | "system" — the config.ini spelling. */
const char *themeModeKey(ThemeMode mode);
/** Parse a config.ini value; returns false and leaves 'out' untouched if unknown. */
bool themeModeFromKey(const char *key, ThemeMode &out);

#endif
