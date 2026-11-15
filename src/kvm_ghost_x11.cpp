/**
 * KVM_GHOST_X11.CPP – see kvm_ghost_x11.h.
 */
#include "kvm_ghost_x11.h"

#if defined(__linux__)

#include <X11/Xutil.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace {

constexpr uint32_t kCardBg = 0x20242cu;
constexpr unsigned long kCardBorder = 0x8a93a6u;
constexpr int kPad = 12;
constexpr int kGap = 8;
constexpr int kMaxLabelBytes = 96;

std::string trimUtf8(const std::string &s, size_t max_bytes)
{
    size_t n = std::min(s.size(), max_bytes);
    while (n > 0 && n < s.size() &&
           (((unsigned char)s[n] & 0xC0) == 0x80))
        --n;
    return s.substr(0, n);
}

uint32_t blendOpaque(uint8_t b, uint8_t g, uint8_t r, uint8_t a,
                     bool premultiplied, uint32_t bg)
{
    const unsigned br = (bg >> 16) & 0xffu;
    const unsigned bgc = (bg >> 8) & 0xffu;
    const unsigned bb = bg & 0xffu;
    unsigned rr, gg, bl;
    if (premultiplied) {
        rr = r + br * (255u - a) / 255u;
        gg = g + bgc * (255u - a) / 255u;
        bl = b + bb * (255u - a) / 255u;
    } else {
        rr = (r * a + br * (255u - a)) / 255u;
        gg = (g * a + bgc * (255u - a)) / 255u;
        bl = (b * a + bb * (255u - a)) / 255u;
    }
    if (rr > 255) rr = 255;
    if (gg > 255) gg = 255;
    if (bl > 255) bl = 255;
    return (rr << 16) | (gg << 8) | bl;
}

} /* namespace */

void KvmGhostX11::attach(Display *dpy)
{
    if (!dpy) return;
    if (dpy_ == dpy) return;
    detach();
    dpy_ = dpy;
    screen_ = DefaultScreen(dpy);
    root_ = RootWindow(dpy, screen_);

    XVisualInfo vi = {};
    const bool have32 = XMatchVisualInfo(dpy, screen_, 32, TrueColor, &vi) != 0;
    const Atom cm_atom = XInternAtom(dpy, "_NET_WM_CM_S0", True);
    const bool compositor =
        cm_atom != None && XGetSelectionOwner(dpy, cm_atom) != None;
    if (have32 && compositor) {
        const Colormap cm = XCreateColormap(dpy, root_, vi.visual, AllocNone);
        if (cm != None) {
            argb_ = true;
            visual_ = vi.visual;
            depth_ = 32;
            cmap_ = cm;
            return;
        }
    }
    argb_ = false;
    visual_ = DefaultVisual(dpy, screen_);
    depth_ = DefaultDepth(dpy, screen_);
    cmap_ = DefaultColormap(dpy, screen_);
}

void KvmGhostX11::detach()
{
    if (!dpy_) return;
    if (win_) {
        XDestroyWindow(dpy_, win_);
        win_ = 0;
    }
    if (gc_) {
        XFreeGC(dpy_, gc_);
        gc_ = 0;
    }
    if (font_) {
        XFreeFont(dpy_, font_);
        font_ = nullptr;
    }
    if (argb_ && cmap_ != None && cmap_ != DefaultColormap(dpy_, screen_))
        XFreeColormap(dpy_, cmap_);
    dpy_ = nullptr;
    visual_ = nullptr;
    depth_ = 0;
    cmap_ = None;
    argb_ = false;
    visible_ = false;
    resetDrag();
}

void KvmGhostX11::ensureWindow()
{
    if (!dpy_ || win_) return;
    XSetWindowAttributes a = {};
    a.override_redirect = True;
    a.event_mask = ExposureMask;
    a.background_pixel = 0;
    a.border_pixel = 0;
    a.colormap = cmap_;
    const unsigned long mask = CWOverrideRedirect | CWEventMask |
                               CWBackPixel | CWBorderPixel | CWColormap;
    win_ = XCreateWindow(dpy_, root_, -10, -10, 1, 1, 0, depth_,
                         InputOutput, visual_, mask, &a);
    if (!win_) return;
    gc_ = XCreateGC(dpy_, win_, 0, nullptr);
    font_ = XLoadQueryFont(dpy_, "9x15");
    if (!font_) font_ = XLoadQueryFont(dpy_, "fixed");
    if (font_) XSetFont(dpy_, gc_, font_->fid);
}

void KvmGhostX11::resetDrag()
{
    count_ = 0;
    name_.clear();
    icon_w_ = icon_h_ = 0;
    icon_.clear();
    have_icon_ = false;
    img_w_ = img_h_ = img_ox_ = img_oy_ = 0;
    img_.clear();
    have_image_ = false;
    img_premult_ = true;
}

std::string KvmGhostX11::label() const
{
    std::string s = name_.empty() ? std::string("Files") : name_;
    if (count_ > 1)
        s += "  +" + std::to_string(count_ - 1);
    return trimUtf8(s, kMaxLabelBytes);
}

int KvmGhostX11::cardWidth() const
{
    int glyph = 0;
    if (have_image_) return img_w_;
    glyph = have_icon_ ? icon_w_ + kGap : 28;
    int text = 7 * 10;
    if (font_ && !label().empty())
        text = XTextWidth(font_, label().data(), (int)label().size());
    int w = kPad * 2 + glyph + text;
    if (w < 160) w = 160;
    if (w > 420) w = 420;
    return w;
}

int KvmGhostX11::cardHeight() const
{
    if (have_image_) return img_h_;
    int h = have_icon_ ? icon_h_ + 2 * kPad : 48;
    if (h < 48) h = 48;
    if (h > 180) h = 180;
    return h;
}

void KvmGhostX11::place()
{
    if (!dpy_ || !win_) return;
    w_ = cardWidth();
    h_ = cardHeight();
    if (w_ < 1) w_ = 1;
    if (h_ < 1) h_ = 1;
    if (have_image_) {
        x_ = cursor_x_ - img_ox_;
        y_ = cursor_y_ - img_oy_;
    } else {
        x_ = cursor_x_ + 16;
        y_ = cursor_y_ + 16;
    }
    XMoveResizeWindow(dpy_, win_, x_, y_, (unsigned)w_, (unsigned)h_);
    XRaiseWindow(dpy_, win_);
    XFlush(dpy_);
}

void KvmGhostX11::begin(int count)
{
    ensureWindow();
    if (!win_) return;
    resetDrag();
    count_ = std::max(1, count);
    visible_ = true;
    place();
    paint();
    XMapRaised(dpy_, win_);
    XFlush(dpy_);
}

void KvmGhostX11::setText(const std::string &name)
{
    name_ = trimUtf8(name, 512);
    if (visible_ && !have_image_) {
        place();
        paint();
    }
}

void KvmGhostX11::setIcon(int w, int h, const std::vector<uint8_t> &bgra)
{
    if (w <= 0 || h <= 0 || w > 2048 || h > 2048 ||
        (uint64_t)w * (uint64_t)h > 36864u ||
        bgra.size() != (size_t)w * (size_t)h * 4)
        return;
    icon_w_ = w;
    icon_h_ = h;
    icon_ = bgra;
    have_icon_ = true;
    if (visible_ && !have_image_) {
        place();
        paint();
    }
}

void KvmGhostX11::setImage(int w, int h, int ox, int oy,
                           const std::vector<uint8_t> &bgra,
                           bool premultiplied)
{
    if (w <= 0 || h <= 0 || w > 2048 || h > 2048 ||
        (uint64_t)w * (uint64_t)h > 36864u ||
        bgra.size() != (size_t)w * (size_t)h * 4)
        return;
    if (ox < -4096) ox = -4096;
    if (ox > 4096) ox = 4096;
    if (oy < -4096) oy = -4096;
    if (oy > 4096) oy = 4096;
    img_w_ = w;
    img_h_ = h;
    img_ox_ = ox;
    img_oy_ = oy;
    img_ = bgra;
    img_premult_ = premultiplied;
    have_image_ = true;
    if (visible_) {
        place();
        paint();
    }
}

void KvmGhostX11::move(int cursor_x, int cursor_y)
{
    if (cursor_x == cursor_x_ && cursor_y == cursor_y_) return;
    cursor_x_ = cursor_x;
    cursor_y_ = cursor_y;
    if (visible_) place();
}

void KvmGhostX11::pump()
{
    if (!dpy_ || !win_) return;
    XEvent ev;
    while (XCheckWindowEvent(dpy_, win_, ExposureMask, &ev)) {
        if (ev.type == Expose && ev.xexpose.count == 0) paint();
    }
}

void KvmGhostX11::paint()
{
    if (!dpy_ || !win_ || w_ < 1 || h_ < 1) return;
    /* Xlib on this platform does not allocate image data for a NULL data
     * pointer; pass an owned buffer. XDestroyImage frees it with free(). */
    const size_t stride = (size_t)w_ * 4u;
    char *data = (char *)std::calloc(stride, (size_t)h_);
    if (!data) return;
    XImage *xi = XCreateImage(dpy_, visual_, depth_, ZPixmap, 0, data,
                              (unsigned)w_, (unsigned)h_, 32, (int)stride);
    if (!xi) {
        std::free(data);
        return;
    }
    if (have_image_)
        paintImage(xi);
    else
        paintCard(xi);
    XPutImage(dpy_, win_, gc_, xi, 0, 0, 0, 0, (unsigned)w_,
              (unsigned)h_);
    XDestroyImage(xi);
    if (!have_image_) {
        const unsigned long fg =
            argb_ ? 0xfffffffful : WhitePixel(dpy_, screen_);
        const int text_x = have_icon_ ? 10 + icon_w_ + kGap : 40;
        if (!have_icon_) {
            XSetForeground(dpy_, gc_, fg);
            const int ix = 10, iy = (h_ - 26) / 2;
            XDrawRectangle(dpy_, win_, gc_, ix, iy, 18, 26);
            XDrawLine(dpy_, win_, gc_, ix + 18, iy + 7, ix + 11, iy);
            XDrawLine(dpy_, win_, gc_, ix + 11, iy, ix + 11, iy + 7);
            XDrawLine(dpy_, win_, gc_, ix + 11, iy + 7, ix + 18, iy + 7);
        }
        if (font_) {
            const std::string s = label();
            XSetForeground(dpy_, gc_, fg);
            const int baseline =
                (h_ + font_->ascent - font_->descent) / 2;
            XDrawString(dpy_, win_, gc_, text_x, baseline, s.data(),
                        (int)s.size());
        }
        XSetForeground(dpy_, gc_, argb_ ? 0xff8a93a6ul : kCardBorder);
        XDrawRectangle(dpy_, win_, gc_, 0, 0, (unsigned)w_ - 1,
                       (unsigned)h_ - 1);
    }
    XFlush(dpy_);
}

void KvmGhostX11::paintImage(XImage *xi)
{
    const uint8_t *p = img_.data();
    for (int y = 0; y < h_; y++) {
        for (int x = 0; x < w_; x++) {
            const uint8_t b = p[0], g = p[1], r = p[2], a = p[3];
            p += 4;
            uint32_t px;
            if (argb_) {
                if (img_premult_) {
                    px = ((uint32_t)a << 24) | ((uint32_t)r << 16) |
                         ((uint32_t)g << 8) | b;
                } else {
                    const uint32_t rp = (uint32_t)r * a / 255u;
                    const uint32_t gp = (uint32_t)g * a / 255u;
                    const uint32_t bp = (uint32_t)b * a / 255u;
                    px = ((uint32_t)a << 24) | (rp << 16) | (gp << 8) | bp;
                }
            } else {
                px = blendOpaque(b, g, r, a, img_premult_, kCardBg);
            }
            XPutPixel(xi, x, y, px);
        }
    }
}

void KvmGhostX11::paintCard(XImage *xi)
{
    const uint32_t bg = argb_ ? (0xff000000u | kCardBg) : kCardBg;
    for (int y = 0; y < h_; y++)
        for (int x = 0; x < w_; x++)
            XPutPixel(xi, x, y, bg);

    const int icon_y = (h_ - icon_h_) / 2;
    if (have_icon_) {
        const uint8_t *p = icon_.data();
        for (int y = 0; y < icon_h_; y++) {
            for (int x = 0; x < icon_w_; x++) {
                const uint8_t b = p[0], g = p[1], r = p[2], a = p[3];
                p += 4;
                const uint32_t rgb = blendOpaque(b, g, r, a, false, kCardBg);
                const uint32_t px = argb_ ? (0xff000000u | rgb) : rgb;
                XPutPixel(xi, 10 + x, icon_y + y, px);
            }
        }
    }

}

void KvmGhostX11::end()
{
    visible_ = false;
    if (dpy_ && win_) {
        XUnmapWindow(dpy_, win_);
        XFlush(dpy_);
    }
    resetDrag();
}

#endif /* __linux__ */
