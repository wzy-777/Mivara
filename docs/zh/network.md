# 网络与防火墙

> [文档索引](../README.md) · [返回 README](../../README.md) · [English](../en/network.md)

本页说明 Mivara 在局域网中使用的端口、接收端需要放行的防火墙规则，以及链路层面的要求。完整的端口定义与各服务的协议用途见[架构与协议 · 网络端口](architecture.md#网络端口)，本页不再重复端口表。

> **仅限可信局域网。** Mivara 的接收端可以要求手动授权或密码（8081/8084 同一策略），8082/8083 也只接受当前会话令牌；但所有通道**都不加密**，授权只决定谁能接入。请勿将 UDP 1900 或 TCP 8081–8085 暴露到公网，也不要为它们做端口转发。完整威胁模型见 [SECURITY.md](../../SECURITY.md)。

## 防火墙规则（接收端）

接收端需要接受发送端的发现请求与视频流。首次运行时 Windows 可能弹出防火墙授权提示；如果已拒绝，或使用自定义防火墙/安全组，可按下列命令手动放行。

```powershell
# Windows（管理员 PowerShell）
New-NetFirewallRule -DisplayName "Mivara SSDP"    -Direction Inbound -Protocol UDP -LocalPort 1900 -Action Allow
New-NetFirewallRule -DisplayName "Mivara Stream"  -Direction Inbound -Protocol TCP -LocalPort 8081 -Action Allow
New-NetFirewallRule -DisplayName "Mivara Compute" -Direction Inbound -Protocol TCP -LocalPort 8082 -Action Allow
New-NetFirewallRule -DisplayName "Mivara Ports"   -Direction Inbound -Protocol TCP -LocalPort 8083 -Action Allow
New-NetFirewallRule -DisplayName "Mivara Stitch"  -Direction Inbound -Protocol TCP -LocalPort 8084 -Action Allow
# 仅在使用拼接文件投放时（可选，两端默认关闭）：
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

## 授权与附加服务安全提示

- **8081 / 8084** 执行接收端「设置 → 接收与安全」里的同一策略：允许任意连接 / 仅密码 / 每次手动授权（默认）。两者共享同一个活动会话槽：任一端已有会话时，另一端的连接直接收到「忙」。密码只在本机保存 PBKDF2 记录，验证用一次性随机数 + 按端口用途分离的 HMAC 应答，密码不出网；但**画面与键鼠事件不加密**。
- **8082 / 8083** 需要本次已授权 8081 会话的 32 字节令牌：连接时出示，之后**每个请求前**都会重新校验，会话结束即失效——已建立的旁路连接会在下一次请求时断开，旧令牌的新连接也会被拒绝。它们仍不加密，因此能窃听链路的设备仍可读取查询结果或终止进程请求。
- **8085** 是拼接文件投放的伴随通道（可选，两端默认关闭）：接收端由当前拼接会话生成一次性 32 字节令牌，在 8084 授权成功后下发，8085 连接出示令牌才被接受，会话结束即失效。它不占用 8084/8081 的活动会话槽，也不额外弹授权框；但通道不加密，**能嗅探 8084 的设备可以截获令牌并抢连 8085 写入文件**。只在可信网络开启，并把接收目录视为该会话可写的位置。
- 「允许任意连接」等于对任何可达设备放行，只在完全可控的网络里使用。
- 请只在可信局域网内使用，不要将这些端口暴露到公网，也不要通过路由器做端口转发。

## 网络要求

- 所有设备同一子网
- 多播依赖交换机 IGMP snooping 配置；**多播被拦截时用直连模式**（`sender <IP>`）
- 1080p@60 建议千兆有线；5 GHz WiFi 可用于较低分辨率
