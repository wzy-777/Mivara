# winget 上架清单

本目录存放提交到 [microsoft/winget-pkgs](https://github.com/microsoft/winget-pkgs) 的清单文件（YAML）。

**为什么值得上架**：winget 下载安装不经过浏览器，且默认社区源会把下载的文件标记为「受信区」（Zone 2），因此 **winget 用户的 SmartScreen 下载/运行警告都不会触发**——这是不做代码签名的情况下唯一能做到这点的分发渠道。

## 首次提交（0.9.1）

1. 打开 https://github.com/microsoft/winget-pkgs 并 Fork；
2. 在自己的 fork 中按路径 `manifests/w/wzy-777/Mivara/0.9.1/` 新建目录，把本目录三个 YAML 原样上传（网页端 Add file → Upload files 即可）；
3. 向 `microsoft/winget-pkgs` 的 `master` 提 Pull Request（标题形如 `New package: wzy-777.Mivara version 0.9.1`）。自动校验会检查清单格式、URL、SHA-256 与静默安装；通过后由人工审核合并。

建议 PR 描述（英文参考）：

> Mivara (幻屏) is an MIT-licensed open-source, cross-platform LAN screen-casting / wireless second-monitor tool by wzy-777. The installer is built with MinGW + Inno Setup from the linked repository; it is not code-signed. Silent install (`/VERYSILENT`) is supported. Installer SHA-256 matches the GitHub release asset digest.

## 每次发新版本

1. 复制 `0.9.1/` 为新版本号目录，更新：
   - 三个文件的 `PackageVersion`
   - `installer.yaml` 的 `InstallerUrl`（**用带版本号的资产名**，不要用固定名 `Mivara-Setup.exe`）与 `InstallerSha256`（`Get-FileHash dist\Mivara-Setup-<ver>.exe -Algorithm SHA256`，与 GitHub 上传后的 asset digest 一致）、`ReleaseDate`
   - `installer.yaml` 中 `AppsAndFeaturesEntries.DisplayName` 的版本号；locale 的 `ReleaseNotesUrl`
2. 同样方式提 PR；或安装 [wingetcreate](https://github.com/microsoft/winget-cli/tree/master/src/WingetCreate)，用 `wingetcreate update wzy-777.Mivara --version <ver> --urls <url> --submit` 自动生成并提交。

## 备注

- `AppsAndFeaturesEntries` 的 `DisplayName`/`ProductCode` 取自 Inno 写入注册表的实际值（`{AppId}_is1`）；如校验器提示不匹配，以注册表实际值为准修正。
- 若校验器提示 `ManifestVersion` 需上调（winget-pkgs 会随时间提升 schema 版本），把三个文件的 `ManifestVersion` 与文件头 aka.ms 链接同步改成校验器要求的版本。
- 若提示 URL 信誉问题（URL-Validation-Error）：属未签名新文件的正常现象，在 PR 中说明为开源项目即可，非硬性门槛。
