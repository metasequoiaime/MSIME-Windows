# 水杉输入法

[官网](https://msime.app) · [用户文档](https://msime.app/docs/) · [隐私说明](PRIVACY.md) · [English README](README.en.md)

<!-- badges:start -->
[![CI](https://img.shields.io/github/actions/workflow/status/metasequoiaime/MSIME-Windows/ci.yml?branch=develop&label=CI)](https://github.com/metasequoiaime/MSIME-Windows/actions/workflows/ci.yml)
[![CodeQL](https://img.shields.io/github/actions/workflow/status/metasequoiaime/MSIME-Windows/codeql.yml?branch=develop&label=CodeQL)](https://github.com/metasequoiaime/MSIME-Windows/actions/workflows/codeql.yml)
[![Release](https://img.shields.io/github/v/release/metasequoiaime/MSIME-Windows?include_prereleases&label=release)](https://github.com/metasequoiaime/MSIME-Windows/releases)
[![Downloads](https://img.shields.io/github/downloads/metasequoiaime/MSIME-Windows/total?label=downloads)](https://github.com/metasequoiaime/MSIME-Windows/releases)
[![License](https://img.shields.io/github/license/metasequoiaime/MSIME-Windows)](LICENSE)
[![Stars](https://img.shields.io/github/stars/metasequoiaime/MSIME-Windows?style=flat)](https://github.com/metasequoiaime/MSIME-Windows/stargazers)
<!-- badges:end -->

一款开源的自由 Windows 输入法，面向 Windows 10 / Windows 11，采用纯 TSF 方案。

## 下载与安装

- **安装包**：[Releases](https://github.com/metasequoiaime/MSIME-Windows/releases)，或[官网下载页](https://msime.app/download/)
- **安装步骤与常见问题**：[docs/installation.md](docs/installation.md)

安装后用 `Win + 空格` 切换到水杉输入法。如果切换不过去、Server 反复退出或设置窗口一闪即关，多半是缺少 Visual C++ 2015–2022 Redistributable（**x64**），处理办法见安装文档的常见问题。

其他平台：[macOS 与 iOS](https://github.com/metasequoiaime/MSIME-Apple) · [Linux](https://github.com/metasequoiaime/MSIME-Linux)

## 2026.09.02 给水杉输入法的新用户的说明

最近两天，水杉输入法的新增用户远超预期，约有 5000 名新用户加入，内测服务的多项 API 配额也因此暂时耗尽。随着用户增长，我们收到了许多功能建议，也发现了一些内测阶段的问题。目前我正在逐一修复，同时重构 UI，并开发一套专门面向输入法场景的原生 GUI 框架。

如果你在使用过程中遇到问题，还请耐心等待后续版本。也欢迎有能力的朋友提交 PR：无论是完善输入法本身，还是参与其背后的 GUI 框架，都非常欢迎。本项目接受纯 vibe 的 PR；唯一的请求是友善交流——不要骂主包。

如果你是在校学生或正在求职，也欢迎通过贡献代码，或整理与学校、专业相关的词库来参与项目(刷个 PR)。输入法是一款能够直接面向大量用户的软件；如果你希望自己参与开发的软件、自己写的代码真正被人使用并获得反馈，这会是一个不错的实践机会。

本项目现在及未来都会保持 100% 开源。开源的目的之一，是探索 AI 能否参与完善输入法这类容错空间较小、与操作系统耦合较深的软件；同时，也希望为由大型厂商主导的工具软件生态提供一个开放、可控的替代选择。

### 如何参与贡献

不会写 TSF 或 C++ 也没关系。项目可以参与的方向按类别整理在[招募开源开发者](https://github.com/metasequoiaime/.github/blob/main/RECRUITING.md)里，从整理词库、补文档、做本地化、测兼容性，到实现新的输入方案都有。

Issue 已经逐条核过并按可接手程度打了标签，可以直接筛：

- [good first issue](https://github.com/metasequoiaime/MSIME-Windows/issues?q=is%3Aissue+is%3Aopen+label%3A%22good+first+issue%22)——改动小，根因和修法已经写明
- [no-code](https://github.com/metasequoiaime/MSIME-Windows/issues?q=is%3Aissue+is%3Aopen+label%3Ano-code)——不需要写代码：图标资源、词库条目、文档、调研
- [help wanted](https://github.com/metasequoiaime/MSIME-Windows/issues?q=is%3Aissue+is%3Aopen+label%3A%22help+wanted%22)——根因明确、改动中等
- [needs-design](https://github.com/metasequoiaime/MSIME-Windows/issues?q=is%3Aissue+is%3Aopen+label%3Aneeds-design)——方案未定，先在 Issue 里讨论再动手

写代码、写文档、整理词库、测试 Bug、制作教程，都是非常有价值的开源贡献。可以纯手写，也可以借助 AI；但请认真检查和测试，并注意不要在 PR、Issue、截图或日志中泄露 API Key 等敏感信息。完整的贡献约定见[贡献指南](https://github.com/metasequoiaime/.github/blob/main/CONTRIBUTING.md)。

**要动代码的话，本仓的环境要求、从源码构建、打包和提交前检查全部写在 [CONTRIBUTING.md](CONTRIBUTING.md) 里。**

## 项目结构

Windows 端的全部一方源码都在本仓，一次 clone 就能拿到完整产品。DLL 与 Server 仍是两个进程，只是不再是两个仓库。

| 目录 | 职责 |
|---|---|
| [`windows/`](windows/) | 核心 TSF 文本服务 DLL：按键预判、焦点、edit session、与 Server 的管道协议 |
| [`server/`](server/) | Server 端：算法调度、候选窗与工具栏宿主、设置程序、词库管理 |
| [`engine/`](engine/) | 输入引擎：输入会话、候选查询、辅助码、跨进程契约、语音模块 |
| [`ui/`](ui/) | 自研原生 GUI 框架（`msimeui`）：Win32 宿主窗口，Direct2D / DirectWrite 渲染 |
| [`ui-html/`](ui-html/) | 界面资源（HTML / CSS / JS）：候选窗、悬浮工具栏、托盘菜单、设置页 |
| [`installer/`](installer/) | 收集各组件产物、自签名、Inno Setup 打包 |
| [`log/`](log/) | 各模块共用的日志采集库 |
| [`experiments/tsf-edit-control/`](experiments/tsf-edit-control/) | 基于 Win32 TSF 的编辑控件实验工程，附最小宿主 demo |
| [`vendor/`](vendor/) | submodule：opencc 与 cpp-pinyin 两个第三方依赖 |
| `scripts/`、`tests/`、`docs/`、`product-lock.json` | 产品级构建脚本、组合验证、文档与外部输入清单 |

### 引擎在仓内

`engine/` 曾经是指向 [MSIME-Engine](https://github.com/metasequoiaime/MSIME-Engine) 的 submodule，现在是本仓的一个一等组件，和 `server/`、`windows/` 平级，并按 Windows 做了专门化：本地 Whisper 推理、macOS 示例宿主、上游 CI 与发布配置、以及建库流水线都已裁掉。来源提交和完整裁剪清单见 [`engine/UPSTREAM.md`](engine/UPSTREAM.md)。

这对日常开发意味着三件事：

- **普通 clone 就能构建。** 引擎不再需要 `git submodule update`，`vendor/` 下只剩 opencc 和 cpp-pinyin。
- **要为 Windows 改引擎就直接改。** 不再需要先往上游提 PR、等合并、再 bump gitlink；引擎和调用它的代码在同一个提交、同一个 PR 里评审。`scripts/format.sh` 也覆盖了引擎源码。
- **引擎不再被 `product-lock.json` 锁。** 一个本仓提交就钉住了引擎，锁里只剩构建时下载的词库发布。

仍在仓外的部分：

- 词库：按固定 tag 和摘要在构建时从 `dict-*` release 取用。建库流水线不在本仓，已归档的 [MSIME-Dict](https://github.com/metasequoiaime/MSIME-Dict) 只读保留，其已发布的 release 仍可用。
- 语音输入的独立工具 [MetasequoiaVoiceInput](https://github.com/metasequoiaime/MetasequoiaVoiceInput) 已归档，其已发布的 release 仍可下载；录音、WAV 编码与识别/润色协议现在在 `engine/voice/`。

## 功能简介

- 中文输入：全拼、双拼（小鹤 / 自然码 / 首道 / 微软）、86 五笔。
- 日文输入：罗马字方案，提供平假名、片假名和日语词库候选；中文状态下也可用临时日语（R 模式）。
- 拼音方案支持辅助码（蓝天小雨点、自然码、首右 2.0、首右 Plus、小鹤）。
- 支持谷歌云候选、AI 联想（DeepSeek / OpenAI / SiliconFlow / Groq）。
- 竖排候选窗支持中英互译释义；本地未命中时可走腾讯云机器翻译。
- 中英混输、emoji 混输、颜文字混输；独立英文候选输入模式。
- 词库管理：全拼 / 五笔 / 英文查询、新增、批量导入与导出。
- 语音输入：豆包流式识别，以及 OpenAI / SiliconFlow / Groq 转写；可选文本润色。
- 手写识别、屏幕键盘、悬浮工具栏、剪贴板历史。
- 快捷短语（K）、日期时间（T）、Unicode（U）、数字转换（V）、Emoji（E）、颜文字（M）、超级简拼（J）、临时英文（Y）、临时日语（R）。
- 智能标点、重复标点转中文、成对标点补全、以词定字、简繁转换。
- 多种皮肤：Fluent、微信绿、石墨 Graphite、杨柳青 Willow green，均支持深色 / 浅色。

## 核心功能指南

> 下面这一节与[用户文档的 Windows 指南](https://msime.app/docs/)内容重叠。指南按设置窗口的实际布局组织，更新更及时；这里保留的是按功能组织的说明。两处不一致时以指南为准。



### 输入方案

在设置 → 输入中切换中文或日文模式。中文方案包括：

- **全拼**：按完整拼音输入。
- **双拼**：用两个按键组合声母和韵母。可选小鹤双拼、自然码双拼、首道双拼和微软双拼。
- **五笔**：当前提供 86 五笔。

翻页按键可在设置中勾选：`-` / `=`、`,` / `.`、`Shift + Tab` / `Tab`、`Page Up` / `Page Down`；`↑` / `↓` 用于移动高亮候选项。

中英文状态可按应用分别记忆，也可全局统一。`Ctrl + Shift + E` 可进入或退出独立的英文候选输入模式。`Ctrl + .` 切换中英文标点，`Ctrl + Shift + Space` 切换全角 / 半角。简繁输出可在设置中选择。

### 辅助码

辅助码是一种可选的形码筛选功能。输入完整的拼音后，在末尾追加辅助码，可以缩小候选范围或调整候选顺序。

辅助码方案可以在配置中选择，目前支持：

- 蓝天小雨点
- 自然码
- 首右 2.0
- 首右 Plus
- 小鹤

辅助码有“单码”和“双码”两种使用方式，也可以分别用于全拼和双拼。

#### 单码辅助

在完整的拼音或双拼编码后追加一个辅助码。

单码辅助主要用于调整候选顺序：

- 单字：辅助码匹配汉字的任意一个形码。
- 词组：辅助码匹配词组首字或末字的首个形码。
- 匹配到的候选会被优先显示，但其他候选仍然保留。

例如，使用自然码辅助码时，“阿”的辅助码为 `ek`：

```text
全拼：aE
双拼：aae 或者 aaK
```

双拼单码中，小写字母优先匹配候选的第一个辅助码，大写字母优先匹配第二个辅助码。对单字，这分别指该字的两个形码；对词组，分别指首字和末字的首个形码。因此，对于辅助码为 `ek` 的“阿”，`aae` 以第一码 `e` 优先，`aaK` 以第二码 `k` 优先。其他候选仍会保留。

注意：双拼追加的单码大小写都可以，但大小写会决定优先匹配的辅助码位置；全拼追加的单码必须是大写。

#### 双码辅助

在完整的拼音或双拼编码后追加两个辅助码。

**双码辅助**会严格筛选候选：

- 单字：两个辅助码分别匹配该字的两个形码。
- 词组：第一个辅助码匹配首字的首个形码，第二个辅助码匹配末字的首个形码。
- 筛选结果只保留符合辅助码的候选。

例如，“阿”的辅助码为 `ek`：

```text
全拼：aEK
双拼：aaeK、aaKe 或 aaKE
```

全拼输入时，两个辅助码使用大写字母。双拼输入时，第一个输入的辅助码为小写时，使用正常顺序，第二码需要大写来触发双码辅助，例如 `eK` 匹配 `ek`。第一个输入的辅助码为大写时，第二码无论大小写都会触发双码辅助，并按反转顺序匹配，例如 `Ke` 或 `KE` 都会匹配实际辅助码 `ek`。

双拼双码辅助的大小写规则如下：

| 第一码 | 第二码 | 结果 |
|---|---|---|
| 小写 | 小写 | 不触发双码辅助 |
| 小写 | 大写 | 触发，按正常顺序匹配 |
| 大写 | 小写 | 触发，按反转顺序匹配 |
| 大写 | 大写 | 触发，按反转顺序匹配 |

**全拼辅助**是在完整的全拼后追加辅助码：

```text
阿：aE       # 单码辅助
阿：aEK      # 双码辅助
阿姨：ayiE   # 单码辅助
阿姨：ayiEN  # 双码辅助
```

**双拼辅助**是在完整的双拼编码后追加辅助码。以小鹤双拼为例：

```text
阿：aae       # 单码辅助
阿：aaK       # 单码辅助，第二码优先
阿：aaeK      # 双码辅助
阿：aaKe      # 双码辅助，反转顺序
阿：aaKE      # 双码辅助，反转顺序
阿姨：aayie   # 单码辅助
阿姨：aayiN   # 单码辅助，末字辅助码优先
阿姨：aayieN  # 双码辅助
阿姨：aayiNe  # 双码辅助，反转顺序
阿姨：aayiNE  # 双码辅助，反转顺序
```

**注意**：辅助码只有在前面的全拼或双拼编码完整时才会生效。如果前面的拼音尚未完整，末尾输入的字母仍会按照普通拼音处理。

### 日语输入

设置 → 输入 → 输入模式选择「日文」后，使用罗马字输入。候选包括平假名、片假名和日语词库结果。中文方案会在切到日文时保留，切回中文后仍使用上次的全拼 / 双拼 / 五笔。

中文模式下也可开启临时日语（R 模式）：按 `Shift + R` 后，后续字母按日语罗马字处理；空格上屏首选，数字选其他候选，上屏后自动回到中文输入。

### 云联想

当前使用谷歌的云联想 API，对网络有所要求。云联想如果返回了当前本地不存在的结果，会插入在首页的第二项。网络不可用时，本地输入不受影响。

### AI 联想

全拼和双拼输入时可异步请求 AI 联想。返回内容如果不与本地以及云联想重复，会插入首页第三项。接口异常时不影响本地候选。

提供商可选 DeepSeek、OpenAI、SiliconFlow、Groq，均走 OpenAI 兼容的 Chat Completions 接口。每个提供商的 Token 分开保存。可配置模型、候选数量上限、接口地址，以及最多三套自定义提示词。

### 候选词翻译

开启后，仅在竖排候选窗口中为候选项显示中文与所选语种的互译，每项最多两个简短释义。优先使用本地释义；本地未命中时可选择腾讯云 TMT，或填写自建 DeepLX 兼容服务的完整接口地址和可选 Bearer API Key，由云端补全。凭据只保存在本机配置文件中。

### 候选项调频

可以在设置中自行选定调频算法：

- **关闭**：不调整顺序。
- **一次置顶**：把触发调频的候选移到首位。
- **折半调频**：把候选移到当前名次与首位之间。
- **线性调频**：按固定步数向前移动（步长可设为 1～6）。
- **一次置前**：前五名中的候选前移一位，第五名之后的候选提到第五名。

还可设置触发频次（同一候选累计选中多少次后才调整一次，1～6）。

### preedit 显示设置

用户可以通过设置 → 外观 → 行内预编辑和设置 → 外观 → 候选窗预编辑来控制应用内的拼音串和候选框内的拼音串的显示风格以及是否显示。

行内预编辑可选「原始按键」「拼音分词」或「不显示」。候选窗预编辑可选「拼音分词」或「不显示」。两者相互独立。

### 中英混输、emoji / 颜文字混输和英文输入法模式

开启中英混输后，中文输入时会根据字母串补充英文候选。触发字符数可在设置中选择（1～8），达到该长度后才出现英文候选项。

emoji 混输会在英文候选之后插入匹配的 emoji；颜文字混输排在 emoji 之后。若同时开启云候选或 AI 联想，这些附加项会相应顺移。

在英文模式下，按下 `Ctrl + Shift + E` 可以切换到独立的英文输入法模式。这个模式与中英混输不同：中英混输是在拼音候选中补充英文，英文候选模式则专门按英文词库匹配当前字母串。

### 标点与以词定字

- **智能标点**：中文标点状态下，逗号、句点或冒号前面紧接着字母或数字时，自动输出英文 `,` / `.` / `:`。
- **重复标点转中文**：智能标点输出英文标点后，2 秒内再次输入同一标点，会替换为对应中文标点。
- **成对标点自动补全**：输入左侧括号、引号等成对符号时，自动补上右侧符号，并把光标放在中间。
- **以词定字**：开启后，按 `[` 上屏当前高亮候选的第一个汉字，按 `]` 上屏最后一个汉字。
- 中文标点状态下，`\` 输入顿号 `、`，反引号输入间隔号 `·`，`Shift + 6` 输入省略号 `……`，`Shift + -` 输入破折号 `——`，`Shift + ,` / `Shift + .` 输入书名号 `《` / `》`。

### 实用功能快捷模式

均在中文状态下用 `Shift + 字母` 进入，可在设置 → 实用功能中分别开关。

| 模式 | 快捷键 | 说明 |
|---|---|---|
| 快捷短语（K） | `Shift + K` | 再输入字母编码调用短语。编码只能是英文字母。支持查询、新增、批量导入和导出。 |
| 日期时间（T） | `Shift + T` | `rq` / `riqi` / `date` 当前日期；`sj` / `shijian` / `time` 当前时间；`xq` / `xingqi` / `week` 当前星期。 |
| Unicode（U） | `Shift + U` | 再输入十六进制码位，例如 `4e00` 或 `+1f600`。空格上屏首选；`Shift + 数字` 选其他候选。 |
| 数字转换（V） | 全拼直接 `v` + 数字，或 `Shift + V` | 例如 `v123456.78` → 壹拾贰万叁仟肆佰伍拾陆元柒角捌分，另有中文大写 / 小写读法、逐位读法、`123,456.78` 与 `12,3456.78` 分节。双拼下 `v` 是声母键，只能用 `Shift + V`。空格上屏首选；`Shift + 数字` 选其他候选。 |
| Emoji（E） | `Shift + E` | 按全拼 / 简拼 / 双拼 / 英文关键词检索 emoji，例如 `xiaolian`、`xl`、`laugh`。 |
| 颜文字（M） | `Shift + M` | 按全拼 / 简拼 / 双拼 / 英文关键词检索颜文字，例如 `haixiu`、`kiss`。 |
| 超级简拼（J） | `Shift + J` | 之后每个字母都按简拼检索。全拼如 `nh` → 你好；双拼会按当前方案转换声母，例如小鹤 `nu` → `n'sh`（你说），与 `ns` → `n's` 不同。调频规则与全拼 / 双拼相同。 |
| 临时英文（Y） | `Shift + Y` | 之后字母按英文处理。空格上屏当前输入；数字可选英文词库单词。上屏后回到中文。 |
| 临时日语（R） | `Shift + R` | 之后字母按日语罗马字处理。上屏后回到中文。 |

剪贴板管理开启后，复制过的文本会出现在表情面板的剪贴板分页中；关闭会立即清空记录，且只记录文本。

### 语音输入

需要网络服务。可在右键菜单中开始或停止录音，也可使用快捷键：

- `RAlt`：长按录音，松开后识别。
- `Ctrl + Win`、`RCtrl + RAlt`：同样为长按录音。
- 录音时按 `Space` 可锁定，松开长按键也不会停止。
- `Ctrl + F9`：开始 / 停止录音，也可结束已锁定的录音。
- `Esc`：取消本次录音。

ASR 提供商可选豆包（流式）、OpenAI、SiliconFlow、Groq。豆包支持数字格式化、标点预测、语义顺滑和热词表。可选文本润色（SiliconFlow / OpenAI / DeepSeek / Groq），预设包括精炼整理、忠实校对、中翻英、口语整理，以及三套自定义提示词。

上屏方式可选 TSF、SendInput 或 `Ctrl + V`。豆包流式识别还可开启 inline 预编辑：已识别文字直接显示在输入框中，松开录音键后上屏（仅 TSF 上屏时生效）。录音期间可选择暂时静音其他应用的播放声音。

独立工具 [MetasequoiaVoiceInput](https://github.com/metasequoiaime/MetasequoiaVoiceInput) 已归档，其已发布的 release 仍可下载；源码现在在 [`engine/voice/`](engine/voice/)。

### 手写、屏幕键盘、悬浮工具栏与皮肤

- **手写识别板**：鼠标或触控书写后显示候选汉字。若没有结果，请确认 Windows 已安装中文手写包。
- **屏幕键盘**：用鼠标或触控输入文字和快捷按键。
- **悬浮工具栏**：可调整缩放和图标尺寸；中英文切换始终显示，其余组件（全角、标点、简繁、表情、屏幕键盘、设置）可按需勾选。
- **皮肤**：Fluent 默认、微信绿、石墨 Graphite、杨柳青 Willow green。候选窗、工具栏等界面可分别跟随全局主题，或单独指定深色 / 浅色。

### 词库

设置 → 词库可切换全拼、五笔和英文词库，支持查询、新增、编辑、删除和批量导入；也可导入纯汉字词组（例如人名），由输入法生成拼音。拼音和五笔批量文件使用 Tab 分隔“词语、编码、权重”三列，因此词语内可保留空格。用户词库可分别导出，便于备份和迁移。快捷短语的导入导出在实用功能页。

### 自定义候选窗翻译

候选窗显示的中英互译来自内置词库，覆盖不全或译得不准时，可以自己加一层覆盖，不需要改内置词库。

在 `%LOCALAPPDATA%\metasequoiaime\` 目录下新建 `custom_translations.txt`，每行一条，用 **Tab** 分隔源词和译文：

```
你好	hello
刚才	a moment ago; just now
serendipity	意外发现珍奇事物的本领
```

规则：源词含汉字即为中译英，全是英文则为英译中；以 `#` 开头的行是注释；空行忽略；文件用 UTF-8 保存，带不带 BOM 都可以。同一个源词写多次时，以最后一次为准。

这份文件优先于内置词库，改完重启输入法生效。

### 服务守护

Server 启动时会同时启动 Watchdog。服务意外退出后，Watchdog 会自动拉起；短时间内反复崩溃会使用指数退避（最长约 30 秒），避免重启风暴。

开发与维护快捷键（输入法服务运行期间全局生效）：

- `Ctrl + Shift + Alt + 1`～`8`：删除当前候选窗对应位置的候选项。
- `Ctrl + Shift + Alt + C`：清除引擎缓存。
- `Ctrl + Shift + Alt + R`：重启输入法服务。
- `Ctrl + Shift + Alt + T`：立即退出输入法服务。

设置界面「外观 → 重启输入法进程」提供同样的重启操作，方便在修改界面渲染方式后立即生效。

## 软件截图

<img src="docs/images/candidate-window-1.png" width="750">

<img src="docs/images/candidate-window-2.png" width="750">

<img src="docs/images/candidate-window-3.png" width="750">

<img src="docs/images/candidate-window-4.png" width="750">

<img src="docs/images/candidate-window-5.png" width="750">

<img src="docs/images/candidate-window-6.png" width="750">

<!-- 暗色 -->

<img src="docs/images/settings-dark.png" width="780">

<!-- 亮色 -->

<img src="docs/images/settings-light.png" width="780">

## 开源协议

GPL-3.0。

## 参考

### Windows 开源输入法参考

以下是一些 Windows 的开源输入法的参考项目。

- [Microsoft TSF IME Demo](https://github.com/microsoft/Windows-classic-samples/tree/main/Samples/IME/cpp/SampleIME)
- [weasel](https://github.com/rime/weasel)
- [PIME](https://github.com/EasyIME/PIME)
- [WindInput](https://github.com/huanfeng/WindInput)

### 一些其他输入法相关的资源

- 微软双拼方案来源：<https://www.bilibili.com/opus/550351671882601523>

<!-- star-history:start -->
## Star History

<a href="https://star-history.com/#metasequoiaime/MSIME-Windows&Date">
  <img src="https://api.star-history.com/svg?repos=metasequoiaime/MSIME-Windows&type=Date" alt="Star History Chart" width="600">
</a>
<!-- star-history:end -->
