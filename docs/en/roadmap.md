# Roadmap

> [Docs index](../README.md) · [Back to README](../../README_EN.md) · [中文](../zh/roadmap.md)

The list below tracks shipped and planned features in order; `[x]` means shipped, `[ ]` means planned.

- [x] Screen mirroring
- [x] Extend mode (virtual display auto attach/detach + per-monitor capture + pointer crossing, Windows)
- [x] Direct connect (bypass SSDP multicast)
- [x] Remote CPU offload (RLE compression + color conversion, real timing)
- [x] Extend handshake (resolution exchange)
- [x] GUI launcher (SDL2_ttf interface: receive/send/search/device list/manual IP)
- [x] macOS CoreGraphics capture
- [x] Remote port inspection service (TCP 8083)
- [x] Per-client statistics + JSON export
- [ ] Receiver input back-channel (real keyboard/mouse control from the receiver side)
- [x] Optional encoders: RLE / H.264 / H.265 / AV1 as a selectable option
- [x] GUI advanced settings (encoder picker, connection history)
- [x] In-app update check + one-click download/install (Windows and Linux, SHA-256 verified; Linux installs with privilege escalation and relaunches)
- [x] Extend-resolution choice (auto-matches the receiver; `extend_res` in `config.ini` overrides it; no GUI control yet)
- [ ] Audio capture and streaming
- [ ] TLS encryption
- [x] Virtual display resolution adapts to the receiver (pre-connection probe; exact mode added to `vdd_settings.xml` when permitted, otherwise letterboxed)
- [ ] Adaptive frame rate
- [ ] Wayland support
- [ ] Virtual display solutions for macOS/Linux
- [x] Screen stitching (pure software KVM: one keyboard/mouse drives two PCs, each keeping its own monitor; Windows/Linux-X11 controller ↔ Windows/Linux-X11 target)
- [x] Stitch file drop (drag to the edge, release over a file manager/desktop folder; copy-only, never moves or deletes the source; off by default on both ends. Both directions work on Windows and Linux/X11)
- [x] Linux reverse drag (facing-edge XDND strip + the controller's 8087 reverse listener)
- [ ] Capture a real icon/thumbnail from a Linux drag source (XDND provides no drag image; a Linux source currently shows a self-drawn card)
- [x] Connection authorization (allow any / password only / manual approval, manual by default; one policy for 8081 and 8084, session tokens for 8082/8083)
- [x] Receive status bar and independent "allow connections / auto-receive" switches
- [x] Windows tray (close hides, session keeps running, single-instance wake, quit confirmation)
