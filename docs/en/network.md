# Network & Firewall

> [Docs index](../README.md) · [Back to README](../../README_EN.md) · [中文](../zh/network.md)

This page covers the ports Mivara uses on the LAN, the firewall rules the receiver needs, and link-level requirements. For the full port definitions and what each service speaks, see [Architecture & Protocol · Network Ports](architecture.md#network-ports); this page does not repeat the port table.

> **Trusted LAN only.** Mivara provides no transport encryption or peer authentication. Never expose UDP 1900 or TCP 8081–8084 to the internet, and do not port-forward them. See [SECURITY.md](../../SECURITY.md) for the full threat model.

## Firewall Rules (Receiver)

The receiver must accept discovery requests and the video stream from the sender. Windows may show a firewall prompt on first run; if it was denied, or you use a custom firewall/security group, allow the ports manually with the commands below.

```powershell
# Windows (Administrator PowerShell)
New-NetFirewallRule -DisplayName "Mivara SSDP"    -Direction Inbound -Protocol UDP -LocalPort 1900 -Action Allow
New-NetFirewallRule -DisplayName "Mivara Stream"  -Direction Inbound -Protocol TCP -LocalPort 8081 -Action Allow
New-NetFirewallRule -DisplayName "Mivara Compute" -Direction Inbound -Protocol TCP -LocalPort 8082 -Action Allow
New-NetFirewallRule -DisplayName "Mivara Ports"   -Direction Inbound -Protocol TCP -LocalPort 8083 -Action Allow
New-NetFirewallRule -DisplayName "Mivara Stitch"  -Direction Inbound -Protocol TCP -LocalPort 8084 -Action Allow
```

```bash
# Linux (UFW)
sudo ufw allow 1900/udp comment 'Mivara SSDP'
sudo ufw allow 8081/tcp comment 'Mivara Video Stream'
sudo ufw allow 8082/tcp comment 'Mivara Compute Offload'
sudo ufw allow 8083/tcp comment 'Mivara Port Inspector'
sudo ufw allow 8084/tcp comment 'Mivara Stitch KVM'
```

## Port Inspector (8083) Security Notice

The `8083` port inspector has **no authentication**: any LAN peer that can reach the port can issue LIST/GET/KILL to inspect or terminate receiver-side processes. Keep it on a trusted LAN only — never expose the port to the public internet or forward it through a router.

The stitch channel 8084 is also unencrypted and does not authenticate the peer; security rests entirely on the receiver's per-session confirmation. As with 8083, keep it on a trusted LAN only.

## Network Requirements

- All devices on the same subnet
- Multicast depends on switch IGMP snooping; **if multicast is blocked, use direct connect** (`sender <IP>`)
- 1080p@60 over gigabit wired recommended; 5 GHz WiFi works at lower resolutions
