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
| `app` | 图形启动器：左侧导航（主页/设置）、接收/发送选择、主页内嵌连接历史、SSDP 搜索、机器列表（投屏/扩展/拼接）、手动 IP、运行时调参 |
| `sender` | 采集画面（GDI/X11/CoreGraphics），协商模式，推流；扩展模式下经 vdd 模块管理虚拟显示器的激活与定位；`--mode stitch` 时改为运行拼接控制器（kvm 模块），不采集画面 |
| `receiver` | SSDP 广播，全屏渲染，承载计算卸载、端口检查与拼接输入三个附加服务 |

## 网络端口

| 端口 | 协议 | 用途 |
|------|------|------|
| 1900 | UDP 多播 | SSDP 发现（M-SEARCH / NOTIFY） |
| 8081 | TCP | 视频帧流 |
| 8082 | TCP | CPU 计算卸载服务 |
| 8083 | TCP | 远程端口检查服务 |
| 8084 | TCP | 拼接（软件 KVM）键鼠输入通道 |

这些端口在接收端都需要放行；防火墙规则与网络要求见 [网络与防火墙](network.md)。

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
bin/sender 192.168.1.105 --mode stitch    # 拼接：只连 8084 键鼠通道，不传视频（GUI 使用）
```

更多网络细节（多播、子网与防火墙）见 [网络与防火墙](network.md)。

### 2 — 扩展握手

选定接收端与显示模式后，**扩展模式**先发送一个 `width = height = 0` 的 `HsRequestV2` 探测包：接收端在 `HsResponseV2.recv_width / recv_height` 中返回当前显示分辨率，`status = 3`（尺寸错误），随后在创建 SDL 窗口、打开解码器之前关闭连接——不会闪窗。发送端据此在启动虚拟显示器前确定目标尺寸。这是既有协议的合法请求，协议版本不变。

随后发送端连接 TCP 8081 交换正式握手包（协议 v2，全部为网络字节序）：

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
      → 驱动仅启动时读取 vdd_settings.xml；目标尺寸缺失且用户同意时
        追加精确模式（先备份 .mivara.bak，回读校验），必要时重启设备一次
      → CCD SetDisplayConfig 激活虚拟显示器（若上次会话遗留则直接接管）
      → 定位到现有桌面右侧；优先使用接收端分辨率，否则用最接近的比例
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
        →  按采集尺寸等比适配到窗口，居中留黑边（绝不拉伸）
        →  SDL_UpdateTexture/YUVTexture → RenderCopy → RenderPresent
```

控制包：`0xFFFFFFFF` + `ctrl_type` + 负载。`CTRL_RESIZE`（扩展模式分辨率变化）与 `CTRL_RECONFIG`（运行时参数变化）都携带完整 `HsRequestV2`，接收端据此重建解码器并跳到下一个关键帧。

各编码的实际带宽占用见 [性能](performance.md)。

### 5 — 附加服务

- **计算卸载（8082）**：PING / COMPRESS（RLE 压缩）/ COLORCONV（BGR→RGB），处理发生在接收端 CPU，含真实耗时测量（Linux/macOS 用 `clock_gettime`，Windows 用 `QueryPerformanceCounter`）。服务不可用时发送端静默回退为本地未压缩帧。
- **端口检查（8083）**：LIST_TCP / LIST_UDP / LIST_ALL / GET_PORT / KILL_PORT。端口数据按平台原生采集：Linux 读 `/proc/net/*` + inode→PID 映射；macOS 调 `lsof`；Windows 用 `GetExtendedTcpTable`/`GetExtendedUdpTable` + `CreateToolhelp32Snapshot`。

> 安全提示：TCP 8083 **没有任何认证**——任何能访问该端口的局域网设备都可以列出、查询甚至终止接收端上的进程。请只在可信局域网内使用。

### 6 — 拼接通道（软件 KVM，8084）

拼接**不是显示模式**：两端都保留自己的物理显示器和桌面，只有键盘/鼠标事件经 TCP 8084 传输，因此完全不走采集、编码、虚拟显示器与 8081 视频协议（`HsRequestV2.mode` 仍只有 0/1，拼接值不会出现在 8081 上）。实现位于 `src/kvm.cpp` / `src/kvm.h`。

**握手（全部为网络字节序的 `uint32`）**

```
控制器 → 目标   KvmHello（16 字节：magic 'MVK1'、版本、保留；reserved[0] = 对方所在侧）
目标 → 控制器   KvmReply（32 字节：magic、版本、状态、目标虚拟桌面 x/y/w/h、保留）
控制器 → 目标   KvmEvent（16 字节：type、a、b、c）× N
```

状态值：`0`=同意，`1`=版本不符，`2`=对方拒绝，`3`=占用。事件类型：`1`=移动（目标虚拟桌面坐标）、`2`=按键（1/2/3/4/5，b=按下）、`3`=滚轮（a=有符号增量，b=是否横向）、`4`=键盘（a=按下，b=VK，c=扫描码 | 扩展位<<16）、`5`=全部释放。

`KvmHello.reserved[0]` 携带**控制器视角下目标的所在侧**（1=左、2=右、3=上、4=下；0 或其他值＝未指定，按右侧处理），仅用于让目标侧确认框写清进入边；旧版目标忽略该字段时按右侧处理，协议版本不变。方向本身完全是控制器侧状态——目标只注入收到的绝对坐标，因此会话中切换方向不需要任何协议消息。

**控制器侧（sender --mode stitch）**

- 低层钩子 `WH_MOUSE_LL` / `WH_KEYBOARD_LL` 捕获本机键鼠；钩子回调**绝不做网络 I/O**（超过系统 `LowLevelHooksTimeout` 会被静默卸载），只把事件压入无锁 SPSC 环形队列，由独立线程发送。
- 光标被系统钳制在虚拟桌面边缘时，钩子的坐标不再变化；因此用 `RIDEV_INPUTSINK` Raw Input 的设备增量判断"继续朝所选方向推"并驱动远端光标。越过所选边缘 3 像素即进入远程状态，本地输入被吞掉；朝其他方向移动会清零累计值。
- 远程时把对方光标移出**进入时那条边**，或按 `Ctrl+Alt+Shift+Q`，即交还控制；进入/离开时重新同步物理按住的修饰键与鼠标键，避免对端卡键。四条边的对应关系：右侧→本机右缘出、对方左缘入/出；左侧→本机左缘出、对方右缘入/出；上方→本机上缘出、对方下缘入/出；下方→本机下缘出、对方上缘入/出。
- 方向由 `--stitch-side left|right|top|bottom` 指定（默认 `right`）；GUI 在启动前弹窗选择并写入 `config.ini` 的 `stitch_side`，下次作为默认。会话中切换方向走 GUI→sender 的 stdin 行 `STITCH|side=…`：stdin 线程调用 `kvmSetControllerSide()` 排队，消息循环在 50 ms 定时器里应用——若此刻正在控制对方，会先交还并释放按键，再挂上新边。
- 连接失败、握手失败、对方拒绝、连接断开都会输出对应 `STAT|event=error`；事件状态变化输出 `STAT|event=stitch|state=local|remote|waiting|…|side=…`，GUI 状态面板据此显示当前方向并提供四个切换选项。
- 非 Windows 平台直接返回错误（拼接暂只支持 Windows）。

**目标侧（receiver 的 8084 服务）**

- 独立线程监听 8084，与视频监听互不影响；空闲与会话中都会轮询停止标志，所有退出路径都会 join。
- 会话在独立线程中执行，监听线程始终保持响应：会话进行中第二个控制端会立即收到 `KVM_ERR_BUSY`（先读取其 hello 再回复，避免关闭未读完的连接触发 RST）；两端都启用 TCP 保活（空闲 15 s、探测间隔 3 s），控制端掉电或断网时约一分钟内结束会话并释放按住的键。
- 收到合法 `KvmHello` 后弹出**置顶确认框**（默认「否」，30 秒自动拒绝，进程退出时自动关闭），文字按 `reserved[0]` 的方向说明对方会从本机哪条边进入；同意后才返回目标虚拟桌面尺寸并开始注入；明确点「否」后 60 秒内同一来源直接拒绝，避免弹窗轰炸（30 秒超时未答不算拒绝、不触发冷却）。等待确认期间监视线程还会 `select` 该连接：控制端若已按 Ctrl+C/「停止」断开，确认框立即关闭、会话位立即释放，不必等满 30 秒。弹框前会先做一次非阻塞 FIN 检查；等待期间被对端取消不算拒绝，但随后 3 秒内会推迟下一次弹框（按来源记录在最多 8 条的表里，另有一份全局记录，因此轮换来源地址也只能绕开按来源那半、绕不开全局退避），且任意两次确认框之间至少间隔 1.5 秒，避免「连上就断」反复闪烁置顶窗口。只有用户真的点了「否」才计入冷却：`MessageBoxW` 返回 0（如无交互式窗口站、资源不足）只视为弹框失败，回复拒绝但不冷却。等待推迟期间仍会轮询该连接，对端若已断开则立即释放会话位。
- 事件用 `SendInput` 注入：鼠标为绝对坐标 + `MOUSEEVENTF_VIRTUALDESK`（0..65535 归一化），键盘优先用扫描码（`KEYEVENTF_SCANCODE`，必要时加扩展位）。会话结束、断开或进程正常退出都会对已按下的每个键/鼠标键发送一次释放（进程被强制终止时来不及执行，这是 GUI 默认隐藏控制台启动下「停止接收」的已知限制）。
- 注入在会话线程内串行执行；`SendInput` 不能跨 UIPI 注入**更高**完整性级别的窗口，也不会作用于安全桌面（因此无法模拟 Ctrl+Alt+Del）。单独运行的 `receiver` 是普通权限，管理员窗口收不到拼接输入；经 GUI 启动的接收端继承 GUI 的管理员权限，因此可以输入到管理员窗口。

> 安全提示：8084 通道**没有加密**，且不验证对端身份——安全性完全依赖目标侧每次会话的人工确认。与 8083 一样，请只在可信局域网内使用。

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
| `mode` | `0`=镜像　`1`=扩展（拼接不走此协议，见 [8084 通道](#6--拼接通道软件-kvm8084)） |
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
