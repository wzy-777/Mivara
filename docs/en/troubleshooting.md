# Troubleshooting

> [Docs index](../README.md) · [Back to README](../../README_EN.md) · [中文](../zh/troubleshooting.md)

This page lists common problems by symptom and how to fix them. Driver errors are covered in [Windows Virtual Display Driver](virtual-display.md); build and runtime DLL-closure issues in [Building](building.md).

## Common Issues

| Symptom | Cause | Fix |
|---------|-------|-----|
| No receivers found | UDP 1900 multicast and broadcast both blocked | Use direct connect `sender <IP>` |
| Exits instantly / no logs from receiver | `receiver`/`app` use the GUI subsystem (-mwindows), stdout discarded | sender logs to console natively; relink receiver/app with `-mconsole` to see output (see [Building](building.md)) |
| Missing *.dll | Incomplete runtime DLL closure | Resolve with `ldd bin/app.exe` and copy missing DLLs into `bin/` (next to the exe); see [Building](building.md) |
| Connection refused | Receiver not running | Check port 8081, restart the receiver |
| "Virtual display driver not found" | MttVDD driver not installed | Follow the [installation steps](virtual-display.md) |
| "Virtual display did not come up" | Driver installed but activation failed | Check device status with `pnputil /enum-devices /class Monitor`, reboot and retry; see [Windows Virtual Display Driver](virtual-display.md) |
| Extend mode black screen / no video | Handshake mismatch between the two ends (mode 0=mirror 1=extend) | Use the same binary version on both ends |
| Virtual display stays in Settings > Display after disconnect | Older builds only did a CCD detach, or sender/app not run as admin | Fixed: the Root\MttVDD device is disabled at exit (sender/app must run elevated). If the log shows a disable failure, manually disable "Virtual Display Driver" in Device Manager. See [Windows Virtual Display Driver](virtual-display.md) |
| "Check for updates" fails (DNS / timeout / secure connection failed) | The check only contacts `github.com`; a flaky direct connection (corporate or mainland-China networks) makes it fail — expected degradation | Use "Open download page" and download in your browser (system proxy/VPN); quit Mivara in-app before running the new installer so the virtual display is detached cleanly |
| "Download update" reports a missing checksum file | That release has no `SHA256SUMS.txt` (in-app install requires a hash anchor, so it is not offered without one) | Use "Open download page" and install manually |
| SmartScreen prompts on download/run ("isn't commonly downloaded" / "Unknown publisher") | Installer is not code-signed and the new file has no download reputation (a normal reputation prompt, not a malware verdict) | Verify the SHA-256, then continue per the prompt — see [Windows SmartScreen Notice](#windows-smartscreen-notice) |

## Windows SmartScreen Notice

Installing straight from GitHub may show the following prompts on some Windows 10/11 machines — these are **SmartScreen reputation prompts (unsigned file, newly released, low download count), not malware verdicts**:

| Prompt | Where | Cause |
|--------|-------|-------|
| "Mivara-Setup.exe isn't commonly downloaded. Make sure you trust…" | Browser download bar | The file has no download reputation yet (standard Microsoft Defender SmartScreen check) |
| "Windows protected your PC" | On launch (may appear) | The installer is not code-signed |
| "Publisher: Unknown" | UAC elevation prompt | Same (unsigned) |

Verify the file first (must match the SHA-256 in the corresponding release notes):

```powershell
Get-FileHash "$HOME\Downloads\Mivara-Setup.exe" -Algorithm SHA256
```

If it matches, continue per the normal flow:

1. Browser download bar → "…" → "Keep";
2. Double-click the installer → "More info" → "Run anyway".

> If in doubt: Properties → General → "Unblock" (or PowerShell `Unblock-File`) is Windows' official "I trust this file" action — run it after the hash check.

(The installer is currently not code-signed — common for open-source projects.)

## Diagnostic Commands

```bash
netstat -tulpn | grep -E '8081|8082|8083|1900'   # receiver's four ports
nc -zv <receiver-IP> 8081                          # TCP reachability
make check                                          # environment self-check
```
