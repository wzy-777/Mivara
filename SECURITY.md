# Security Policy / 安全策略

Mivara (幻屏) is a LAN-only screen-mirroring tool. This document describes how to report a vulnerability, what is and is not considered a vulnerability, and the security model of the shipped services.

Mivara（幻屏）是一个仅限局域网使用的投屏工具。本文说明如何报告安全问题、哪些属于/不属于安全漏洞，以及各服务的安全模型。

## Supported Versions / 支持版本

Security fixes are provided for the **0.11.x** release line only. Older releases receive no fixes; upgrade to the latest release before reporting an issue.

仅对 **0.11.x** 版本线提供安全修复。更早的版本不再修复；报告问题前请先升级到最新版本。

## Reporting a Vulnerability / 报告漏洞

Please use **GitHub private vulnerability reporting** (Security Advisories) at <https://github.com/wzy-777/Mivara/security/advisories/new>. This keeps the report private until a fix is available.

请优先使用 GitHub 的**私密漏洞报告**（Security Advisories）：<https://github.com/wzy-777/Mivara/security/advisories/new>。修复发布前，报告内容保持私密。

If private reporting is unavailable, open a regular [issue](https://github.com/wzy-777/Mivara/issues) that says a private channel is needed — **without exploit details, steps to reproduce, or proof-of-concept code**.

如果无法使用私密报告，请开一个普通 [issue](https://github.com/wzy-777/Mivara/issues) 说明需要私密沟通渠道即可——**不要在 issue 中包含利用细节、复现步骤或 PoC 代码**。

- We aim to acknowledge a report within **7 days**.
- We aim to fix or publicly disclose the issue within **90 days** of acknowledgement.
- Please do **not** publish details before a fix or coordinated disclosure date is agreed.
- 我们力争在 **7 天内**确认收到报告。
- 我们力争在确认后 **90 天内**完成修复或公开披露。
- 在修复完成或双方约定的披露日期之前，请**不要**公开细节。

## Threat Model / 威胁模型

The following are **design limitations of a LAN-only tool, not vulnerabilities by themselves**. Reports that depend on them being different are out of scope unless they demonstrate a concrete impact beyond what is described here.

以下是这类仅限局域网工具**固有的设计取舍，本身不构成安全漏洞**。除非能证明超出下述范围的具体影响，否则仅依赖这些取舍的报告不在受理范围内。

- **No transport encryption.** TCP 8081 (video + authorization), 8082 (RLE compute offload), 8083 (port inspector) and 8084 (stitch KVM) are plaintext, and the sender does not verify the receiver's identity. SSDP discovery uses unauthenticated UDP 1900. Anyone who can reach these ports on the LAN can talk to them and can read the traffic.
- **无传输加密。** TCP 8081（视频 + 授权）、8082（RLE 计算卸载）、8083（端口检查）与 8084（拼接 KVM）均为明文，且发送端不验证接收端身份；SSDP 发现使用无认证的 UDP 1900。局域网内任何能访问这些端口的设备都可以与之通信并读取流量。
- **8081 admission control is receiver-side only.** The receiver applies open / password / manual approval; a new install defaults to manual approval, and "allow any connection" is an explicit opt-in. This decides *who may start a session*; it does not encrypt the picture, and it does not authenticate the receiver to the sender.
- **8081 的准入控制仅在接收端。** 接收端执行「任意 / 密码 / 手动授权」策略，新装默认手动授权，「允许任意连接」需显式选择。它只决定**谁可以建立会话**，不加密画面，也不向发送端证明接收端身份。
- **The connection password is a short admission secret, not a strong key.** It is 4–8 printable ASCII characters and only meant to gate a trusted LAN. Letters are matched case-insensitively: A–Z are folded to a–z before PBKDF2, which lowers each letter from about 5.7 to 4.7 bits and shrinks an 8-character password's search space by up to 256×. The receiver stores a PBKDF2 record and never receives the plaintext; the proof is domain-separated per port (8081 vs 8084), binds a fresh nonce and the exact request bytes, and new records use 300,000 PBKDF2 iterations (existing records keep the count they were created with). Online guessing is throttled per source across both ports. Two limitations remain by design: a device that captures a challenge and proof can brute-force a short password offline, and a leaked `config.ini` is credential-equivalent — a symmetric verifier cannot prevent either. Moving to an audited aPAKE is the long-term fix; this release does not claim one.
- **连接密码是短准入口令，不是强密钥。** 密码为 4–8 个可打印 ASCII 字符，仅用于可信局域网内的准入。字母不区分大小写：PBKDF2 前会把 A–Z 折叠为 a–z，每个字母的信息量约从 5.7 bit 降到 4.7 bit，8 字符密码的搜索空间最多缩小 256 倍。接收端只保存 PBKDF2 记录、永不接触明文；证明按端口用途分离（8081 与 8084 不同），绑定每次新生成的随机数与完整请求字节；新设置的密码使用 300000 次 PBKDF2 迭代（旧记录保留创建时的次数）。在线猜测在 8081/8084 间共享按来源节流。仍有两项设计内限制：截获挑战与证明的设备可离线暴力破解短密码；`config.ini` 泄露等价于凭据泄露——对称校验记录无法避免这两点。长期方案是迁移到经过审计的 aPAKE，本版本不作此宣称。
- **8082/8083 token gate.** Both services require the 32-byte token of the current authorized 8081 session, read it on connect and **re-check it before every request** (including before 8083's port listing and kill). The token is cleared when the session ends, so an already-open sidecar connection loses access on its next request and a new connection with the old token is refused. The token travels in the clear, so a LAN peer that can sniff the live session can replay it while that session lives. Neither service is encrypted.
- **8082/8083 的令牌门。** 两个服务都要求当前已授权 8081 会话的 32 字节令牌：建连时读取一次，并在**每个请求前重新校验**（包括 8083 的端口列举与结束进程之前）。会话结束即清空令牌，因此已建立的旁路连接会在下一次请求时失去权限，携带旧令牌的新连接会被拒绝。令牌明文传输，能窃听该会话的局域网设备可在会话存续期间重放。两者均不加密。
- **8084 injects keyboard and mouse input.** It obeys the same 8081 policy (open / password / manual; manual by default) and adds no second confirmation. 8081 and 8084 share one active-session slot: while either port has a session, the other answers BUSY, and an unauthenticated handshake never holds the slot. Manual approval is serialized across both ports (one prompt at a time; a second device gets BUSY). Under manual approval the target shows a prompt (default deny, 30 s timeout). Only an explicit refusal suppresses prompts from that source IP for 60 s — a timeout, peer disconnect, prompt failure or a late allow after the deadline releases the slot without arming the cooldown. The channel is unencrypted, so authorization is the only gate.
- **8084 注入键盘与鼠标输入。** 它与 8081 使用同一策略（任意 / 密码 / 手动，默认手动），不再额外增加一次确认。8081 与 8084 共用一个活动会话槽：任一端有会话时，另一端返回 BUSY；未认证的握手不会占住该槽。手动授权在两个端口间串行（同一时刻只有一个弹窗；第二个设备返回 BUSY）。手动授权时接收端弹窗（默认拒绝，30 秒超时）。只有**明确拒绝**才会在 60 秒内抑制该源 IP 的弹窗——超时、对端断开、弹窗失败或超过截止时间的“允许”都会直接释放槽位，不写冷却。通道不加密，因此授权就是唯一关卡。
- **Elevation.** `sender` and `app` embed a `requireAdministrator` manifest. A `receiver` launched by the GUI inherits that elevated token and can inject into elevated windows; a standalone `receiver.exe` is unelevated, and UIPI blocks injection into higher-integrity windows.
- **权限提升。** `sender` 与 `app` 内嵌 `requireAdministrator` 清单。由 GUI 启动的 `receiver` 会继承该管理员令牌，因而能向高权限窗口注入输入；独立运行的 `receiver.exe` 未被提升，受 UIPI 限制无法向更高完整性级别的窗口注入。
- **Virtual display (VDD) settings file.** In extend mode the sender appends a missing receiver resolution to `vdd_settings.xml` by default, using a backup plus validated replace, and it refuses to modify the file while any VDD CCD path is active. Set `vdd_mode_write=never` in `config.ini` to forbid the write entirely (the GUI does not offer this toggle).
- **虚拟显示器（VDD）配置文件。** 扩展模式下，发送端默认会把缺失的接收端分辨率追加到 `vdd_settings.xml`（写入前先备份、写入后校验），且在任何 VDD CCD 路径处于活动状态时拒绝修改。可在 `config.ini` 设置 `vdd_mode_write=never` 完全禁止写入（GUI 不提供该开关）。
- **Updater.** The in-app updater contacts only `github.com/wzy-777/Mivara` over HTTPS, restricts redirects to `github.com` / `*.githubusercontent.com`, validates the tag against a fixed charset, and is fail-closed when `SHA256SUMS.txt` lacks an entry for the installer.
- **更新器。** 应用内更新只通过 HTTPS 访问 `github.com/wzy-777/Mivara`，重定向仅允许落到 `github.com` / `*.githubusercontent.com`，并按固定字符集校验 tag；若 `SHA256SUMS.txt` 中没有安装包的条目，安装会失败关闭（fail-closed）。

## Out of Scope / 不在受理范围

- Using a documented plaintext service, or the explicit "allow any connection" policy, on a trusted LAN (see Threat Model above).
- Resource-exhaustion denial of service.
- Attacks requiring physical access to a device.
- Issues in unsupported versions.
- Social engineering.
- Issues that require an already-compromised host.
- 在可信局域网内使用文档中已说明的明文服务，或显式选择「允许任意连接」策略（见上文威胁模型）。
- 资源耗尽型拒绝服务。
- 需要物理接触设备的攻击。
- 不受支持版本中的问题。
- 社会工程学攻击。
- 需要主机已被攻陷才能成立的问题。

## Safe Harbor / 安全港

Good-faith security research is welcome. We will not pursue or support legal action against researchers who avoid privacy violations, data destruction and disruption of services, and who follow the reporting process above.

我们欢迎善意的安全研究。对于避免侵犯隐私、破坏数据、干扰服务，并按上述流程报告的研究者，我们不会追究或支持任何法律行动。

## A Note on Release Artifacts / 关于发布产物

The Windows installer is **not code-signed yet**. Before running a downloaded installer, verify its SHA-256 against the published `SHA256SUMS.txt` (see [CODE_SIGNING_POLICY.md](CODE_SIGNING_POLICY.md) for the signing plan).

Windows 安装包**目前尚未进行代码签名**。运行下载的安装包前，请对照发布的 `SHA256SUMS.txt` 校验其 SHA-256（签名计划见 [CODE_SIGNING_POLICY.md](CODE_SIGNING_POLICY.md)）。
