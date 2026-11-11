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
| "Virtual display driver is not installed on the sender" | MttVDD is missing on the sender (older installers only copied files) | Reinstall with the virtual-display task selected, or use [Manual Installation and Repair](virtual-display.md) |
| "MttVDD is installed, but no virtual monitor was exposed" | The device exists but cannot be enabled or exposes no monitor | Check Root\MttVDD in Device Manager and `vdd\install_log.txt`; see [Windows Virtual Display Driver](virtual-display.md) |
| Extend mode black screen / no video | Handshake mismatch between the two ends (mode 0=mirror 1=extend) | Use the same binary version on both ends |
| Virtual display stays in Settings > Display after disconnect | Older builds only did a CCD detach, or sender/app not run as admin | Fixed: the Root\MttVDD device is disabled at exit (sender/app must run elevated). If the log shows a disable failure, manually disable "Virtual Display Driver" in Device Manager. See [Windows Virtual Display Driver](virtual-display.md) |
| "Check for updates" fails (DNS / timeout / secure connection failed) | The check only contacts `github.com`; a flaky direct connection (corporate or mainland-China networks) makes it fail — expected degradation | Use "Open download page" and download in your browser (system proxy/VPN); quit Mivara in-app before running the new installer so the virtual display is detached cleanly |
| "Download update" reports a missing checksum file | That release has no `SHA256SUMS.txt` (in-app install requires a hash anchor, so it is not offered without one) | Use "Open download page" and install manually |
| SmartScreen prompts on download/run ("isn't commonly downloaded" / "Unknown publisher") | Installer is not code-signed and the new file has no download reputation (a normal reputation prompt, not a malware verdict) | Verify the SHA-256, then continue per the prompt — see [Windows SmartScreen Notice](#windows-smartscreen-notice) |
| Stitch occasionally disconnects by itself / panel says "connection lost, reconnecting…" | A local NIC switch (USB NIC selective suspend, Wi-Fi taking over) or a peer restart invalidated the TCP 8084 socket; it is not a heartbeat false positive | The controller reconnects automatically (1/2/4/8/16/30 s backoff) and Stop stays available; check `[kvm] connection lost: <reason>` and `STAT|event=stitch|state=reconnecting|reason=…` in the log for the cause. For a USB Ethernet adapter, disable "Selective suspend" or raise its idle timeout in Device Manager / adapter advanced properties, and turn off unneeded Wi-Fi auto-connect. With manual/password receive policy the reconnect asks for authorization again. If the peer's IP changed, reconnects keep dialing the old address — return Home and search again |
| Dragging a file during stitch does nothing | File drop is off on one/both ends; the send toggle was only changed after the stitch session started and the old session was never reconnected; TCP 8085 is blocked; `bin/stitchcap.exe` is missing or blocked by security software; the release point is not Explorer/the desktop | Enable "Allow the peer to send files" on the receiver and "Drag to the edge to send files" on the sender under Settings → General and save; changing the send toggle during a session now reconnects stitch automatically with the new setting (the peer consents again), while the receiver toggle applies on save; allow TCP 8085 (reverse drag also needs TCP 8087 on the controller); keep `bin/stitchcap.exe` next to the exes; release over an Explorer window or the desktop — otherwise items go to `Downloads\Mivara` (or `%LOCALAPPDATA%\Mivara\Received`). If it still fails, read the `STAT|event=drop` stages (`ready/capture/release/sending/receiving/done/error`) in the log to find the break; for capture-layer detail set `MIVARA_DROP_TRACE=1` and `MIVARA_STITCHCAP_TRACE=1` in the sender's environment and reconnect, then read `%TEMP%\mivara-dropctl-trace.log` and `%TEMP%\mivara-stitchcap-trace.log` |
| Dragging into the peer immediately bounces back and the copy is cancelled | Any small pullback after crossing used to count as "hand control back", aborting the drag | Fixed: a file drag now needs a deliberate ~24 px pushback to return. Keep moving onto the peer and release to complete the copy; a reverse drag (target → controller) lands on the controller's desktop and returns automatically on release |
| The recent-connection folder page says disabled / cannot connect | The peer has not enabled file transfer or receiving, TCP 8086 is blocked, or the versions differ | On the peer enable "Allow file transfer" under Settings → General and start receiving; update both ends; allow TCP 8086 when needed; complete the normal password/manual authorization |
| File transfer upload/download reports "invalid path" or "name or size exceeds the limit" | The peer name contains characters the local OS rejects (on Windows `: * ? " < > \|`, reserved names, trailing dot/space) or exceeds the local component limit | Rename it on the peer and retry; collision suffixes are auto-truncated, but an illegal or over-long original name cannot be created locally |

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
