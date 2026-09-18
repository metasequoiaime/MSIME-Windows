# TSF 真机按键行为验证（本地脚本）

在**真实宿主**里用 `SendInput` 驱动按键，断言组词层面的可观察结果（提交文本、诊断日志事件），
覆盖 CI 覆盖不到的 TSF 按键链路：按段删除、退选、长按守卫、模式退化等。

> **这不是 CI 门禁，也不替代物理按键验证。**
> 注入按键与真实键盘在**修饰键时序**和 **auto-repeat** 上并不完全等价；
> 详见下面的「已知注入伪影」。脚本产出的是可复核的证据，不是"通过 = 没问题"的结论。
> 运行期间别动鼠标键盘：其它窗口抢焦点会让结果不可信（脚本会显式报 `FOCUS_FAIL`）。

## 文件

| 文件 | 用途 |
|---|---|
| `Invoke-TsfKeyBehaviorTests.ps1` | 测试脚本（配置驱动） |
| `cases.example.json` | 用例示例：按 tag 分组（quanpin / xiaohe / microsoft / wubi / japanese / localmodes / steps / diag） |
| `TsfTestHost.ps1` | 兜底宿主：WinForms 多行文本框。目标机没有任何编辑器时使用 |

## 前提

1. 本机已安装**包含被测行为**的测试构建（例如先跑 `installer/test-light.ps1`），
   并确认宿主进程加载的是新 DLL（脚本预检会打印实际加载路径）。
2. 宿主：不传 `-HostPath` 时先自动探测编辑器（notepad / notepad4 / Notepad++ / write / wordpad，
   跳过 scoop shim 这类启动即退出的壳），都没有才用自带的 `TsfTestHost.ps1`。
3. **中文态怎么来**（容易踩坑）：
   - `Ctrl+Space` 是 **Windows 系统级**的输入法切换，只负责把系统切到本输入法；
   - 本输入法自己的中英文开关在 `config.toml` 的 `keybindings.switch_language_shift` /
     `switch_language_ctrl` / `switch_language_ctrl_alt_space`（设置 → 快捷键 → 中英文切换）。
     脚本会读这份配置，按启用的键尝试切换；三组全关时会在预检直接给出提示而不是跑出假结果。
   - 出厂默认：`Shift` 与 `Ctrl+Alt+Space` 开、`Ctrl` 关。
   - `default_ime_mode = "english"` 时新进程从英文态起步，所以校准是必需的。

## 跑法

```powershell
# 用示例配置跑全拼 + 通用步骤用例（自动探测宿主；也可加 -HostPath 指定）
pwsh -File windows/tests/real-machine/Invoke-TsfKeyBehaviorTests.ps1 `
  -ConfigFile windows/tests/real-machine/cases.example.json -Tag quanpin,steps

# 指定宿主 + 指定用例
pwsh -File windows/tests/real-machine/Invoke-TsfKeyBehaviorTests.ps1 `
  -HostPath 'C:\path\to\your\editor.exe' `
  -Cases 'seg:nihaoma=nihao','bs:nihaoma=nihaom'

# 长按不外溢（要开 general.tsf_diagnostic_log，脚本只读日志、不改配置）
pwsh -File windows/tests/real-machine/Invoke-TsfKeyBehaviorTests.ps1 `
  -ConfigFile windows/tests/real-machine/cases.example.json -Tag diag `
  -DiagnosticLogPath "$([Environment]::GetFolderPath('Desktop'))\水杉IME诊断日志.log"

# 只解析配置、打印展开后的步骤（不启动宿主）
pwsh -File windows/tests/real-machine/Invoke-TsfKeyBehaviorTests.ps1 `
  -ConfigFile windows/tests/real-machine/cases.example.json -DryRun
```

## 配置格式（cases.example.json）

顶层字段：`host`、`hostArguments`、`imeToggle`、`reuseHost`、`reportPath`、`diagnosticLog`
（CLI 同名参数优先；`host` 留空 = 自动探测/自带宿主）。

`cases` 里每个用例二选一：

```jsonc
// 1) 紧凑写法（无 tag，适合 CLI 一次性用例）
"seg:nihaoma=nihao"

// 2) 结构化：preset + text，或通用 steps
{ "name": "退选", "tags": ["quanpin"], "preset": "retreat", "text": "womendejiax", "expect": "womendejia" }
{ "name": "自定义", "tags": ["quanpin"], "steps": [
    { "type": "text", "value": "nihaoma" },
    { "type": "chord", "key": "LEFT", "mods": ["CTRL"] },
    { "type": "text", "value": "x" },
    { "type": "key", "key": "ENTER" } ],
  "expect": { "equals": "nihaoxma" } }
```

| preset | 展开 |
|---|---|
| `probe` | 键入 text（提交键写在 text 里，如结尾空格） |
| `seg` | text → Ctrl+Backspace → Enter |
| `bs` | text → Backspace → Enter |
| `retreat` | text → Space → Backspace → Enter |
| `segleft` | text → Ctrl+Left → `x` → Enter |
| `shiftseg` / `shiftbs` | Shift+首字符（本地模式）→ 其余 → Ctrl+Backspace / Backspace → Enter |
| `hold` | `前缀|组词`：粘贴前缀 → 键入组词 → 连续 keydown 不抬键（近似长按） |

| 步骤 type | 参数 | 说明 |
|---|---|---|
| `text` | `value` | 键入 ASCII（字母/数字/空格/`;`/`'`） |
| `key` | `key` | 单键：ENTER / BACKSPACE / SPACE / TAB / ESC / LEFT / RIGHT / UP / DOWN / DELETE / HOME / END / 单字符 |
| `chord` | `key`, `mods` | 修饰键**按住跨越目标键**（CTRL/SHIFT/ALT） |
| `shiftKey` | `key` | 按住 Shift 敲一个键（触发 U/K/E/M/J/Y/R 本地模式） |
| `paste` | `value` | 剪贴板粘贴（播种文档文本） |
| `repeat` | `key`, `count`, `intervalMs` | 连续 keydown 不抬键（近似长按；物理长按仍需人工确认） |
| `wait` | `ms` | 等待 |

`expect`：字符串（精确相等）或对象 `{ "equals": "..." }` / `{ "regex": "..." }` / `{ "contains": "..." }`，
可加 `"diag": { "event": "backspace-repeat-suppressed", "minCount": 1 }`（要求本次用例新增的日志事件数）。

## 结果解读

- 报告默认 `%TEMP%\tsf-key-behavior-report.txt`（UTF-8），每条含观察值与期望值。
- 退出码：`0` 全过；`1` 有断言失败；`2` 环境问题（预检/焦点）——**不是产品结论**。
- `FOCUS_FAIL`：宿主没拿到前台，结果不可信，关掉抢焦点窗口后重跑。
- `PREFLIGHT_FAIL`：宿主没加载 `MetasequoiaImeTsf.dll`，或无法进入中文组词态（含修法提示）。

## 为什么这样写（都踩过）

| 机制 | 防的坑 |
|---|---|
| 每个步骤前后校验前台窗口属于宿主（句柄比较） | 抢焦点导致用例文本粘连/假结论（真实发生过） |
| 修饰键"按住跨越目标键"（`Ctrl down → 200ms → 键 → 200ms → Ctrl up`） | TSF 用 `GetAsyncKeyState` 采样修饰键；连发会让 `Ctrl+Backspace` 退化成普通 Backspace |
| 默认每个用例开新宿主 | local 模式下 `Ctrl+A` 被吃，清文档失败会让结果串在一起 |
| 每个宿主用 `ni ` 探针校准中英文态：先试配置里启用的水杉切换键，再退到系统 `Ctrl+Space` | 真机 `default_ime_mode` 可能是 english；`Ctrl+Space` 只是系统切换，不是水杉的中英文开关 |
| 预检要求"宿主必须能进入中文组词态" | 静默跑出一堆假 FAIL 比直接报环境问题更糟 |
| `-HostPath` 留空时跳过 shim、扫描真实 exe，最后才用自带宿主 | scoop/其它 shim 启动即退出，会得到没有窗口的死进程 |

## 已知注入伪影（不要据此报产品 bug）

- **中文态、无组合时注入的 Backspace** 可能被 TSF 分类成
  `INVOKE/FINALIZE_TEXTSTORE`（`wch=U+0008`）而不到宿主。该分支早于本脚本验证的功能存在，
  是注入时序的产物；物理按键不受影响（需人工确认）。定位：打开 `general.tsf_diagnostic_log`
  看该键的 `category/function/buffer_len`。
- 双拼是**贪心最长配对**：`cb`（cou）这类合法编码会先被配走，尾部 `;` 可能自成一段；
  删除跟随 preedit 的同一条切分，**不是 bug**。

## 不覆盖

- UiLess 宿主（宿主自绘候选栏）与旧 Server 组合：需要特定宿主/旧二进制，手工验证。
- 物理 auto-repeat 的真实时序：`repeat` 步骤只是近似；长按行为建议人工按一次确认。
- 自带 WinForms 宿主的限制：它依赖本输入法已是活动输入法（脚本已用 CTF
  `ActivateLanguageProfile` 尝试激活，并在预检里要求能进入中文组态）。
  目标机若没有编辑器且该宿主也起不来，请在系统里把本输入法设为当前输入法后重跑。
