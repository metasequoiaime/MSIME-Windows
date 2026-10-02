# 外部皮肤 manifest（TOML）

每个皮肤目录需要 `skin.toml`，`id` 必须与文件夹名一致。`schema_version` 当前为整数 `1`。

Windows 与 macOS、Linux、iOS 等跨平台客户端读同一套规则：同一个皮肤包要么在每个平台上都能加载，要么都被拒绝，社区皮肤库也按这套规则校验。规则的权威实现和共享用例表在 [msime](https://github.com/metasequoiaime/msime) 的 `crates/client-core/src/skin/catalog.rs` 与 `catalog/client_dialect.json`。只有 Windows 会画的键（见下文 `border_width_dip`、`shadow`、`font_family`、`page_arrows`、细分配色和右键菜单配色）在其他平台上只校验、不生效。

- `id` 不能是全局主题 ID（`system`、`shuishan`、`light`、`paper`、`night`、`ink`、`custom`）、内置皮肤 ID，也不能是 `default`。
- `skin.toml` 最大 64 KiB。
- 可选的文本键（`author`、`description`、`preview`、`toolbar_stylesheet` 和 `[license]` 里的键）写了就不能是空字符串。

候选框和悬浮工具栏都由 `skin.toml` 声明，不要再提供 `cand.css` 或工具栏 CSS。所有图片路径都相对于皮肤目录，且不能指向目录之外。

## 顶层字段

- 必填：`schema_version`、`id`、`name`、`version`、`base`（继承的内置皮肤：`fluent`、`wechat`、`graphite`、`willow_green`、`autumn_osmanthus`、`microsoft`）。在其他平台上，这些外观画在原生配色之上，包没写的颜色和圆角按该外观在 Windows 上的配色补齐，`fluent` 就是原生配色
- `base` 也可以写跨平台客户端的全局主题：`system`、`shuishan`、`light`、`paper`、`night`、`ink`，社区皮肤库里的皮肤用的就是这一套。`system` 等同 `fluent`。其余五个主题各自固定一种明暗（`shuishan`、`night`、`ink` 为深色，`light`、`paper` 为浅色）：只取该明暗的 `[candidate.*]` 与 `[toolbar.*]` 配色，Windows 深浅两种模式都画它，没写的颜色按主题补齐，右键菜单与工具栏也跟着用这套配色；`supports.themes` 没有声明该明暗时皮肤不会生效。要在 Windows 上沿用其他内置皮肤的外观，仍写上面的内置皮肤 ID
- 可选：`author`（最长 120 字节）、`description`（最长 500 字节）
- 可选：`preview`，皮肤列表里的预览图，必须指向包内已有的文件
- 可选：`toolbar_stylesheet`，跨平台客户端的工具栏样式表，Windows 不加载它；写了就必须是包根目录里一个已有的 `.css` 文件
- `[supports]`：`layouts`（`horizontal` / `vertical`）、`themes`（`dark` / `light`）
- `[license]`：`code`、`assets`（各最长 120 字节）、`source`（最长 500 字节）

## `[candidate_window]`

- `min_width_dip`：卡片最小宽度；实际最小宽度还会被装饰图的 `width_dip` 撑大
- `corner_radius_dip`：卡片圆角，`0`–`32`；不写则沿用 `base`
- `border_width_dip`：卡片外框线宽，`0`–`4`；颜色取 `border`，不写 `border` 时取 `base` 的边框色（杨柳青、秋桂没有外框，此时仍不可见）
- `item_corner_radius_dip`：选中 / 悬停高亮的圆角，`0`–`16`；贴着卡片四角的那几个角仍跟 `corner_radius_dip`
- `shadow`：卡片阴影，`none` | `soft` | `strong`；不写则沿用 `base`。只影响候选卡片，不影响右键菜单
- `font_family`：候选字体名，UTF-8 编码后最长 64 字节（约 21 个汉字），不能含引号、反斜杠、逗号、分号、尖括号、花括号和反引号。排在用户设置的字体前面，缺字时仍回落到用户字体；字号不由皮肤决定
- `page_arrows`：候选框里的翻页箭头，`true` | `false`。横排时是候选右侧贴着最后一行的一小列，竖排时是最后一行下方的一矮行、“‹”与序号左对齐；颜色取 `number`，悬停底色取 `hover`。不写则沿用 `base` 的默认皮肤设置（数据目录 `skins\default\<base>\skin.toml`，出厂为 `false`）

### `[candidate_window.decoration]`（可选）

卡片上方的装饰图。`top_inset_dip` 与 `width_dip` 要么都大于 0，要么都为 0（不写算 0）；都为 0 时没有装饰，也不能写 `image`。有尺寸而不写 `image` 时改用顶层的 `preview`（须是图片），两者都没有就不画装饰。

- `image`：图片路径，扩展名须是 `png`、`jpg`、`jpeg`、`gif`、`webp`、`svg`、`ico`、`bmp` 或 `avif`
- `top_inset_dip` / `width_dip`：装饰框的高和宽，框贴在卡片上方，不与卡片重叠；图片在框内等比缩放
- `align`：`left` | `center` | `right`，相对卡片

### `[candidate_window.background]`（可选）

卡片背景图，画在底色之上、文字之下，按圆角裁剪，对深浅两套主题都生效。

- `image`：图片路径，扩展名规则同装饰图
- `fit`：`cover`（铺满裁切）| `contain`（完整显示）| `stretch`（拉伸）
- `opacity`：`0`–`1`

## `[candidate.dark]` / `[candidate.light]`

`accent`、`selected`、`hover`、`surface`、`border`、`text`、`number`、`translation`、`show_selected_bar`

细分配色，都可省略，不写时按右列回落：

| 键 | 作用 | 不写时 |
|---|---|---|
| `candidate_text` | 普通候选的文字 | `text` |
| `preedit_text` | 预编辑（拼音）文字 | `text` |
| `preedit_caret` | 预编辑光标 | `accent` |
| `selected_text` | 选中候选的文字 | `base` 的选中行文字色 |
| `selected_number` | 选中候选的序号 | `base` 的选中行序号色 |
| `selected_translation` | 选中候选的翻译 | `translation`，再不写则取选中文字色的 62% 不透明度 |
| `selected_bar` | 选中项左侧竖条 | `accent` |
| `preedit_background` | 预编辑行底色，圆角同高亮圆角 | 透明 |
| `preedit_divider` | 预编辑行下方 1px 分隔线 | 无 |

设置页里的「候选文字颜色」优先于 `text`、`candidate_text` 和 `preedit_text`。

### `[candidate.dark.menu]` / `[candidate.light.menu]`（可选）

候选窗右键菜单：`background`、`border`、`text`、`hover`。

## `[toolbar]`

悬浮工具栏样式，D2D 与 WebView2 两个渲染器都生效。所有键都可省略，不写的沿用 `base`。

- `corner_radius_dip`：`0`–`32`
- `[toolbar.dark]` / `[toolbar.light]`：`background`、`border`、`handle`（左侧拖动条）、`divider`、`icon`、`hover`

## 颜色格式

所有颜色键——`[candidate.dark]` / `[candidate.light]` 里除 `show_selected_bar` 外的键、`[candidate.*.menu]` 和 `[toolbar.dark]` / `[toolbar.light]` 的全部键——都接受以下写法。数字、布尔、枚举和路径类的键不是颜色，不能写成颜色值。

| 写法 | 示例 | 说明 |
|---|---|---|
| `#rgb` / `#rrggbb` | `#e8a`、`#e08aa8` | 不透明 |
| `#rgba` / `#rrggbbaa` | `#e8a4`、`#e08aa847` | 末尾一位或两位是不透明度 |
| `rgb(r, g, b)` | `rgb(224, 138, 168)` | `r`、`g`、`b` 为 0–255 |
| `rgba(r, g, b, a)` | `rgba(224, 138, 168, 0.28)` | `a` 为 0–1 |
| `transparent` | `transparent` | 全透明，等同 `#0000` |

- 十六进制大小写均可，但**必须带 `#`**：原生（D2D）渲染器不带 `#` 也能识别，WebView2 渲染器按 CSS 解析，会把它当作无效值，两种渲染结果就不一致了。
- 不透明度在**末尾**（`#RRGGBBAA`），不是 Windows 常见的 `#AARRGGBB`。例如 `#e9e8e89d` 约为 62% 不透明，`rgba(224, 138, 168, 0.28)` 与 `#e08aa847` 等价。
