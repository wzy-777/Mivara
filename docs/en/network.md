# Network & Firewall

> [Docs index](../README.md) · [Back to README](../../README_EN.md) · [中文](../zh/network.md)

This page covers the ports Mivara uses on the LAN, the firewall rules the receiver needs, and link-level requirements. For the full port definitions and what each service speaks, see [Architecture & Protocol · Network Ports](architecture.md#network-ports); this page does not repeat the port table.

> **Trusted LAN only.** The receiver can require manual approval or a password (the same policy for 8081 and 8084), and 8082/8083 only accept the current session token — but **no channel is encrypted**, and authorization only decides who may connect. Never expose UDP 1900 or TCP 8081–8085 to the internet, and do not port-forward them. See [SECURITY.md](../../SECURITY.md) for the full threat model.

## Firewall Rules (Receiver)

The receiver must accept discovery requests and the video stream from the sender. Windows may show a firewall prompt on first run; if it was denied, or you use a custom firewall/security group, allow the ports manually with the commands below.

```powershell
# Windows (Administrator PowerShell)
New-NetFirewallRule -DisplayName "Mivara SSDP"    -Direction Inbound -Protocol UDP -LocalPort 1900 -Action Allow
New-NetFirewallRule -DisplayName "Mivara Stream"  -Direction Inbound -Protocol TCP -LocalPort 8081 -Action Allow
New-NetFirewallRule -DisplayName "Mivara Compute" -Direction Inbound -Protocol TCP -LocalPort 8082 -Action Allow
New-NetFirewallRule -DisplayName "Mivara Ports"   -Direction Inbound -Protocol TCP -LocalPort 8083 -Action Allow
New-NetFirewallRule -DisplayName "Mivara Stitch"  -Direction Inbound -Protocol TCP -LocalPort 8084 -Action Allow
# Only for stitch file drop (optional, off by default on both ends):
New-NetFirewallRule -DisplayName "Mivara FileDrop" -Direction Inbound -Protocol TCP -LocalPort 8085 -Action Allow
```

```bash
# Linux (UFW)
sudo ufw allow 1900/udp comment 'Mivara SSDP'
sudo ufw allow 8081/tcp comment 'Mivara Video Stream'
sudo ufw allow 8082/tcp comment 'Mivara Compute Offload'
sudo ufw allow 8083/tcp comment 'Mivara Port Inspector'
sudo ufw allow 8084/tcp comment 'Mivara Stitch KVM'
sudo ufw allow 8085/tcp comment 'Mivara Stitch File Drop (optional)'
```

## Authorization and Sidecar Security Notice

- **8081 / 8084** obey the same policy from Settings → Receiving & Security: allow any connection / password only / manual approval (the default). They share one active-session slot: while either has a session, the other is answered "busy". Only a PBKDF2 record is stored locally and the password never crosses the wire (a fresh nonce is answered with a proof domain-separated per port), but **the video and input events are not encrypted**.
- **8082 / 8083** require the 32-byte token of the current authorized 8081 session. It is presented on connect and **re-checked before every request**; the gate is cleared when the session ends, so an already-open sidecar connection loses access on its next request and a new connection with the old token is refused. They are still unencrypted, so a device sniffing the link can read query results or kill requests.
- **8085** is the companion file-drop channel for stitch (optional, off by default on both ends): the receiver generates a one-shot 32-byte token for the current stitch session and hands it over after 8084 authorization; 8085 accepts a connection only with that token, and the token dies with the session. It does not claim the 8081/8084 active-session slot and does not add a second approval prompt — but it is unencrypted, so **a device that can sniff 8084 can capture the token and race it to write files over 8085**. Enable it only on trusted networks and treat the receive folder as writable by that session.
- "Allow any connection" lets any reachable device in — use it only on a network you fully control.
- Keep the ports on a trusted LAN only — never expose them to the internet or forward them through a router.

## Network Requirements

- All devices on the same subnet
- Multicast depends on switch IGMP snooping; **if multicast is blocked, use direct connect** (`sender <IP>`)
- 1080p@60 over gigabit wired recommended; 5 GHz WiFi works at lower resolutions
