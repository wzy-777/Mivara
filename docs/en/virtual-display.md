# Virtual Display Driver

> [Docs index](../README.md) · [Back to README](../../README_EN.md) · [中文](../zh/virtual-display.md)

Extend mode relies on a Windows indirect display driver (IddCx) to provide a real virtual monitor. This page covers the driver's provenance, the one-time installation steps and resolution configuration; for how the device is enabled, attached and cleaned up during each session, see [Architecture & Protocol](architecture.md).

## Driver Provenance

This project adapts **[VirtualDrivers/Virtual-Display-Driver](https://github.com/VirtualDrivers/Virtual-Display-Driver)** (MIT license, properly signed via SignPath).

## Installation and Lifecycle

The driver **only needs to be installed once**. Afterwards no manual steps are needed — sender enables the device and attaches the virtual display when an extend session starts, and detaches + disables it when the session ends (so it disappears from Settings > Display completely). For the per-session enable/disable, CCD attach/detach and exit paths, see [Architecture & Protocol](architecture.md).

### Installation (Administrator PowerShell)

```powershell
# 1. Download from GitHub Releases (e.g. VDD.Control.25.7.23.zip) and extract

# 2. Stage the driver
pnputil /add-driver MttVDD.inf /install
# (note: the driver in the x86 directory actually targets NTamd64)

# 3. Create the Root\MttVDD device
devcon.exe install MttVDD.inf "Root\MttVDD"

# 4. Copy the settings file (resolution/refresh-rate list lives there)
Copy-Item vdd_settings.xml C:\Windows\System32\drivers\UMDF\vdd_settings.xml

# 5. Rescan; "Generic Monitor (VDD by MTT)" appearing means success
pnputil /scan-devices
```

### Resolution Notes

Once activated, the virtual display is automatically configured to **1920x1080@60** and placed right of the existing desktop. The resolution can be chosen at session start (interactive mode) or defaults to 1920x1080; to change the default, edit the `vddConfigure(dev, x, 0, w, h, 60)` call in `vddStartup()` in vdd.cpp, or add modes to the driver's `vdd_settings.xml` and rebuild.
