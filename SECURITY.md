# Security Policy / 安全策略

Mivara (幻屏) is a LAN-only screen-mirroring tool. This document describes how to report a vulnerability, what is and is not considered a vulnerability, and the security model of the shipped services.

Mivara（幻屏）是一个仅限局域网使用的投屏工具。本文说明如何报告安全问题、哪些属于/不属于安全漏洞，以及各服务的安全模型。

## Supported Versions / 支持版本

Security fixes are provided for the **0.10.x** release line only. Older releases receive no fixes; upgrade to the latest release before reporting an issue.

仅对 **0.10.x** 版本线提供安全修复。更早的版本不再修复；报告问题前请先升级到最新版本。

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

- **No transport encryption or peer authentication.** TCP 8081 (video), 8082 (RLE compute offload), 8083 (port inspector) and 8084 (stitch KVM) are plaintext and unauthenticated. SSDP discovery uses unauthenticated UDP 1900. Anyone who can reach these ports on the LAN can talk to them.
- **无传输加密、无对端认证。** TCP 8081（视频）、8082（RLE 计算卸载）、8083（端口检查）与 8084（拼接 KVM）均为明文且不验证对端身份；SSDP 发现使用无认证的 UDP 1900。局域网内任何能访问这些端口的设备都可以与之通信。
- **8081 has no pairing or local consent.** Any reachable peer can start a video session and supply fullscreen content; only use the receiver on a trusted private LAN.
- **8081 没有配对或本机确认。** 任何可达对端都能建立视频会话并投放全屏内容；请只在可信的专用局域网内启动接收端。
- **8083 accepts LIST/GET/KILL from any reachable peer** and can terminate receiver-side processes. Keep it on a trusted LAN only.
- **8083 接受任何可达对端的 LIST/GET/KILL**，可终止接收端进程。请仅在可信局域网内使用。
- **8084 injects keyboard and mouse input.** The target shows a per-session topmost consent dialog (default No, 30 s auto-dismiss); an explicit refusal suppresses prompts from that source IP for 60 s. There is no encryption and no peer authentication, so this human confirmation is the only gate.
- **8084 注入键盘与鼠标输入。** 接收端每次会话都会弹出置顶的人工确认框（默认「否」，30 秒自动关闭）；明确拒绝后 60 秒内不再对该源 IP 弹窗。由于没有加密也没有对端认证，这次人工确认就是唯一的关卡。
- **Elevation.** `sender` and `app` embed a `requireAdministrator` manifest. A `receiver` launched by the GUI inherits that elevated token and can inject into elevated windows; a standalone `receiver.exe` is unelevated, and UIPI blocks injection into higher-integrity windows.
- **权限提升。** `sender` 与 `app` 内嵌 `requireAdministrator` 清单。由 GUI 启动的 `receiver` 会继承该管理员令牌，因而能向高权限窗口注入输入；独立运行的 `receiver.exe` 未被提升，受 UIPI 限制无法向更高完整性级别的窗口注入。
- **Virtual display (VDD) settings file.** In extend mode the sender appends a missing receiver resolution to `vdd_settings.xml` by default, using a backup plus validated replace, and it refuses to modify the file while any VDD CCD path is active. Set `vdd_mode_write=never` in `config.ini` to forbid the write entirely (the GUI does not offer this toggle).
- **虚拟显示器（VDD）配置文件。** 扩展模式下，发送端默认会把缺失的接收端分辨率追加到 `vdd_settings.xml`（写入前先备份、写入后校验），且在任何 VDD CCD 路径处于活动状态时拒绝修改。可在 `config.ini` 设置 `vdd_mode_write=never` 完全禁止写入（GUI 不提供该开关）。
- **Updater.** The in-app updater contacts only `github.com/wzy-777/Mivara` over HTTPS, restricts redirects to `github.com` / `*.githubusercontent.com`, validates the tag against a fixed charset, and is fail-closed when `SHA256SUMS.txt` lacks an entry for the installer.
- **更新器。** 应用内更新只通过 HTTPS 访问 `github.com/wzy-777/Mivara`，重定向仅允许落到 `github.com` / `*.githubusercontent.com`，并按固定字符集校验 tag；若 `SHA256SUMS.txt` 中没有安装包的条目，安装会失败关闭（fail-closed）。

## Out of Scope / 不在受理范围

- Using a documented unauthenticated service on a trusted LAN (see Threat Model above).
- Resource-exhaustion denial of service.
- Attacks requiring physical access to a device.
- Issues in unsupported versions.
- Social engineering.
- Issues that require an already-compromised host.
- 在可信局域网内使用文档中已说明的无认证服务（见上文威胁模型）。
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
