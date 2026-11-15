/**
 * KVM_GHOST_X11.H – visual drag card shared by the X11 stitch backends.
 *
 * The target shows this while a file drag is being carried over from the
 * controller; the controller shows it while a reverse drag is being carried
 * back. It is display-only:
 *
 *   - a small opaque card (page glyph or the source's icon + name) when no
 *     compositing manager is available;
 *   - a true ARGB window with the source's real premultiplied BGRA drag image
 *     when the screen has a 32-bit visual and a compositing manager.
 *
 * The caller accumulates the frame protocol (KVM_EV_DRAG_INFO/TEXT,
 * DRAG_ICON_INFO/DATA, DRAG_IMAGE_INFO/DATA) and calls setText/setIcon/
 * setImage when a sequence completes. One instance belongs to one Xlib thread.
 */
#ifndef MIVARA_KVM_GHOST_X11_H
#define MIVARA_KVM_GHOST_X11_H

#if defined(__linux__)

#include <X11/Xlib.h>
#include <cstdint>
#include <string>
#include <vector>

class KvmGhostX11 {
public:
    void attach(Display *dpy);
    void detach();

    void begin(int count);
    void setText(const std::string &name);
    /* Non-premultiplied BGRA, as sent by KVM_EV_DRAG_ICON_*. */
    void setIcon(int w, int h, const std::vector<uint8_t> &bgra);
    /* Premultiplied BGRA from the source's real drag image, or the
     * non-premultiplied icon when `premultiplied` is false. */
    void setImage(int w, int h, int ox, int oy,
                  const std::vector<uint8_t> &bgra, bool premultiplied);
    void move(int cursor_x, int cursor_y);
    void pump();
    void end();

private:
    void ensureWindow();
    void resetDrag();
    void place();
    void paint();
    void paintCard(XImage *xi);
    void paintImage(XImage *xi);
    std::string label() const;
    int cardWidth() const;
    int cardHeight() const;

    Display *dpy_ = nullptr;
    Window win_ = 0;
    GC gc_ = 0;
    XFontStruct *font_ = nullptr;
    int screen_ = 0;
    Window root_ = 0;
    Visual *visual_ = nullptr;
    int depth_ = 0;
    Colormap cmap_ = 0;
    bool argb_ = false;

    int cursor_x_ = 0, cursor_y_ = 0;
    int x_ = 0, y_ = 0, w_ = 0, h_ = 0;
    bool visible_ = false;

    int count_ = 0;
    std::string name_;

    int icon_w_ = 0, icon_h_ = 0;
    std::vector<uint8_t> icon_;
    bool have_icon_ = false;

    int img_w_ = 0, img_h_ = 0, img_ox_ = 0, img_oy_ = 0;
    bool img_premult_ = true;
    std::vector<uint8_t> img_;
    bool have_image_ = false;
};

#endif /* __linux__ */
#endif /* MIVARA_KVM_GHOST_X11_H */
