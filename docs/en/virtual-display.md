# Virtual Display Driver

> [Docs index](../README.md) · [Back to README](../../README_EN.md) · [中文](../zh/virtual-display.md)

Extend mode relies on a Windows indirect display driver (IddCx) to provide a real virtual monitor. This page covers the driver's provenance, the one-time installation steps and resolution configuration; for how the device is enabled, attached and cleaned up during each session, see [Architecture & Protocol](architecture.md).

## Driver Provenance

This project adapts **[VirtualDrivers/Virtual-Display-Driver](https://github.com/VirtualDrivers/Virtual-Display-Driver)** (MIT license, properly signed via SignPath).

## Installation and Lifecycle

The driver **only needs to be installed once**. Afterwards no manual steps are needed — sender enables the device and attaches the virtual display when an extend session starts, and detaches + disables it when the session ends (so it disappears from Settings > Display completely). For the per-session enable/disable, CCD attach/detach and exit paths, see [Architecture & Protocol](architecture.md).

### Installation (Administrator PowerShell)

The driver is bundled with the installer under `vdd\` (`C:\Program Files\Mivara\vdd` by default); a source checkout has the same layout. Run the bundled script from an elevated PowerShell — it stages the driver, creates the `Root\MttVDD` device, copies `vdd_settings.xml` and picks the x86/AMD64 or ARM64 package for the current machine:

```powershell
cd "C:\Program Files\Mivara\vdd"   # adjust if installed elsewhere
powershell -ExecutionPolicy Bypass -File .\install_vdd.ps1

# "Generic Monitor (VDD by MTT)" appearing in Settings > Display means success
```

Manual equivalent, if you prefer to run the steps yourself:

```powershell
$drv = 'SignedDrivers\x86\VDD'   # ARM64 Windows: SignedDrivers\ARM64\VDD
# (note: the x86 directory's INF actually targets NTamd64)

pnputil /add-driver "$drv\MttVDD.inf" /install
.\Dependencies\devcon.exe create "@Root\MttVDD"
Copy-Item "$drv\vdd_settings.xml" C:\Windows\System32\drivers\UMDF\vdd_settings.xml
pnputil /scan-devices
```

### Resolution Notes

Extend mode **asks the receiver for its current resolution first** (a zero-window probe) and then configures the virtual display to the same size, so the extended screen is 1:1 instead of stretched when the two aspect ratios differ. The virtual display is placed right of the existing desktop.

The driver reads its mode list from `vdd_settings.xml` once at start-up; a size that is not in the list cannot be applied. Therefore:

- If the list already contains the receiver's size, it is used directly (the shipped list has 800x600, 1366x768, 1920x1080, 2560x1440 and 3840x2160).
- If it does not, the sender can append the size to `vdd_settings.xml` **after explicit user consent** (the original is backed up once as `vdd_settings.xml.mivara.bak` and the result is re-read and verified), then restarts the virtual display device once so the driver reloads it. The GUI asks on the first extend connection and remembers the answer in `config.ini` as `vdd_mode_write` (`ask` / `always` / `never`). If the virtual display is currently in use (e.g. a leftover session after a crash), the file is left untouched this time; the new mode loads at the next session.
- Without an exact mode and without write permission, the sender picks the **closest aspect ratio** the driver exposes and the receiver centres the picture with black bars instead of stretching it.
- Matching uses the receiver's full resolution: a 4K receiver also raises capture, encoding and bandwidth. To save resources, set a smaller `extend_res` in `config.ini`; the receiver then centres the picture with black bars, still without stretching.

To force a size manually, use `--extend-res 2560x1600` on the command line or set `extend_res=2560x1600` in `config.ini` (default `auto`). Added modes appear in the sender's Windows Settings > Display > Display resolution list while the extend session is running and can be selected there; they remain available to every later extend session.

> The settings path comes from the registry `HKLM\SOFTWARE\MikeTheTech\VirtualDisplayDriver` values `SettingsPath` / `VDDPATH` when present, otherwise `C:\Windows\System32\drivers\UMDF\vdd_settings.xml`. Writing requires administrator rights (the sender carries a `requireAdministrator` manifest).
