# 虚拟显示器驱动

> [文档索引](../README.md) · [返回 README](../../README.md) · [English](../en/virtual-display.md)

扩展模式依赖 Windows 间接显示驱动（IddCx）提供一块真实的虚拟显示器。本页说明驱动来源、一次性安装步骤与分辨率配置；设备在每次会话中的启用、附加与清理流程，见[架构与协议](architecture.md)。

## 驱动来源

本项目适配 **[VirtualDrivers/Virtual-Display-Driver](https://github.com/VirtualDrivers/Virtual-Display-Driver)**（MIT 协议，SignPath 正规签名）。

## 安装与生命周期

驱动**只需安装一次**。装好后无需任何手动操作——sender 会在扩展会话开始时自动启用设备并激活虚拟显示器、结束时分离并禁用设备（彻底从「设置 → 屏幕」消失）。每次会话的具体启用/禁用、CCD 附加/分离与退出路径，见[架构与协议](architecture.md)。

### 安装步骤（管理员 PowerShell）

驱动已随安装包放在 `vdd\` 目录下（默认 `C:\Program Files\Mivara\vdd`），源码 checkout 的目录结构相同。请在管理员 PowerShell 中运行随附脚本——它会完成驱动入库、创建 `Root\MttVDD` 设备、拷贝 `vdd_settings.xml`，并按当前机器选择 x86/AMD64 或 ARM64 驱动包：

```powershell
cd "C:\Program Files\Mivara\vdd"   # 安装到其他位置时自行调整
powershell -ExecutionPolicy Bypass -File .\install_vdd.ps1

# 「设置 → 屏幕」中出现 "Generic Monitor (VDD by MTT)" 即成功
```

若希望手动执行，等价步骤如下：

```powershell
$drv = 'SignedDrivers\x86\VDD'   # ARM64 Windows 改为 SignedDrivers\ARM64\VDD
# （注意：x86 目录内的 INF 实际面向 NTamd64）

pnputil /add-driver "$drv\MttVDD.inf" /install
.\Dependencies\devcon.exe create "@Root\MttVDD"
Copy-Item "$drv\vdd_settings.xml" C:\Windows\System32\drivers\UMDF\vdd_settings.xml
pnputil /scan-devices
```

### 分辨率说明

扩展模式会**先询问接收端当前的分辨率**（一次零窗口探测），再把虚拟显示器配置成相同尺寸，因此扩展屏可以 1:1 显示、不会因为两边比例不同而拉伸。虚拟显示器始终置于现有桌面右侧。

驱动只在启动时读取一次 `vdd_settings.xml` 中的模式列表，列表里没有的尺寸无法应用。因此：

- 列表里已有接收端尺寸时直接使用（内置 800x600、1366x768、1920x1080、2560x1440、3840x2160）。
- 列表里没有时，sender 可以在**用户明确同意后**把该尺寸追加进 `vdd_settings.xml`（首次写入前备份为 `vdd_settings.xml.mivara.bak`，写入后回读校验），并重启一次虚拟显示设备让驱动重新加载。GUI 会在首次扩展连接时询问一次，选择保存在 `config.ini` 的 `vdd_mode_write`（`ask` / `always` / `never`）。若此时虚拟屏正被占用（例如上次异常退出遗留的会话），本次不会改动文件，新模式会在下一次会话开始时加载。
- 如果既没有精确模式、也没有获得写入授权，sender 会选择驱动中**最接近的比例**，接收端再把画面按比例居中显示——多出来的部分是黑边，而不是拉伸。
- 自动匹配会把虚拟显示器设成接收端的完整分辨率：接收端是 4K 时，采集、编码和带宽也会随之增加。若想省资源，可在 `config.ini` 用 `extend_res` 指定较小的尺寸，接收端会黑边居中，仍然不会拉伸。

手动指定尺寸可用命令行 `--extend-res 2560x1600`，或在 `config.ini` 中设置 `extend_res=2560x1600`（默认 `auto`）。写入的模式会在扩展会话期间出现在发送端 Windows「设置 → 屏幕 → 显示器分辨率」列表中，可直接选择；之后每次扩展会话都仍然可用。

> `vdd_settings.xml` 的路径优先取注册表 `HKLM\SOFTWARE\MikeTheTech\VirtualDisplayDriver` 的 `SettingsPath` / `VDDPATH`，否则使用 `C:\Windows\System32\drivers\UMDF\vdd_settings.xml`。写入需要管理员权限（sender 自带 `requireAdministrator` 清单）。
