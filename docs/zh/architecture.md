# 架构与协议

> [文档索引](../README.md) · [返回 README](../../README.md) · [English](../en/architecture.md)

本页说明 Mivara（幻屏）的目录结构、可执行程序、网络端口、一次会话从发现到收尾的完整流程，以及协议 v3 与连接授权的常量与编码选项。安装与构建步骤见[构建与打包](building.md)，日常使用见[使用指南](usage.md)，排障见[故障排除](troubleshooting.md)。

## 目录结构

```
Mivara/
├── makefile                  # 跨平台构建（Linux/macOS/Windows MinGW）
├── src/
│   ├── app.cpp               # 图形启动器（SDL2 + SDL2_ttf）：左侧导航 + 主页/设置（历史内嵌主页）
│   ├── sender.cpp            # 采集、握手、推流
│   ├── receiver.cpp          # 全屏显示、SSDP 广播、计算服务、端口服务、拼接目标端
│   ├── proto.h               # 协议 v3（握手 + 连接授权 + 控制包/编码枚举）
│   ├── auth.cpp/.h           # SHA-256/HMAC/PBKDF2、密码记录、会话令牌、失败节流
│   ├── kvm.cpp/.h            # 拼接（软件 KVM）控制器与目标端，TCP 8084
│   ├── tray.cpp/.h           # Windows 托盘图标 + 单实例（其他平台为空实现）
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
| `app` | 图形启动器：左侧导航（主页/设置）、连接设备入口、低权重接收状态栏、主页内嵌连接历史、SSDP 搜索、机器列表（投屏/扩展/拼接）、手动 IP、授权弹窗、托盘与单实例、运行时调参 |
| `sender` | 采集画面（GDI/X11/CoreGraphics），协商模式，推流；扩展模式下经 vdd 模块管理虚拟显示器的激活与定位；`--mode stitch` 时改为运行拼接控制器（kvm 模块），不采集画面 |
| `receiver` | SSDP 广播，全屏渲染，承载计算卸载、端口检查与拼接输入三个附加服务 |

## 网络端口

| 端口 | 协议 | 用途 |
|------|------|------|
| 1900 | UDP 多播 | SSDP 发现（M-SEARCH / NOTIFY） |
| 8081 | TCP | 视频帧流 |
| 8082 | TCP | CPU 计算卸载服务（需会话令牌） |
| 8083 | TCP | 远程端口检查服务（需会话令牌） |
| 8084 | TCP | 拼接（软件 KVM）键鼠输入通道（与 8081 同一接收策略） |
| 8085 | TCP | 拼接文件投放（可选，默认关闭；由当前拼接会话的一次性令牌把关，仅复制） |

这些端口在接收端都需要放行；防火墙规则与网络要求见 [网络与防火墙](network.md)。8082/8083 不再是「无需认证」的服务：它们只接受本次已授权 8081 会话下发的 32 字节令牌，会话结束即失效。

## 工作原理

一次完整会话依次经过：发现（或直连）→ 握手 → 扩展模式下激活虚拟显示器 → 帧流传输 → 会话收尾拆除。以下按这个顺序展开。

### 1 — 发现（SSDP）

```
接收端  →  加入 239.255.255.250:1900，周期 NOTIFY 广播，监听 M-SEARCH
发送端  →  每块网卡发送 M-SEARCH，统一监听应答；GUI 首台即显示，后续增量加入
```

- 所有网卡以非阻塞 socket 同时等待，按 100 ms 间隔重传三轮；5 秒是整次搜索的总上限，不再按网卡或收包次数累计超时，取消也会中断等待
- 每块网卡发送多播、255.255.255.255 广播及本接口的定向广播；应答按**数据包源地址 + 端口**先去重，不再为每条重复应答串行探测 TCP，实际连接时再验证
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

随后发送端连接 TCP 8081 交换正式握手包（协议 v3，全部为网络字节序）：

```
发送端 → 接收端   HsRequestV2（48 字节，uint32 x 12）
  magic          0x54564853 ('TVHS')
  version        3
  width, height  采集尺寸（缩放由编码器内部完成）
  fps            目标帧率
  mode           0=镜像  1=扩展
  codec          0=原始 1=RLE 2=H.264 3=H.265 4=AV1
  codec_flags    bit0=希望硬件编码
  bitrate_kbps   0=质量（CRF）模式
  quality        0..51（CRF 类；0xFFFFFFFF = 编码器默认哨兵，保留未用）
  gop_frames     IDR 间隔（0=默认 2*fps）
  reserved       0

接收端 → 发送端   HsAuthStart（64 字节；所有策略都会发送）
  magic          0x54564841 ('TVHA')  version 2
  mode           0=任意 1=密码 2=手动授权
  kdf_iter       PBKDF2 迭代次数（仅密码模式有效）
  salt[16]       密码 KDF 盐（仅密码模式有效）
  nonce[32]      每次连接重新生成

发送端 → 接收端   HsRequestExtV3（48 字节）
  magic          0x54564858 ('TVHX')  version 2
  device[32]     发送端主机名（手动授权弹窗显示）
  flags, reserved

发送端 → 接收端   HsAuthProof（40 字节；仅密码模式）
  magic 0x54564841  version 2  proof[32]

接收端 → 发送端   HsResponseV2（36 字节）
  magic, version, recv_width, recv_height
  status         0=OK 1=版本 2=编码 3=尺寸 4=解码器 5=旧协议
                 6=忙 7=密码/节流 8=授权超时 9=被拒绝 10=接收端授权配置损坏
  codec          接受的编码
  codec_flags, reserved[2]

接收端 → 发送端   HsResponseExtV3（44 字节；仅 status=OK）
  magic 0x54564852 ('TVHR')  version 2  token[32]  flags
```

授权在**接收端**强制执行：密码只以本地 PBKDF2 记录形式存在，证明按用途分离：

```
purpose_key = HMAC-SHA256(PBKDF2(fold(密码), salt, iter), "Mivara/v1/auth-key" ‖ purpose)
proof       = HMAC-SHA256(purpose_key, "Mivara/v1/auth-proof" ‖ purpose
                          ‖ nonce ‖ HsRequestV2 ‖ HsRequestExtV3)
```

8081 的 `purpose=1`、8084 的 `purpose=2`，因此两端的录制证明不能互换。`fold` 只处理 ASCII
字母（A–Z 折叠为 a–z，其余字节不变，不受 locale 影响），因此密码不区分大小写；建立记录、
8081 发送端与 8084 拼接端必须在 PBKDF2 前使用同一折叠规则，接收端只保存记录、不做折叠。
新设置的密码使用 300000 次 PBKDF2 迭代（旧记录保留原迭代次数）。密码本身从不上网；`token` 是本次会话的
32 字节随机令牌，8082/8083 在**每个请求前**都要重新校验它。手动授权由接收端弹窗决定
（默认拒绝，30 秒超时，同一时刻只有一个授权请求；另一个设备收到「忙」）。8081 与 8084
共享同一个活动会话槽：任一端已有会话时，另一端的连接收到「忙」。所有策略都会先收到
`HsAuthStart`（任意连接/手动模式下 salt 与 iter 为空）；密码记录缺失或损坏时
回 `status=10`，绝不降级为任意连接。

> 用途分离与更高的迭代次数只能降低跨端口重放和原始记录被直接当密钥使用的风险，**不能**
> 消除离线猜测或 `config.ini` 泄露等价于凭据这两项残余风险；详见 [SECURITY.md](../../SECURITY.md)。

两端必须为同一版本。旧版（v2 的 48/36 字节握手）会在授权之前收到 `status=1`，不会被当成可绕过授权的客户端；更旧的 16 字节握手会被识别为 legacy peer 并给出明确报错，不会再黑屏。

### 3 — 虚拟显示器生命周期（扩展模式）

```
选择扩展模式
  → vddStartup()：
      保存当前非 MTT 显示路径、源、复制组与模式基线
      → 启用 Root\MttVDD 设备（若被禁用；SetupAPI，需管理员）
      → 驱动仅启动时读取 vdd_settings.xml；目标尺寸缺失时默认追加
        精确模式（`vdd_mode_write=never` 可关闭；先备份 .mivara.bak，
        回读校验），必要时重启设备一次
      → 移除驱动自动建立的 MTT 复制路径，恢复原有显示布局
      → 从实际 CCD 路径选择未占用的源并分配独立复制组
      → 定位到现有桌面右侧；优先使用接收端分辨率，否则用最接近的比例
      → 回读独立拓扑、实际 GDI 设备名与原布局，再锁定采集源
  → 握手、推流（窗口/鼠标可穿越到虚拟屏）
  → 会话结束 / Ctrl+C / 断开连接 / 关闭控制台窗口
  → vddShutdown()：仅拆除 MTT 路径 + 禁用 Root\MttVDD 设备 + 恢复原布局
      → 虚拟屏从「设置 → 屏幕」彻底消失（不是仅从桌面分离）
```

实现于 [vdd.cpp](https://github.com/wzy-777/Mivara/blob/main/src/vdd.cpp) / [vdd.h](https://github.com/wzy-777/Mivara/blob/main/src/vdd.h)（sender 与 GUI 共用），基于
Windows CCD API（`QueryDisplayConfig` / `SetDisplayConfig`）、
`ChangeDisplaySettingsEx` 与 SetupAPI 设备启停（`DIF_PROPERTYCHANGE`）。
启用驱动时，Windows 可能自动把 MTT 屏幕复制到物理显示源；这并不等于独立扩展。代码从真实 CCD 路径分配未占用的源与独立复制组，不使用全局 `SDC_TOPOLOGY_EXTEND`，也不把物理屏与 MTT 的复制拓扑当成采集目标。分辨率和位置设置后必须回读确认，正常停止保留当时的非 MTT 布局，启动失败则恢复启动前基线；这些临时布局不写入显示配置数据库。

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
| GUI「停止推流」 | 通过 stdin 发送 `STOP|mivara` 优雅停止（最长等 8 秒）；无管道的可见控制台使用 Ctrl+Break，若被强杀则 GUI 调 `vddForceDetach()` 兜底 |

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

> 安全说明：8082 与 8083 都需要本次已授权 8081 会话的 32 字节令牌：连接时出示一次，并在**每个请求前**重新校验（包括 8083 的端口列举与结束进程之前）。会话结束即清空令牌，因此已建立的旁路连接会在下一次请求时失效，携带旧令牌的新连接会被拒绝。它们仍**不加密**，能窃听链路的设备仍可读取查询结果或终止进程的请求，因此请只在可信局域网内使用。

### 6 — 拼接通道（软件 KVM，8084）

拼接**不是显示模式**：两端都保留自己的物理显示器和桌面，只有键盘/鼠标事件经 TCP 8084 传输，因此完全不走采集、编码、虚拟显示器与 8081 视频协议（`HsRequestV2.mode` 仍只有 0/1，拼接值不会出现在 8081 上）。实现位于 `src/kvm.cpp` / `src/kvm.h`。

**握手（全部为网络字节序的 `uint32`）**

```
控制器 → 目标   KvmHello（16 字节：magic 'MVK1'、版本 2、保留；reserved[0] = 对方所在侧）

之后按接收端的「接收与安全」策略分支：

  open     目标 → 控制器   KvmReply（32 字节：magic、版本、状态、目标虚拟桌面 x/y/w/h、保留）

  password / manual
           目标 → 控制器   HsAuthStart（64 字节，与 8081 共用）
           控制器 → 目标   HsRequestExtV3（48 字节，含发送端主机名）
           控制器 → 目标   HsAuthProof（40 字节，仅密码模式）
           目标 → 控制器   HsResponseV2（36 字节，状态见下）
           目标 → 控制器   KvmReply（32 字节，仅 HS_OK，后接事件流）

控制器 → 目标   KvmEvent（16 字节：type、a、b、c）× N
```

`KvmReply` 状态：`0`=同意，`1`=版本不符，`2`=对方拒绝，`3`=占用。`HsResponseV2` 状态沿用 8081（`0`=OK、`7`=密码错误/节流、`8`=授权超时、`9`=被拒绝、`10`=对方密码记录损坏）。事件类型：`1`=移动（目标虚拟桌面坐标）、`2`=按键（1/2/3/4/5，b=按下）、`3`=滚轮（a=有符号增量，b=是否横向）、`4`=键盘（a=按下，b=VK，c=扫描码 | 扩展位<<16）、`5`=全部释放。

授权证明绑定本次连接并使用拼接用途（`purpose=2`）：`HMAC-SHA256(purpose_key, "Mivara/v1/auth-proof" ‖ 2 ‖ nonce ‖ KvmHello ‖ HsRequestExtV3)`，nonce 每次新生成，因此录制下来的应答无法重放。8081 与 8084 共享同一个活动会话槽：任一端已有会话时，另一端的连接在授权前就会收到 `HS_ERR_BUSY`。手动授权在两个端口间串行；只有明确拒绝才写 60 秒冷却，超时、对端断开或弹窗失败都会立即释放。hello / identity / proof 分别为 3 / 10 / 125 秒（与控制器 120 秒的密码输入时限对齐），总预算 140 秒，未完成的握手不会长期占住端口。控制器读取回复的前 4 字节区分三种情况（`MVK1` = 直接应答、`TVHA` = 授权挑战、`TVHS` = 授权前的拒绝）；版本 1 的旧控制器会收到 `KVM_ERR_VERSION`，无法绕过策略。`KvmHello.reserved[0]` 携带**控制器视角下目标的所在侧**（1=左、2=右、3=上、4=下；0 或其他值＝未指定，按右侧处理），仅用于让授权弹窗写清进入边。方向本身完全是控制器侧状态——目标只注入收到的绝对坐标，因此会话中切换方向不需要任何协议消息。

**控制器侧（sender --mode stitch）**

- 低层钩子 `WH_MOUSE_LL` / `WH_KEYBOARD_LL` 捕获本机键鼠；钩子回调**绝不做网络 I/O**（超过系统 `LowLevelHooksTimeout` 会被静默卸载），只把事件压入无锁 SPSC 环形队列，由独立线程发送。
- 光标被系统钳制在虚拟桌面边缘时，钩子的坐标不再变化；因此用 `RIDEV_INPUTSINK` Raw Input 的设备增量判断"继续朝所选方向推"并驱动远端光标。越过所选边缘 3 像素即进入远程状态，本地输入被吞掉；朝其他方向移动会清零累计值。
- 远程时把对方光标移出**进入时那条边**，或按 `Ctrl+Alt+Shift+Q`，即交还控制；进入/离开时重新同步物理按住的修饰键与鼠标键，避免对端卡键。四条边的对应关系：右侧→本机右缘出、对方左缘入/出；左侧→本机左缘出、对方右缘入/出；上方→本机上缘出、对方下缘入/出；下方→本机下缘出、对方上缘入/出。
- 方向由 `--stitch-side left|right|top|bottom` 指定（默认 `right`）；GUI 在启动前弹窗选择并写入 `config.ini` 的 `stitch_side`，下次作为默认。会话中切换方向走 GUI→sender 的 stdin 行 `STITCH|side=…`：stdin 线程调用 `kvmSetControllerSide()` 排队，消息循环在 50 ms 定时器里应用——若此刻正在控制对方，会先交还并释放按键，再挂上新边。
- 连接失败、握手失败、对方拒绝、连接断开都会输出对应 `STAT|event=error`；事件状态变化输出 `STAT|event=stitch|state=local|remote|waiting|…|side=…`，GUI 状态面板据此显示当前方向并提供四个切换选项。
- 非 Windows 平台直接返回错误（拼接暂只支持 Windows）。

**目标侧（receiver 的 8084 服务）**

- 独立线程监听 8084，与视频监听互不影响；空闲与会话中都会轮询停止标志，所有退出路径都会 join。
- 会话在独立线程中执行，监听线程始终保持响应：会话进行中第二个控制端会立即收到 `KVM_ERR_BUSY`（先读取其 hello 再回复，避免关闭未读完的连接触发 RST）。控制器 drain 线程同时检查 EOF/错误，不让对端退出在空闲时无限漏检。两端以 `KvmHello.reserved[1]` / `KvmReply.reserved` 的 `KVM_FEATURE_HEARTBEAT` 协商心跳；都支持时每 3 秒双向 PING/PONG，约 9 秒未收到数据即断开并释放按键。旧端不协商心跳，回退为 TCP 保活（空闲 15 s、间隔 3 s）。本地停止先 `shutdown` 唤醒发送，目标收到 EOF 后释放输入，不再重复阻塞发送。
- 收到合法 `KvmHello` 后**先执行接收策略**（不再单独弹一次「拼接确认」）：`open` 直接放行；`password` 校验 HMAC 证明（失败按来源节流）；`manual` 复用 8081 的审批流程——GUI 模式下由 app 弹窗（显示设备名、IP 与「请求：拼接控制」，默认拒绝，30 秒超时，重复请求合并），单独运行 `bin/receiver` 时回退为置顶原生确认框（默认「否」，30 秒自动拒绝，进程退出时自动关闭，等待期间 `select` 连接以便控制端断开时立即释放）。授权通过后才返回目标虚拟桌面尺寸并开始注入；明确拒绝后 60 秒内同一来源直接拒绝。密码记录缺失或损坏时回 `HS_ERR_AUTH_CONFIG`（状态 10），绝不降级为任意连接。版本 1 的控制端收到 `KVM_ERR_VERSION`。
- 事件用 `SendInput` 注入：鼠标为绝对坐标 + `MOUSEEVENTF_VIRTUALDESK`（0..65535 归一化），键盘优先用扫描码（`KEYEVENTF_SCANCODE`，必要时加扩展位）。会话结束、断开或进程正常退出都会对已按下的每个键/鼠标键发送一次释放（进程被强制终止时来不及执行，这是 GUI 默认隐藏控制台启动下「停止接收」的已知限制）。
- 注入在会话线程内串行执行；`SendInput` 不能跨 UIPI 注入**更高**完整性级别的窗口，也不会作用于安全桌面（因此无法模拟 Ctrl+Alt+Del）。单独运行的 `receiver` 是普通权限，管理员窗口收不到拼接输入；经 GUI 启动的接收端继承 GUI 的管理员权限，因此可以输入到管理员窗口。

> 安全提示：8084 与 8081 使用同一授权策略（任意 / 密码 / 手动），但通道**没有加密**——授权只决定谁能接入，不保护键鼠事件内容。请只在可信局域网内使用。

### 7 — 拼接文件投放（8085，可选）

文件投放是拼接会话的伴随通道，**不是新的显示模式，也不是新的会话**：它不占用 8081/8084 的活动会话槽，不额外弹授权框，默认在两端关闭。控制器开启 `--file-drop 1` 且 Medium 完整性的 `stitchcap.exe` 就绪时，在 `KvmHello.reserved[1]` 置 `KVM_FEATURE_FILE_DROP`；接收端仅在 `file_drop_accept=1`、8085 监听正常且授权成功时回显该位，并在 `KvmReply` 之后发送「1 个标记 + 8 个固定 `KvmEvent`」承载 32 字节一次性令牌。控制器在启动 `drainLoop()` 之前按固定长度读完，因此 16 字节事件流不会失步。8085 是独立 TCP 服务：`DropHello` 携带令牌、传输 id、松手坐标与数量/字节上限；之后是 manifest、分块、逐文件 SHA-256 与结果帧。接收端把文件写入 `%LOCALAPPDATA%\Mivara\Drop\<transfer>`，校验通过后复制到松手位置所在的资源管理器/桌面文件夹；只复制，绝不覆盖、移动或删除源文件。发送端的边缘捕获由 `stitchcap.exe` 完成：它是 asInvoker 进程（用 shell 令牌启动，Medium 完整性），在共享边放一条 3 像素置顶窗口，`DragEnter` 读取 `CF_HDROP` 后经命名管道上报，并返回 `DROPEFFECT_NONE`，因此本机不会发生复制。Medium 是硬要求：High 窗口收不到 Medium Explorer 的 OLE drop（UIPI）。

## 协议速查

| 项目 | 值 |
|------|-----|
| 握手魔数 | `0x54564853`（`'TVHS'`） |
| 协议版本 | `3` |
| 握手请求 `HsRequestV2` | 48 字节（12 × `uint32`） |
| 授权挑战 `HsAuthStart` | 64 字节（模式、KDF 参数、salt、nonce） |
| 身份扩展 `HsRequestExtV3` | 48 字节（发送端主机名） |
| 密码证明 `HsAuthProof` | 40 字节（HMAC-SHA256） |
| 握手响应 `HsResponseV2` | 36 字节 |
| 会话令牌 `HsResponseExtV3` | 44 字节（32 字节令牌，仅 OK） |
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
| `status` | `0`=OK　`1`=版本　`2`=编码　`3`=尺寸　`4`=解码器　`5`=旧协议　`6`=忙　`7`=密码/节流　`8`=授权超时　`9`=被拒绝　`10`=授权配置损坏 |
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
