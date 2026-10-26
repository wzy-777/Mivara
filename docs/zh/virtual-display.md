# 虚拟显示器驱动

> [文档索引](../README.md) · [返回 README](../../README.md) · [English](../en/virtual-display.md)

扩展模式依赖 Windows 间接显示驱动（IddCx）提供一块真实的虚拟显示器。本页说明驱动来源、一次性安装步骤与分辨率配置；设备在每次会话中的启用、附加与清理流程，见[架构与协议](architecture.md)。

## 驱动来源

本项目适配 **[VirtualDrivers/Virtual-Display-Driver](https://github.com/VirtualDrivers/Virtual-Display-Driver)**（MIT 协议，SignPath 正规签名）。

## 安装与生命周期

驱动**只需在发送端安装一次**，接收端不需要它。装好后无需任何手动操作——sender 会在扩展会话开始时自动启用设备并激活虚拟显示器、结束时分离并禁用设备（彻底从「设置 → 屏幕」消失）。每次会话的具体启用/禁用、CCD 附加/分离与退出路径，见[架构与协议](architecture.md)。

### 安装方式

Windows 安装器会在开始时检测 `Root\MttVDD` 设备：未安装时显示并默认勾选「安装虚拟显示驱动（扩展模式需要，推荐）」，复制文件后自动完成驱动入库、创建设备并选择 x86/AMD64 或 ARM64 驱动包；**已安装时改为显示默认不勾选的「重新安装虚拟显示驱动」**，直接升级不会碰现有驱动。无需扩展模式时可以取消勾选；镜像投屏和接收功能不依赖此驱动。

安装脚本可重复执行：检测到设备已存在且没有要求重新安装时会直接跳过，已有 `vdd_settings.xml` 也不会被覆盖；只有在勾选重新安装时才更新设备上的驱动。任何关键步骤失败都会返回非零状态，安装器会显示错误及日志位置，不会把失败当作成功。

### 手动安装与修复

旧版本安装器只复制驱动文件；升级后如果仍无法扩展，请在发送端的管理员 PowerShell 中运行（`-Reinstall` 用于强制重装已存在的驱动）：

```powershell
cd "C:\Program Files\Mivara\vdd"   # 安装到其他位置时自行调整
powershell -NoProfile -ExecutionPolicy Bypass -File .\install_vdd.ps1 -Reinstall
```

脚本会保留已有分辨率配置并核对 `Root\MttVDD` 设备。失败时查看同一目录的 `install_log.txt`；发送端日志还会区分「未安装驱动」「设备无法启用」和「设备已安装但未暴露虚拟显示器」。

### 分辨率说明

扩展模式会**先询问接收端当前的分辨率**（一次零窗口探测），再把虚拟显示器配置成相同尺寸，因此扩展屏可以 1:1 显示、不会因为两边比例不同而拉伸。虚拟显示器始终置于现有桌面右侧。

驱动只在启动时读取一次 `vdd_settings.xml` 中的模式列表，列表里没有的尺寸无法应用。因此：

- 列表里已有接收端尺寸时直接使用（内置 800x600、1366x768、1920x1080、2560x1440、3840x2160）。
- 列表里没有时，sender 会**自动**把该尺寸追加进 `vdd_settings.xml`（首次写入前备份为 `vdd_settings.xml.mivara.bak`，写入后回读校验），并重启一次虚拟显示设备让驱动重新加载。不再弹出授权询问；如不希望修改系统文件，可在 `config.ini` 设置 `vdd_mode_write=never`（默认 `always`，旧配置的 `ask` 按 `always` 处理）。若此时虚拟屏正被占用（例如上次异常退出遗留的会话），本次不会改动文件，新模式会在下一次会话开始时加载。
- 如果既没有精确模式、也没有获得写入授权，sender 会选择驱动中**最接近的比例**，接收端再把画面按比例居中显示——多出来的部分是黑边，而不是拉伸。
- 自动匹配会把虚拟显示器设成接收端的完整分辨率：接收端是 4K 时，采集、编码和带宽也会随之增加。若想省资源，可在 `config.ini` 用 `extend_res` 指定较小的尺寸，接收端会黑边居中，仍然不会拉伸。

手动指定尺寸可用命令行 `--extend-res 2560x1600`，或在 `config.ini` 中设置 `extend_res=2560x1600`（默认 `auto`）。写入的模式会在扩展会话期间出现在发送端 Windows「设置 → 屏幕 → 显示器分辨率」列表中，可直接选择；之后每次扩展会话都仍然可用。

> `vdd_settings.xml` 的路径优先取注册表 `HKLM\SOFTWARE\MikeTheTech\VirtualDisplayDriver` 的 `SettingsPath` / `VDDPATH`，否则使用 `C:\Windows\System32\drivers\UMDF\vdd_settings.xml`。写入需要管理员权限（sender 自带 `requireAdministrator` 清单）。
