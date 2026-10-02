# msime-skins

水杉输入法（Metasequoia IME）的外部皮肤合集。

## 皮肤列表

| 目录 | 名称 | 说明 |
| --- | --- | --- |
| [`niya-demo`](niya-demo/) | Niya Demo | 官方外部皮肤样例：候选框上方的人物装饰图、星点背景图、12dip 圆角，以及配套的悬浮工具栏配色 |
| [`bigfish`](bigfish/) | 蓝色大肥鱼 | 鲸鱼女仆角色装饰、海底气泡背景，深海蓝配色搭配金色序号 |
| [`bigfish-national-day`](bigfish-national-day/) | 大肥鱼·国庆 | 红裙敬礼的大肥鱼站在卡片右上方、金色星星背景，中国红高亮搭配金色序号 |
| [`bigfish-peek`](bigfish-peek/) | 大肥鱼·探头 | 黑白女仆装的大肥鱼从卡片上沿探出头、气泡与蕾丝背景，深蓝底色搭配浅蓝高亮 |
| [`qq-blue`](qq-blue/) | QQ 经典蓝 | 腾讯 QQ 配色：亮蓝光标与序号，深蓝选中块 |
| [`sogou`](sogou/) | 搜狗经典 | 搜狗经典配色：白底、蓝色候选、橙红高亮、红色光标 |
| [`sogou-classic`](sogou-classic/) | 搜狗经典·石墨 | 基于石墨，仿经典搜狗：白底浅蓝边框、蓝字候选、首选红字不铺底色、拼音下方浅蓝分隔线 |

除 `sogou-classic` 基于 `graphite` 外，其余皮肤均基于 `fluent`；全部支持横排 / 竖排布局以及深色 / 浅色主题。

## 默认皮肤设置（`default/`）

[`default/`](default/) 不是外部皮肤，而是六个内置皮肤（`fluent`、`wechat`、`graphite`、`willow_green`、`autumn_osmanthus`、`microsoft`）各自的设置清单。安装包会把它们放到数据目录的 `skins\default\<id>\skin.toml`，升级时只补缺失的文件、不覆盖已有的，所以可以直接在那里改。

内置皮肤的外观仍由输入法内置样式决定，这份清单只承载皮肤级的开关，目前支持：

| 表 / 键 | 说明 |
| --- | --- |
| `schema_version` / `id` / `name` | `id` 必须与目录名一致 |
| `[candidate_window]` `page_arrows` | 候选框里是否显示翻页箭头（`true` / `false`，出厂与缺省都是 `false`） |

外部皮肤没写的开关沿用它 `base` 对应的这份清单。改完后在设置页刷新皮肤列表，或重新选择一次皮肤即可生效；设置页「外观」和「皮肤」里的候选框预览会一并显示或隐藏箭头。

## 安装

除 `niya-demo`（素材授权未核实）外，这里的皮肤都随安装包装到数据目录的 `skins\<id>`。用户那边已有同名皮肤且内容和随包的不一样（改过、多了或少了文件）时，安装器先把它改名成 `<id>.bak` 保留下来（已有 `.bak` 则用 `<id>.2.bak`……），再装新版本；输入法不会把 `.bak` 目录列成皮肤。

手动安装时，把想用的皮肤目录整个复制到：

```
%LOCALAPPDATA%\metasequoiaime\skins\
```

例如 `%LOCALAPPDATA%\metasequoiaime\skins\qq-blue\skin.toml`，然后在输入法设置中选择对应皮肤即可。

## 皮肤结构

每个皮肤是一个独立目录，至少包含一个 `skin.toml`，图片等资源放在目录内（如 `assets/`）：

```
my-skin/
├── skin.toml
└── assets/          # 可选
    ├── background.png
    └── character.png
```

`skin.toml` 主要字段：

| 表 / 键 | 说明 |
| --- | --- |
| `schema_version` | 皮肤格式版本，目前为 `1` |
| `id` / `name` / `version` / `author` / `description` | 基本信息，`id` 建议与目录名一致 |
| `base` | 继承的内置皮肤，如 `fluent`；未写的键沿用 base。也可以写跨平台全局主题 `system`、`shuishan`、`light`、`paper`、`night`、`ink`，见[字段说明](../examples/skin-examples/schema/README.md) |
| `[supports]` | `layouts`（`horizontal` / `vertical`）、`themes`（`dark` / `light`） |
| `[candidate_window]` | `min_width_dip`、`corner_radius_dip`（0–32）、`border_width_dip`（0–4）、`item_corner_radius_dip`（高亮圆角，0–16）、`shadow`（`none` / `soft` / `strong`）、`font_family`（排在用户字体前面）、`page_arrows`（翻页箭头：横排在候选右侧，竖排在最后一行下方、与序号左对齐；不写时沿用 base 的[默认皮肤设置](#默认皮肤设置default)，出厂关闭） |
| `[candidate_window.decoration]` | 卡片上方的装饰图：`image`（不写时用 `preview`）、`top_inset_dip`、`width_dip`、`align`（`left` / `center` / `right`） |
| `[candidate_window.background]` | 卡片背景图：`image`、`fit`（`cover` / `contain` / `stretch`）、`opacity`（0–1） |
| `[candidate.dark]` / `[candidate.light]` | 候选配色：`accent`、`selected`、`hover`、`surface`、`border`、`text`、`number`、`translation`、`show_selected_bar`；细分配色 `candidate_text`、`preedit_text`、`preedit_caret`、`selected_text`、`selected_number`、`selected_translation`、`selected_bar`、`preedit_background`、`preedit_divider` |
| `[candidate.dark.menu]` / `[candidate.light.menu]` | 候选窗右键菜单：`background`、`border`、`text`、`hover` |
| `[toolbar]` / `[toolbar.dark]` / `[toolbar.light]` | 悬浮工具栏：`corner_radius_dip`，以及 `background`、`border`、`handle`、`divider`、`icon`、`hover` |
| `[license]` | `code`、`assets`、`source`：代码与素材的授权信息 |

所有颜色键（`[candidate.*]` 里除 `show_selected_bar` 外的键、`[candidate.*.menu]` 与 `[toolbar.*]` 的全部键）都支持十六进制 `#RGB`、`#RGBA`、`#RRGGBB`、`#RRGGBBAA`，以及 `rgb(r, g, b)`、`rgba(r, g, b, a)` 和 `transparent`。十六进制必须带 `#`（WebView2 渲染器按 CSS 解析，不带 `#` 会失效），不透明度在末尾（`#RRGGBBAA`，不是 `#AARRGGBB`），例如 `rgba(224, 138, 168, 0.28)` 与 `#e08aa847` 等价。图片路径相对于皮肤目录，且不能指向目录之外。完整的带注释示例见 [`niya-demo/skin.toml`](niya-demo/skin.toml)。

## 授权说明

本仓库以 [GNU General Public License v3.0](LICENSE) 开源。单个皮肤如在 `skin.toml` 的 `[license]` 中另有声明，以该声明为准。

`niya-demo` 中的图片素材授权尚未核实（`assets = "UNVERIFIED-DEMO-ONLY"`），仅作演示用途，请勿直接用于再分发的皮肤。
