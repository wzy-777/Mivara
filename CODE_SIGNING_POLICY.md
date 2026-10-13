# Code signing policy / 代码签名声明

Free code signing provided by [SignPath.io](https://signpath.io), certificate by [SignPath Foundation](https://signpath.org).

## Scope / 范围

This policy covers the Windows release artifacts of **Mivara (幻屏)** published on the [GitHub Releases](https://github.com/wzy-777/Mivara/releases) page — currently the installer `Mivara-Setup-<version>.exe` (and, where applicable, the executables it contains).

本声明适用于本项目在 GitHub Releases 发布的 Windows 产物：安装包 `Mivara-Setup-<版本>.exe`（以及其中包含的可执行文件，如适用）。

## Roles / 角色

- **Authors / Reviewers / Approvers**: wzy-777 (sole maintainer; holds all three roles). / 唯一维护者 wzy-777 兼任全部角色。
- Multi-factor authentication is enabled on both the GitHub and SignPath accounts. / GitHub 与 SignPath 账号均已启用多因素认证。

## Rules / 规则

- We sign **only** binaries that we build ourselves from this repository, in our CI (`.github/workflows/release-installer.yml`). Builds from unmerged or third-party sources are never signed. / 仅签名由本项目 CI 从本仓库源码构建的产物；不签名任何第三方或未合并来源的二进制。
- Every signing request is manually reviewed and approved before the certificate is applied. / 每次签名请求均经人工审核批准后才会执行。
- The signing certificate is held by SignPath Foundation (the private key lives in SignPath's HSM); no personal information is embedded in signed binaries. / 签名证书由 SignPath Foundation 持有（私钥存于 SignPath 的 HSM）；签名产物不包含任何个人信息。

Questions or corrections: please open an [issue](https://github.com/wzy-777/Mivara/issues).
