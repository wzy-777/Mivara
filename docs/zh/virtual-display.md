# 虚拟显示器驱动

> [文档索引](../README.md) · [返回 README](../../README.md) · [English](../en/virtual-display.md)

扩展模式依赖 Windows 间接显示驱动（IddCx）提供一块真实的虚拟显示器。本页说明驱动来源、一次性安装步骤与分辨率配置；设备在每次会话中的启用、附加与清理流程，见[架构与协议](architecture.md)。

## 驱动来源

本项目适配 **[VirtualDrivers/Virtual-Display-Driver](https://github.com/VirtualDrivers/Virtual-Display-Driver)**（MIT 协议，SignPath 正规签名）。

## 安装与生命周期

驱动**只需安装一次**。装好后无需任何手动操作——sender 会在扩展会话开始时自动启用设备并激活虚拟显示器、结束时分离并禁用设备（彻底从「设置 → 屏幕」消失）。每次会话的具体启用/禁用、CCD 附加/分离与退出路径，见[架构与协议](architecture.md)。

### 安装步骤（管理员 PowerShell）

```powershell
# 1. 从 GitHub Releases 下载（如 VDD.Control.25.7.23.zip）并解压

# 2. 驱动入库
pnputil /add-driver MttVDD.inf /install
# （注意：x86 目录内的驱动实际面向 NTamd64）

# 3. 创建 Root\MttVDD 设备
devcon.exe install MttVDD.inf "Root\MttVDD"

# 4. 拷贝设置文件（分辨率/刷新率列表在此配置）
Copy-Item vdd_settings.xml C:\Windows\System32\drivers\UMDF\vdd_settings.xml

# 5. 扫描设备，出现 "Generic Monitor (VDD by MTT)" 即成功
pnputil /scan-devices
```

### 分辨率说明

虚拟显示器激活后由 sender 自动配置为 **1920x1080@60** 并置于现有桌面右侧。分辨率可在会话开始时选择（交互模式）或默认 1920x1080；如需修改默认值，编辑 vdd.cpp 中 `vddStartup()` 里的 `vddConfigure(dev, x, 0, w, h, 60)` 调用，或编辑驱动的 `vdd_settings.xml` 添加模式后重新编译。
