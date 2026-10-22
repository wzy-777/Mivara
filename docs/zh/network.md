# 网络与防火墙

> [文档索引](../README.md) · [返回 README](../../README.md) · [English](../en/network.md)

本页说明 Mivara 在局域网中使用的端口、接收端需要放行的防火墙规则，以及链路层面的要求。完整的端口定义与各服务的协议用途见[架构与协议 · 网络端口](architecture.md#网络端口)，本页不再重复端口表。

## 防火墙规则（接收端）

接收端需要接受发送端的发现请求与视频流。首次运行时 Windows 可能弹出防火墙授权提示；如果已拒绝，或使用自定义防火墙/安全组，可按下列命令手动放行。

```powershell
# Windows（管理员 PowerShell）
New-NetFirewallRule -DisplayName "Mivara SSDP"    -Direction Inbound -Protocol UDP -LocalPort 1900 -Action Allow
New-NetFirewallRule -DisplayName "Mivara Stream"  -Direction Inbound -Protocol TCP -LocalPort 8081 -Action Allow
New-NetFirewallRule -DisplayName "Mivara Compute" -Direction Inbound -Protocol TCP -LocalPort 8082 -Action Allow
New-NetFirewallRule -DisplayName "Mivara Ports"   -Direction Inbound -Protocol TCP -LocalPort 8083 -Action Allow
```

```bash
# Linux (UFW)
sudo ufw allow 1900/udp comment 'Mivara SSDP'
sudo ufw allow 8081/tcp comment 'Mivara Video Stream'
sudo ufw allow 8082/tcp comment 'Mivara Compute Offload'
sudo ufw allow 8083/tcp comment 'Mivara Port Inspector'
```

## 端口检查服务（8083）安全提示

`8083` 的端口检查服务**没有认证机制**：任何能访问该端口的局域网设备都可以执行 LIST/GET/KILL，查看或终止接收端的进程。请只在可信局域网内使用，不要将该端口暴露到公网，也不要通过路由器做端口转发。

## 网络要求

- 所有设备同一子网
- 多播依赖交换机 IGMP snooping 配置；**多播被拦截时用直连模式**（`sender <IP>`）
- 1080p@60 建议千兆有线；5 GHz WiFi 可用于较低分辨率
