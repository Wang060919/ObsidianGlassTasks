<h1 align="center">Obsidian Glass Tasks</h1>

<p align="center">把 Obsidian 待办放到 Windows 桌面，让它融入你的壁纸。</p>

<p align="center">Windows 11 · Markdown 待办 · 液态玻璃效果</p>

![月夜花枝背景上的玻璃待办演示](docs/assets/showcase.gif)

<p align="center"><sub>6 秒循环演示：使用项目玻璃着色器和示例待办生成，并非实际桌面录屏。</sub></p>

## 它能做什么？

Obsidian Glass Tasks 是一个 Windows 桌面浮窗。它读取你指定的一篇 Markdown 笔记，把未完成的复选框任务显示在桌面上；在浮窗里勾选后，任务状态会写回原笔记。

- **桌面玻璃效果**：背景随桌面变化，带轻模糊、圆角折射和边缘高光。
- **与笔记同步**：约每两秒读取一次笔记，也可以手动刷新。
- **完成与撤销**：在桌面勾选任务，必要时撤销最近一次完成。
- **Markdown 与双链**：展示强调、代码、链接等行内格式，点击 Obsidian 双链跳转到对应笔记。
- **按自己的方式摆放**：拖动窗口、选择置顶，或暂停动态背景后截图。

浮窗使用固定暗色界面，背景由 Windows 本机采集并渲染，不会上传。正常运行时，浮窗本身会从系统捕获结果中排除，避免画面反复采样自身。

## 先了解这些

目前需要从源码构建，版本为 **0.1.8**。程序只读取配置中的**一篇笔记**，不执行 Obsidian Tasks 插件的全库查询，也不会生成循环任务或自动添加完成日期。它可以显示该笔记中的标准 Markdown 待办，例如：

```markdown
- [ ] 整理本周的工作计划
- [ ] 阅读 [[项目笔记]] #todo
- [x] 已完成的任务
```

无需给任务添加特定标签；已完成的任务不会显示在浮窗里。勾选时只修改对应复选框的状态，保留文件原有的 UTF-8 BOM、换行和其他内容。若原行发生变化或匹配不明确，程序会提示错误。

标题 `Inbox` 使用随项目提供的 Parisienne 花体。底部状态使用英文斜体衬线字，优先使用电脑上已安装的 Anthropic Serif，并在缺少该字体时回退到 Georgia；项目不包含 Anthropic 字体文件。

## 环境要求

**运行：**

- Windows 11 x64，以及支持 D3D11 的显卡。
- Microsoft Edge WebView2 Runtime。
- PowerShell 7.4 或更高版本，用于写回笔记。

**从源码构建：**还需要 Git、Visual Studio 2022 C++ Build Tools、Windows SDK 10.0.26100.0，以及下面步骤中下载的 WebView2 SDK 1.0.4258.31。运行 Markdown 测试另需 Node.js 18 或更高版本。

## 从源码安装

以下命令在 PowerShell 7 中执行。

### 1. 获取项目并配置笔记

```powershell
git clone https://github.com/Wang060919/ObsidianGlassTasks.git
Set-Location ObsidianGlassTasks
Copy-Item -LiteralPath .\resources\config.example.json -Destination .\resources\config.json
notepad .\resources\config.json
```

将配置改成自己的笔记库和文件路径。下面是格式示例，路径需要替换：

```json
{
  "vault": "MyVault",
  "sourcePath": "C:\\Notes\\MyVault\\Inbox\\Tasks.md",
  "noteFile": "Inbox/Tasks.md",
  "powershellPath": "C:\\Program Files\\PowerShell\\7\\pwsh.exe"
}
```

| 配置项 | 用途 |
| --- | --- |
| `vault` | Obsidian 中的笔记库名称。 |
| `sourcePath` | 要读取的 Markdown 文件绝对路径；文件需要已经存在。 |
| `noteFile` | 同一文件相对笔记库的路径，用于在 Obsidian 中打开它。 |
| `powershellPath` | PowerShell 7 可执行文件的绝对路径，可用 `(Get-Command pwsh).Source` 查看。 |

`resources/config.json` 是本机配置，不会被 Git 跟踪。

### 2. 下载 SDK 并构建

```powershell
New-Item -ItemType Directory -Path .\work -Force | Out-Null
curl.exe --fail --location --output .\work\webview2-sdk.zip https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/1.0.4258.31/microsoft.web.webview2.1.0.4258.31.nupkg
if ($LASTEXITCODE -ne 0) { throw 'WebView2 SDK download failed' }
if ((Get-FileHash -LiteralPath .\work\webview2-sdk.zip).Hash -ne '56F7F4B8BF9AEE4B8EFEFBBDD4F67D5F74EBD1B100ED0806DA71BF76AF481AA9') { throw 'WebView2 SDK checksum mismatch' }
Expand-Archive -LiteralPath .\work\webview2-sdk.zip -DestinationPath .\work\webview2-sdk
pwsh -NoProfile -File .\Build.ps1 -WebView2Sdk .\work\webview2-sdk
```

### 3. 安装并启动

```powershell
pwsh -NoProfile -File .\Install.ps1
& (Join-Path $env:LOCALAPPDATA 'Programs\ObsidianGlassTasks\NativeGlassTasks.exe')
```

默认安装到 `%LOCALAPPDATA%\Programs\ObsidianGlassTasks`，也可以给安装脚本传入 `-InstallDirectory` 指定其他位置。安装脚本不会覆盖已有目录，也不会自动创建开机启动项。

如需开机启动，可自行将程序快捷方式放入 Windows 启动文件夹。之后若要修改笔记配置，请先退出程序，再编辑安装目录里的 `resources/config.json`。

## 使用

| 操作 | 方式 |
| --- | --- |
| 移动窗口 | 拖动标题栏；位置会自动保存。 |
| 完成任务 | 点击任务左侧的圆圈；底部可撤销最近一次完成，撤销成功提示约 3 秒后消失。 |
| 打开笔记 | 点击标题旁的打开图标，或点击任务中的 Obsidian 链接。 |
| 置顶、刷新或退出 | 点击标题旁的“…”或右键打开窗口选项。 |
| 截图 | 在窗口选项中开启“暂停动态玻璃（便于截图）”，完成后再次点击恢复。 |

托盘图标可重新唤出窗口。也可以通过 `NativeGlassTasks.exe --stop` 请求正常退出；如果正在保存，程序会等保存完成后退出。

## 材质与限制

玻璃效果使用 C++、D3D11、DXGI Desktop Duplication、DirectComposition 和 WebView2 实现。材质参考 [liquidglass 的 Dock 预设](https://github.com/shayann07/liquidglass/blob/0cbd5e24058d2fb3fffe45b72cd07915d4dcea6f/liquidglass/src/commonMain/kotlin/com/wexpa/liquidglass/GlassStyle.kt#L672)，中央保留轻模糊和较高透光度，圆角与直边使用连续折射曲线。

这是基于公开资料实现的 Windows 效果，不包含 Apple 私有渲染器，也不代表与 iOS 26 完全一致。高透光背景上的原始文字颜色不保证达到 WCAG AA 对比度。当前仅验证过一套 NVIDIA 单屏环境；跨显卡、多屏、HDR、远程桌面、锁屏恢复和高 DPI 仍需在对应环境中验证。

窗口状态及不含笔记正文的诊断保存在 `%LOCALAPPDATA%\ObsidianGlassTasks`。背景画面仅在本机显卡内参与渲染，不保存画面文件或上传；程序没有监听端口，也没有开启远程调试。

## 开发与检查

构建完成后可运行以下检查。每次测试请使用新的目录，写回测试只操作独立生成的笔记：

```powershell
node --test .\tests\tasks.test.mjs
pwsh -NoProfile -File .\tests\action.tests.ps1 -FixtureRoot .\work\action-fixture
pwsh -NoProfile -File .\tests\native.tests.ps1 -FixtureRoot .\work\native-fixture
```

原生检查覆盖合成背景上的玻璃渲染、WebView2 桥接、完成与撤销。合成渲染不采集桌面；视觉效果和鼠标操作仍需在实际 Windows 桌面核对。

<details>
<summary>从旧版 Rainmeter 皮肤切回</summary>

仅适用于此前已经安装 `MarkdownTasks\Glass` 皮肤的用户。普通安装不需要 Rainmeter。

安装时可传入 `-RainmeterIniPath <现有 Rainmeter.ini 的路径>` 保存备份。需要恢复时，在安装目录运行：

```powershell
pwsh -NoProfile -File .\Restore-Rainmeter.ps1 -RainmeterPath 'C:\Program Files\Rainmeter\Rainmeter.exe'
```

该脚本关闭本程序、移除指向本程序的开机快捷方式，并激活已有皮肤；不会整文件覆盖 Rainmeter 配置，也不会修改笔记。窗口菜单中的“切回 Rainmeter 版”需要 `Rainmeter.exe` 可从 `PATH` 找到。

</details>

第三方来源与许可见 [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md)。
