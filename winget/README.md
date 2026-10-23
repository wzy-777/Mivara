# winget 上架清单

本目录存放提交到 [microsoft/winget-pkgs](https://github.com/microsoft/winget-pkgs) 的清单文件（YAML）。

**为什么值得上架**：winget 下载安装不经过浏览器，且默认社区源会把下载的文件标记为「受信区」（Zone 2），因此 **winget 用户的 SmartScreen 下载/运行警告都不会触发**——这是不做代码签名的情况下唯一能做到这点的分发渠道。

## 自动提交（推荐）

`release.sh` 在 GitHub Release 创建后自动调用 `winget/submit.sh <ver>`；也可以单独运行：

```bash
./winget/submit.sh 0.10.0            # 创建或更新 PR
./winget/submit.sh 0.10.0 --dry-run  # 只打印计划，不写任何东西
```

脚本固化了几条规则：

- **一个包只有一个 PR**：若当前 gh 账号已有该包的 open PR，就在它自己的分支上原地更新，不再开第二个；
- **新包 PR 只能带一个版本**：分支里未发布的旧版本目录会被删除；上游已发布的版本绝不动；
- 幂等：清单内容一致且没有多余版本时不产生提交、不重复留言；
- 不克隆 winget-pkgs（约百万文件），全部通过 Git Data API 写入 `wzy-777/winget-pkgs` fork；
- 若 fork 不存在会自动创建；提交前若本机有 `winget` CLI 会先跑 `winget validate`。

## 手动流程（备用）

1. Fork https://github.com/microsoft/winget-pkgs；
2. 在 fork 里按路径 `manifests/w/wzy-777/Mivara/<version>/` 放入本目录三个 YAML；
3. 向 `microsoft/winget-pkgs` 的 `master` 提 PR（新包标题 `New package: wzy-777.Mivara version <ver>`，更新标题 `Update: wzy-777.Mivara version <ver>`）。自动校验检查清单格式、URL、SHA-256 与静默安装；通过后由版主合并。

建议 PR 描述（英文参考，脚本用的就是这段）：

> Mivara (幻屏) is an MIT-licensed open-source, cross-platform LAN screen-casting / wireless second-monitor tool by wzy-777. The installer is built with MinGW + Inno Setup from the linked repository; it is not code-signed. Silent install (`/VERYSILENT`) is supported. Installer SHA-256 matches the GitHub release asset digest.

版本目录由 release.sh 生成，其中 `InstallerUrl` **用带版本号的资产名**（不要用固定名 `Mivara-Setup.exe`），`InstallerSha256` 与 GitHub 上传后的 asset digest 一致。

## 备注

- `AppsAndFeaturesEntries` 的 `DisplayName`/`ProductCode` 取自 Inno 写入注册表的实际值（`{AppId}_is1`）；如校验器提示不匹配，以注册表实际值为准修正。
- 若校验器提示 `ManifestVersion` 需上调（winget-pkgs 会随时间提升 schema 版本），把三个文件的 `ManifestVersion` 与文件头 aka.ms 链接同步改成校验器要求的版本。
- 若提示 URL 信誉问题（URL-Validation-Error）：属未签名新文件的正常现象，在 PR 中说明为开源项目即可，非硬性门槛。
