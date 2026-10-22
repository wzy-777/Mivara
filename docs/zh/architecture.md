# 架构与协议

> [文档索引](../README.md) · [返回 README](../../README.md) · [English](../en/architecture.md)

本页说明 Mivara（幻屏）的目录结构、可执行程序、网络端口、一次会话从发现到收尾的完整流程，以及协议 v2 的常量与编码选项。安装与构建步骤见[构建与打包](building.md)，日常使用见[使用指南](usage.md)，排障见[故障排除](troubleshooting.md)。

## 目录结构

```
Mivara/
├── makefile                  # 跨平台构建（Linux/macOS/Windows MinGW）
├── src/
│   ├── app.cpp               # 图形启动器（SDL2 + SDL2_ttf）：左侧导航 + 主页/设置（历史内嵌主页）
│   ├── sender.cpp            # 采集、握手、推流
│   ├── receiver.cpp          # 全屏显示、SSDP 广播、计算服务、端口服务
│   ├── proto.h               # 协议 v2（握手/控制包/编码枚举）
│   ├── codec.cpp/.h          # FFmpeg 编解码（H.264/H.265/AV1，sender + receiver）
│   ├── config.cpp/.h         # GUI 设置持久化（config.ini）
│   ├── update.cpp/.h         # 更新检查 + 应用内下载安装（GUI 专用）
│   ├── version.h             # 版本号唯一来源（C++ 与 Inno Setup 共用）
│   ├── history.cpp/.h        # 连接历史（history.ini）
│   ├── discover.cpp/.h       # SSDP 发现引擎
│   ├── gpu_accelerate.c/.h   # 远程 CPU 卸载（RLE 压缩 / 色彩修正）
│   ├── ports.cpp/.h          # 远程端口检查服务
│   └── vdd.cpp/.h            # Windows 虚拟显示器生命周期（sender + GUI 共用）
├── bin/                      # 可执行程序 + 运行时 DLL（构建输出）
├── scripts/                  # Windows 便捷启动脚本
├── docs/                     # 中英文使用文档（docs/README.md 为索引，含截图）
├── installer/                # Inno Setup 打包脚本（Mivara.iss）
├── vdd/                      # 虚拟显示器驱动（MttVDD 签名驱动 + 安装脚本）
├── winget/                   # winget-pkgs 上架清单（每次发布更新）
├── test/                     # 本地测试 / 诊断代码（不入库）
└── build/                    # 目标文件
```

## 可执行程序

| 程序 | 职责 |
|------|------|
| `app` | 图形启动器：左侧导航（主页/设置）、接收/发送选择、主页内嵌连接历史、SSDP 搜索、机器列表（投屏/扩展）、手动 IP、运行时调参 |
| `sender` | 采集画面（GDI/X11/CoreGraphics），协商模式，推流；扩展模式下经 vdd 模块管理虚拟显示器的激活与定位 |
| `receiver` | SSDP 广播，全屏渲染，承载计算卸载与端口检查两个附加服务 |

## 网络端口

| 端口 | 协议 | 用途 |
|------|------|------|
| 1900 | UDP 多播 | SSDP 发现（M-SEARCH / NOTIFY） |
| 8081 | TCP | 视频帧流 |
| 8082 | TCP | CPU 计算卸载服务 |
| 8083 | TCP | 远程端口检查服务 |

四个端口在接收端都需要放行；防火墙规则与网络要求见 [网络与防火墙](network.md)。

## 工作原理

一次完整会话依次经过：发现（或直连）→ 握手 → 扩展模式下激活虚拟显示器 → 帧流传输 → 会话收尾拆除。以下按这个顺序展开。

### 1 — 发现（SSDP）

```
接收端  →  加入 239.255.255.250:1900，周期 NOTIFY 广播，监听 M-SEARCH
发送端  →  逐网卡发送 M-SEARCH（多播 + 广播兜底），收集 200 OK 应答，列出接收端供用户选择
```

- 发送端在每块本地网卡上分别发探测（多播 + 255.255.255.255 广播），企业网丢多播时广播往往仍可达
- 应答以**数据包源地址**为准（LOCATION 多网卡机器可能写错网卡，源地址必可达）
- 接收端应答的 LOCATION 使用与请求方**同子网**的网卡 IP

若网络策略连广播也拦截，可使用直连模式：

```
bin/sender 192.168.1.105        # 跳过发现，直接连接 8081
bin/sender 192.168.1.105 8081   # 也可指定端口
bin/sender 192.168.1.105 --mode extend    # 非交互：直接指定 mirror / extend（GUI 使用）
```

更多网络细节（多播、子网与防火墙）见 [网络与防火墙](network.md)。

### 2 — 扩展握手

选定接收端与显示模式后，发送端连接 TCP 8081 交换握手包（协议 v2，全部为网络字节序）：

```
发送端 → 接收端   HsRequestV2（48 字节，uint32 x 12）
  magic          0x54564853 ('TVHS')
  version        2
  width, height  采集尺寸（缩放由编码器内部完成）
  fps            目标帧率
  mode           0=镜像  1=扩展
  codec          0=原始 1=RLE 2=H.264 3=H.265 4=AV1
  codec_flags    bit0=希望硬件编码
  bitrate_kbps   0=质量（CRF）模式
  quality        0..51（CRF 类；0xFFFFFFFF = 编码器默认哨兵，保留未用）
  gop_frames     IDR 间隔（0=默认 2*fps）
  reserved       0

接收端 → 发送端   HsResponseV2（36 字节）
  magic, version, recv_width, recv_height
  status         0=OK 1=版本 2=编码 3=尺寸 4=解码器 5=旧协议
  codec          接受的编码
  codec_flags, reserved[2]
```

两端必须为同一版本。旧版（16 字节握手）会被识别为 legacy peer 并给出明确报错，不会再黑屏。

### 3 — 虚拟显示器生命周期（扩展模式）

```
选择扩展模式
  → vddStartup()：
      启用 Root\MttVDD 设备（若被禁用；SetupAPI，需管理员）
      → CCD SetDisplayConfig 激活虚拟显示器（若上次会话遗留则直接接管）
      → 定位到现有桌面右侧，1920x1080@60
      → 自动锁定该显示器为采集源
  → 握手、推流（窗口/鼠标可穿越到虚拟屏）
  → 会话结束 / Ctrl+C / 断开连接 / 关闭控制台窗口
  → vddShutdown()：分离虚拟显示器 + 禁用 Root\MttVDD 设备
      → 虚拟屏从「设置 → 屏幕」彻底消失（不是仅从桌面分离）
```

实现于 [vdd.cpp](https://github.com/wzy-777/Mivara/blob/main/src/vdd.cpp) / [vdd.h](https://github.com/wzy-777/Mivara/blob/main/src/vdd.h)（sender 与 GUI 共用），基于
Windows CCD API（`QueryDisplayConfig` / `SetDisplayConfig`）、
`ChangeDisplaySettingsEx` 与 SetupAPI 设备启停（`DIF_PROPERTYCHANGE`）。

> 注意：仅做 CCD 分离是不够的——PnP 显示设备仍会出现在「设置 → 屏幕」里并可被
> 扩展。所以每次会话结束会**禁用设备**（整个 IddCx/UMDF 栈下线），下次扩展会话
> 开始时自动重新启用。按硬件 ID `Root\MttVDD` 精确匹配，不影响其它厂商的虚拟
> 显示适配器。

驱动的安装步骤与分辨率配置见 [虚拟显示器驱动](virtual-display.md)。

**虚拟显示器不会残留**——所有退出路径都会拆除并禁用它：

| 退出路径 | 处理方式 |
|----------|----------|
| 正常结束 / 对端断开 / 发送失败 | `main` 收尾时 `vddShutdown()` |
| Ctrl+C | 优雅退出，同样走收尾 |
| 关闭 sender 控制台窗口（X） | 在 `CTRL_CLOSE_EVENT` 处理函数内直接拆除 |
| 进程 `exit()` | `atexit(vddShutdown)` 兜底 |
| GUI「停止推流」 | 先发 Ctrl+C 优雅停止（最长等 8 秒），若被强杀则 GUI 调 `vddForceDetach()` 兜底 |

推流套接字带 2 秒发送超时：对端断开或假死时发送很快失败，会话立即收尾拆除虚拟显示器，不会卡死。

### 4 — 帧流

```
发送端   →  采集画面（扩展模式=虚拟屏；镜像=主屏）
        →  将鼠标光标绘制进帧（GetCursorInfo + DrawIconEx）
        →  按协商编码处理：
             H.264/H.265/AV1  → FFmpeg 编码（硬件优先，低延迟调优）→ 发送访问单元
             RLE              → 可选经计算卸载服务压缩（8082）
             原始             → 直接发送 RGB24
        →  发送 [uint32 帧长][帧数据]

接收端   →  读帧头、读帧体
        →  编码流 → FFmpeg 解码 → YUV/NV12 纹理渲染
        →  RLE → 解压 → RGB24 纹理
        →  SDL_UpdateTexture/YUVTexture → RenderCopy → RenderPresent
```

控制包：`0xFFFFFFFF` + `ctrl_type` + 负载。`CTRL_RESIZE`（扩展模式分辨率变化）与 `CTRL_RECONFIG`（运行时参数变化）都携带完整 `HsRequestV2`，接收端据此重建解码器并跳到下一个关键帧。

各编码的实际带宽占用见 [性能](performance.md)。

### 5 — 附加服务

- **计算卸载（8082）**：PING / COMPRESS（RLE 压缩）/ COLORCONV（BGR→RGB），处理发生在接收端 CPU，含真实耗时测量（Linux/macOS 用 `clock_gettime`，Windows 用 `QueryPerformanceCounter`）。服务不可用时发送端静默回退为本地未压缩帧。
- **端口检查（8083）**：LIST_TCP / LIST_UDP / LIST_ALL / GET_PORT / KILL_PORT。端口数据按平台原生采集：Linux 读 `/proc/net/*` + inode→PID 映射；macOS 调 `lsof`；Windows 用 `GetExtendedTcpTable`/`GetExtendedUdpTable` + `CreateToolhelp32Snapshot`。

> 安全提示：TCP 8083 **没有任何认证**——任何能访问该端口的局域网设备都可以列出、查询甚至终止接收端上的进程。请只在可信局域网内使用。

## 协议速查

| 项目 | 值 |
|------|-----|
| 握手魔数 | `0x54564853`（`'TVHS'`） |
| 协议版本 | `2` |
| 握手请求 `HsRequestV2` | 48 字节（12 × `uint32`） |
| 握手响应 `HsResponseV2` | 36 字节 |
| 字节序 | 网络字节序（全部 `uint32`） |
| 帧数据包 | `[uint32 帧长][帧数据]` |
| 控制包 | `uint32 0xFFFFFFFF` + `uint32 ctrl_type` + 负载 |
| 分辨率变化 | 控制包 `CTRL_RESIZE`，负载为完整 `HsRequestV2` |
| 运行时调参 | 控制包 `CTRL_RECONFIG`，负载为完整 `HsRequestV2` |

| 字段 | 取值 |
|------|------|
| `mode` | `0`=镜像　`1`=扩展 |
| `codec` | `0`=原始　`1`=RLE　`2`=H.264　`3`=H.265　`4`=AV1 |
| `codec_flags` | `bit0`=希望硬件编码 |
| `status` | `0`=OK　`1`=版本　`2`=编码　`3`=尺寸　`4`=解码器　`5`=旧协议 |
| `bitrate_kbps` | `0`=质量（CRF）模式 |
| `gop_frames` | `0`=默认 `2*fps` |

字段级布局见上文「[2 — 扩展握手](#2--扩展握手)」。

## 编解码

| 编码 | 说明 | 默认 |
|------|------|------|
| **H.264** | 画质/延迟均衡；硬件（NVENC/QSV/AMF/VAAPI）优先，回退 libx264 | ✅ |
| **H.265** | 同码率画质更好；硬件优先，回退 libx265 | |
| **AV1** | 压缩率最高；硬件优先，回退 SVT-AV1 | |
| **原始** | 无损 RGB24，带宽最大 | |
| **RLE** | 无损压缩（可远程卸载到接收端 CPU） | |

低延迟调优：无 B 帧、`zerolatency`、GOP=2×fps、resize 后立即 IDR。桌面文字建议在设置中开启 4:4:4 色度（仅软件编码）。

带宽与延迟数据见 [性能](performance.md)。
