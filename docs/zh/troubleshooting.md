# 故障排除

> [文档索引](../README.md) · [返回 README](../../README.md) · [English](../en/troubleshooting.md)

本页按现象列出常见问题与解决办法。与虚拟显示器驱动相关的报错见[虚拟显示器驱动](virtual-display.md)，与构建、运行时 DLL 闭包相关的问题见[编译](building.md)。

## 常见问题

| 现象 | 原因 | 解决 |
|------|------|------|
| No receivers found | UDP 1900 多播/广播均被拦 | 用直连模式 `sender <IP>` |
| 程序秒退 / 接收端无日志 | `receiver`/`app` 是 GUI 子系统（-mwindows），stdout 丢弃 | sender 自带控制台日志；receiver/app 需手动用 `-mconsole` 重新链接看输出（见[编译](building.md)） |
| 找不到 *.dll | 运行时 DLL 闭包不全 | 用 `ldd bin/app.exe` 解析闭包，把缺失 DLL 拷到 `bin/`（与 exe 同目录）；详见[编译](building.md) |
| Connection refused | 接收端未运行 | 检查 8081 端口，重启接收端 |
| "Virtual display driver is not installed on the sender" | 发送端未安装 MttVDD（旧安装器只复制了文件） | 重新安装并保留虚拟显示驱动任务，或按[手动安装与修复](virtual-display.md)运行脚本 |
| "MttVDD is installed, but no virtual monitor was exposed" | 驱动存在但无法启用或没有暴露显示器 | 检查设备管理器中的 Root\MttVDD 状态和 `vdd\install_log.txt`；详见[虚拟显示器驱动](virtual-display.md) |
| 扩展模式黑屏 / 无画面 | 两端握手不一致（模式值 0=镜像 1=扩展） | 接收端与发送端使用同版本二进制 |
| 断开后虚拟屏仍在「设置 → 屏幕」 | 旧版本只做 CCD 分离，或未以管理员运行 | 已修复：退出时禁用 Root\MttVDD 设备（需管理员权限运行 sender/app）。若日志出现 disable 失败，可在设备管理器手动禁用「Virtual Display Driver」。驱动说明见[虚拟显示器驱动](virtual-display.md) |
| 「检查更新」失败（DNS 解析失败 / 请求超时 / 安全连接失败） | 检查只访问 `github.com`，直连不稳定时（企业网络或大陆网络常见）会失败，属预期降级 | 点设置页「打开下载页」用浏览器（走系统代理/VPN）手动下载安装；安装新版本前建议先在应用内「退出」，确保虚拟显示器正确卸载 |
| 点「下载更新」提示「未找到校验文件」 | 该 Release 未附带 `SHA256SUMS.txt`（应用内安装要求有哈希锚点，缺失时不做应用内安装） | 用「打开下载页」手动下载安装即可 |
| 下载/运行时 SmartScreen 提示（「通常不会下载」「未知发布者」） | 安装包未做代码签名 + 新文件无下载信誉（正常信誉提示，非报毒） | 校验 SHA-256 后按提示继续，详见 [Windows SmartScreen 提示说明](#windows-smartscreen-提示说明) |

## Windows SmartScreen 提示说明

从 GitHub 直接下载的安装包，在部分 Windows 10/11 上会遇到以下提示——都是 **SmartScreen 的信誉提示（文件未签名 + 新发布、下载量少），不是报毒**：

| 提示 | 出现位置 | 原因 |
|------|----------|------|
| 「Mivara-Setup.exe 通常不会下载。打开前请确保信任…」 | 浏览器下载栏 | 该文件尚无下载信誉（Microsoft Defender SmartScreen 常规检查） |
| 「Windows 已保护你的电脑」 | 双击运行时（可能） | 安装包未做代码签名 |
| 「发布者：未知」 | UAC 提权窗口 | 同上（未签名） |

先校验文件完整性（结果应与对应 Release 说明中的 SHA-256 一致）：

```powershell
Get-FileHash "$HOME\Downloads\Mivara-Setup.exe" -Algorithm SHA256
```

校验一致后按正常流程继续：

1. 浏览器下载条目旁「…」→「保留」；
2. 双击安装包 →「更多信息」→「仍要运行」。

> 若仍有顾虑：`属性 → 常规 → 解除锁定`（或 PowerShell `Unblock-File`）是 Windows 官方的「我已确认信任此文件」操作，可先校验哈希再执行。

（本安装包目前未做代码签名，这是开源项目的常见现状。）

## 诊断命令

```bash
netstat -tulpn | grep -E '8081|8082|8083|1900'   # 接收端四端口
nc -zv <接收端IP> 8081                            # TCP 可达性
make check                                        # 环境自检
```
