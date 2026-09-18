<#
.SYNOPSIS
    TSF 真机按键行为验证（本地运行，不接 CI）。

.DESCRIPTION
    在真实宿主里用 SendInput 驱动按键，断言组词层面的可观察结果（提交文本、诊断日志事件）。
    测试用例由 JSON 配置文件维护，支持两种写法：
      - 紧凑字符串： "seg:nihaoma=nihao"（kind:keys[=expected]）
      - 结构化对象： preset（seg/bs/retreat/segleft/shiftseg/shiftbs/hold）+ text，
                    或 steps 通用步骤列表（text/key/chord/paste/repeat/wait/shiftKey）

    覆盖的是 CI 覆盖不到的 TSF 按键链路：按段删除、退选、长按守卫、模式退化等。
    它是手工回归的加速器，不替代物理按键验证（见 README 的「已知注入伪影」）。

.PARAMETER ConfigFile
    JSON 配置路径。字段（CLI 同名参数优先）：
      host / hostArguments / imeToggle / reuseHost / reportPath / diagnosticLog / cases
    cases 是数组，元素可以是紧凑字符串或结构化对象。

.PARAMETER Tag
    只跑带该 tag 的用例（可多个）。未给时跑全部。

.PARAMETER DryRun
    只解析配置并打印用例与展开后的步骤，不启动宿主、不注入按键。用于校验配置。

.PARAMETER Cases / CasesFile
    兼容入口：直接给紧凑用例，不用配置文件。

.PARAMETER HostPath / HostArguments / ReportPath / DiagnosticLogPath
    覆盖配置里的对应字段。

.PARAMETER ReuseHost / SkipImeToggle
    覆盖配置里的 reuseHost / imeToggle。

.NOTES
    退出码：0 = 全部通过；1 = 存在断言失败；2 = 环境/焦点问题（不是产品结论）。
#>
[CmdletBinding()]
param(
    [string]$ConfigFile,
    [string[]]$Tag = @(),
    [switch]$DryRun,
    [string[]]$Cases = @(),
    [string]$CasesFile,
    [string]$HostPath,
    [string[]]$HostArguments,
    [string]$ReportPath,
    [string]$DiagnosticLogPath,
    # 水杉自己的中英文切换键在 config.toml 的 keybindings.switch_language_*；
    # Ctrl+Space 是 Windows 系统切换输入法，不是水杉的中英文开关。
    [string]$ImeConfigPath = (Join-Path $env:LOCALAPPDATA 'metasequoiaime\config.toml'),
    [switch]$ReuseHost,
    [switch]$SkipImeToggle
)

$ErrorActionPreference = 'Stop'

# 允许 -Tag a,b（单个逗号串）与数组两种写法。
$Tag = @($Tag -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne '' })

# ---------------------------------------------------------------- 配置加载

function Read-JsonFile([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "配置文件不存在：$Path" }
    $raw = Get-Content -LiteralPath $Path -Raw
    try { return ($raw | ConvertFrom-Json) }
    catch { throw "配置文件不是合法 JSON：$Path`n$($_.Exception.Message)" }
}

$config = $null
if ($ConfigFile) { $config = Read-JsonFile $ConfigFile }

$hostExe = if ($HostPath) { $HostPath } elseif ($config -and $config.host) { [string]$config.host } else { '' }
if (-not $hostExe) { $hostExe = '' }
# host 留空 = 先自动探测本机编辑器（notepad/notepad4/Notepad++/write/wordpad），
# 都没有才用自带的 WinForms 宿主（TsfTestHost.ps1，由本机 pwsh 以子进程启动）。
$useBuiltinHost = $false
if (-not $hostExe) {
    # 探测顺序：PATH 里的编辑器（跳过 scoop/其他 shim——它们是启动即退出的壳），
    # 常见安装目录，scoop apps 下的真实 exe；都没有才退回自带 WinForms 宿主。
    $candidates = New-Object System.Collections.Generic.List[string]
    foreach ($name in @('notepad.exe', 'notepad4.exe', 'Notepad++.exe', 'write.exe', 'wordpad.exe')) {
        $found = Get-Command $name -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($found -and $found.Source -and $found.Source -notlike '*\shims\*') { $candidates.Add($found.Source) }
    }
    if ($env:ProgramFiles) { $candidates.Add((Join-Path $env:ProgramFiles 'Notepad++\notepad++.exe')) }
    if (${env:ProgramFiles(x86)}) { $candidates.Add((Join-Path ${env:ProgramFiles(x86)} 'Notepad++\notepad++.exe')) }
    $scoopApps = Join-Path $env:USERPROFILE 'scoop\apps'
    if (Test-Path -LiteralPath $scoopApps) {
        foreach ($app in @('notepad4', 'notepad++')) {
            $hit = Get-ChildItem -Path (Join-Path $scoopApps "$app\*\*.exe") -ErrorAction SilentlyContinue |
                Where-Object { $_.Name -notmatch 'matepath' } | Select-Object -First 1
            if ($hit) { $candidates.Add($hit.FullName) }
        }
    }
    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path -LiteralPath $candidate -PathType Leaf)) { $hostExe = $candidate; break }
    }
}
if ($hostExe -eq '') { $useBuiltinHost = $true }
$hostArgs = if ($PSBoundParameters.ContainsKey('HostArguments')) { $HostArguments }
            elseif ($config -and $config.hostArguments) { @($config.hostArguments) } else { @() }
if ($useBuiltinHost) {
    # 用当前进程的真实可执行文件，而不是 PATH 里的 pwsh（可能是 scoop shim：
    # shim 启动完真身就退出，会得到一个没有窗口的死进程）。
    $hostExe = [System.Diagnostics.Process]::GetCurrentProcess().MainModule.FileName
    if (-not $hostExe) { throw '找不到 pwsh 可执行文件；用 -HostPath 指定外部宿主。' }
    $handleFile = Join-Path $env:TEMP 'tsf-harness-host-handle.txt'
    $focusFile = Join-Path $env:TEMP 'tsf-harness-host-focus.txt'
    Remove-Item -LiteralPath $handleFile -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $focusFile -ErrorAction SilentlyContinue
    $hostArgs = @('-NoProfile', '-NoLogo', '-STA', '-WindowStyle', 'Hidden', '-File', (Join-Path $PSScriptRoot 'TsfTestHost.ps1'), '-HandleFile', $handleFile, '-FocusFile', $focusFile)
}
$useReuseHost = if ($PSBoundParameters.ContainsKey('ReuseHost')) { [bool]$ReuseHost }
                elseif ($config -and $config.PSObject.Properties.Name -contains 'reuseHost') { [bool]$config.reuseHost } else { $false }
$useImeToggle = if ($PSBoundParameters.ContainsKey('SkipImeToggle')) { -not [bool]$SkipImeToggle }
                elseif ($config -and $config.PSObject.Properties.Name -contains 'imeToggle') { [bool]$config.imeToggle } else { $true }
$diagLog = if ($DiagnosticLogPath) { $DiagnosticLogPath } elseif ($config -and $config.diagnosticLog) { [string]$config.diagnosticLog } else { '' }
$report = if ($ReportPath) { $ReportPath } elseif ($config -and $config.reportPath) { [string]$config.reportPath }
          else { Join-Path $env:TEMP 'tsf-key-behavior-report.txt' }

$rawCases = New-Object System.Collections.Generic.List[object]
if ($config -and $config.cases) { foreach ($c in @($config.cases)) { $rawCases.Add($c) } }
if ($CasesFile) {
    if (-not (Test-Path -LiteralPath $CasesFile -PathType Leaf)) { throw "用例文件不存在：$CasesFile" }
    foreach ($line in Get-Content -LiteralPath $CasesFile) {
        if ($line.Trim() -ne '' -and -not $line.TrimStart().StartsWith('#')) { $rawCases.Add($line.Trim()) }
    }
}
foreach ($c in $Cases) { $rawCases.Add($c) }
if ($rawCases.Count -eq 0) { throw '没有用例：用 -ConfigFile / -Cases / -CasesFile 传入。' }

# ---------------------------------------------------------------- 用例模型

$VK = @{
    ENTER = 0x0D; BACKSPACE = 0x08; SPACE = 0x20; TAB = 0x09; ESC = 0x1B
    LEFT = 0x25; RIGHT = 0x27; UP = 0x26; DOWN = 0x28; DELETE = 0x2E
    HOME = 0x24; END = 0x23
}
$MOD = @{ CTRL = 0x11; SHIFT = 0x10; ALT = 0x12 }

function Expand-Preset([string]$Preset, [string]$Text) {
    switch ($Preset) {
        'probe' { return @(@{ type = 'text'; value = $Text }) }
        'seg' { return @(@{ type = 'text'; value = $Text }, @{ type = 'chord'; key = 'BACKSPACE'; mods = @('CTRL') }, @{ type = 'key'; key = 'ENTER' }) }
        'bs' { return @(@{ type = 'text'; value = $Text }, @{ type = 'key'; key = 'BACKSPACE' }, @{ type = 'key'; key = 'ENTER' }) }
        'retreat' { return @(@{ type = 'text'; value = $Text }, @{ type = 'key'; key = 'SPACE' }, @{ type = 'key'; key = 'BACKSPACE' }, @{ type = 'key'; key = 'ENTER' }) }
        'segleft' { return @(@{ type = 'text'; value = $Text }, @{ type = 'chord'; key = 'LEFT'; mods = @('CTRL') }, @{ type = 'text'; value = 'x' }, @{ type = 'key'; key = 'ENTER' }) }
        'shiftseg' { return @(@{ type = 'shiftKey'; key = $Text.Substring(0, 1) }, @{ type = 'text'; value = $Text.Substring(1) }, @{ type = 'chord'; key = 'BACKSPACE'; mods = @('CTRL') }, @{ type = 'key'; key = 'ENTER' }) }
        'shiftbs' { return @(@{ type = 'shiftKey'; key = $Text.Substring(0, 1) }, @{ type = 'text'; value = $Text.Substring(1) }, @{ type = 'key'; key = 'BACKSPACE' }, @{ type = 'key'; key = 'ENTER' }) }
        'hold' {
            $steps = @()
            $bar = $Text.IndexOf('|')
            if ($bar -ge 0) {
                $steps += @{ type = 'paste'; value = $Text.Substring(0, $bar) }
                $steps += @{ type = 'text'; value = $Text.Substring($bar + 1) }
            }
            else { $steps += @{ type = 'text'; value = $Text } }
            $steps += @{ type = 'repeat'; key = 'BACKSPACE'; count = 20; intervalMs = 35 }
            return $steps
        }
        default { throw "未知 preset：$Preset（可用：probe/seg/bs/retreat/segleft/shiftseg/shiftbs/hold）" }
    }
}

function Convert-Case($raw, [int]$index) {
    # 紧凑字符串："kind:keys[=expected]"
    if ($raw -is [string]) {
        $spec = $raw.Trim()
        $expected = $null; $hasExpectation = $false
        $eq = $spec.IndexOf('=')
        if ($eq -ge 0) { $hasExpectation = $true; $expected = $spec.Substring($eq + 1); $spec = $spec.Substring(0, $eq) }
        $colon = $spec.IndexOf(':')
        if ($colon -lt 0) { throw "用例缺少 ':'：$raw" }
        $preset = $spec.Substring(0, $colon)
        $text = $spec.Substring($colon + 1)
        return [pscustomobject]@{
            name = "$preset($text)"; tags = @(); skip = ''
            steps = Expand-Preset $preset $text
            expect = $expected; hasExpectation = $hasExpectation
            diag = $null
        }
    }

    $name = if ($raw.PSObject.Properties.Name -contains 'name' -and $raw.name) { [string]$raw.name } else { "case$index" }
    $tags = if ($raw.PSObject.Properties.Name -contains 'tags' -and $raw.tags) { @($raw.tags) } else { @() }
    $skip = if ($raw.PSObject.Properties.Name -contains 'skip' -and $raw.skip) { [string]$raw.skip } else { '' }

    $steps = @()
    if ($raw.PSObject.Properties.Name -contains 'steps' -and $raw.steps) { $steps = @($raw.steps) }
    elseif ($raw.PSObject.Properties.Name -contains 'preset') {
        if (-not ($raw.PSObject.Properties.Name -contains 'text')) { throw "$name：preset 需要 text" }
        $steps = Expand-Preset ([string]$raw.preset) ([string]$raw.text)
    }
    else { throw "$name：需要 preset+text 或 steps" }

    $expected = $null; $hasExpectation = $false; $diag = $null
    if ($raw.PSObject.Properties.Name -contains 'expect' -and $null -ne $raw.expect) {
        $hasExpectation = $true
        if ($raw.expect -is [string]) { $expected = $raw.expect }
        else {
            if ($raw.expect.PSObject.Properties.Name -contains 'equals') { $expected = [string]$raw.expect.equals }
            elseif ($raw.expect.PSObject.Properties.Name -contains 'regex') { $expected = @{ regex = [string]$raw.expect.regex } }
            elseif ($raw.expect.PSObject.Properties.Name -contains 'contains') { $expected = @{ contains = [string]$raw.expect.contains } }
            else { $expected = '' }
            if ($raw.expect.PSObject.Properties.Name -contains 'diag') { $diag = $raw.expect.diag }
        }
    }
    return [pscustomobject]@{
        name = $name; tags = $tags; skip = $skip
        steps = $steps; expect = $expected; hasExpectation = $hasExpectation; diag = $diag
    }
}

$caseList = New-Object 'System.Collections.Generic.List[object]'
$i = 0
foreach ($raw in $rawCases) { $i++; $caseList.Add((Convert-Case $raw $i)) }

if ($Tag.Count -gt 0) {
    $filtered = New-Object 'System.Collections.Generic.List[object]'
    foreach ($c in $caseList) {
        foreach ($t in @($c.tags)) { if ($Tag -contains $t) { $filtered.Add($c); break } }
    }
    if ($filtered.Count -eq 0) { throw "没有匹配 tag 的用例：$($Tag -join ', ')" }
    $caseList = $filtered
}

if ($DryRun) {
    Write-Output "config: $ConfigFile"
    Write-Output "host: $(if ($useBuiltinHost) { 'builtin (WinForms TextBox)' } else { $hostExe })"
    Write-Output "tags: $($Tag -join ', ')"
    foreach ($c in $caseList) {
        $labels = foreach ($step in $c.steps) {
            $arg = if ($step.key) { $step.key } elseif ($step.value) { $step.value } else { '' }
            "$($step.type):$arg"
        }
        $expectText = if (-not $c.hasExpectation) { '(未断言)' }
                      elseif ($c.expect -is [hashtable]) { (($c.expect.Keys | Select-Object -First 1)) + ':' + (($c.expect.Values | Select-Object -First 1)) }
                      else { $c.expect }
        $diagText = if ($c.diag) { ' diag=' + $c.diag.event } else { '' }
        Write-Output ("- {0}  tags=[{1}]  steps=[{2}]  expect={3}{4}" -f $c.name, ($c.tags -join ','), ($labels -join ' -> '), $expectText, $diagText)
    }
    exit 0
}

if (-not $useBuiltinHost -and -not (Test-Path -LiteralPath $hostExe -PathType Leaf)) { throw "宿主不存在：$hostExe" }

# ---------------------------------------------------------------- 注入与宿主

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Kbd {
  [StructLayout(LayoutKind.Sequential)] public struct KEYBDINPUT { public ushort wVk; public ushort wScan; public uint dwFlags; public uint time; public IntPtr dwExtraInfo; }
  [StructLayout(LayoutKind.Sequential)] public struct MOUSEINPUT { public int dx; public int dy; public uint mouseData; public uint dwFlags; public uint time; public IntPtr dwExtraInfo; }
  [StructLayout(LayoutKind.Sequential)] public struct HARDWAREINPUT { public uint uMsg; public ushort wParamL; public ushort wParamH; }
  [StructLayout(LayoutKind.Explicit)] public struct INPUTUNION { [FieldOffset(0)] public KEYBDINPUT ki; [FieldOffset(0)] public MOUSEINPUT mi; [FieldOffset(0)] public HARDWAREINPUT hi; }
  [StructLayout(LayoutKind.Sequential)] public struct INPUT { public uint type; public INPUTUNION u; }
  [DllImport("user32.dll", SetLastError=true)] static extern uint SendInput(uint n, INPUT[] inputs, int cb);
  [DllImport("user32.dll")] public static extern short VkKeyScan(char c);
  [DllImport("user32.dll")] static extern uint MapVirtualKey(uint code, uint mapType);
  const uint KEYUP = 0x0002, KEYBOARD = 1;
  static INPUT Ki(ushort vk, bool up) { var i = new INPUT(); i.type = KEYBOARD; i.u.ki.wVk = vk; i.u.ki.wScan = (ushort)MapVirtualKey(vk, 0); i.u.ki.dwFlags = up ? KEYUP : 0; i.u.ki.dwExtraInfo = IntPtr.Zero; return i; }
  public static void Down(ushort vk) { var a = new INPUT[]{ Ki(vk,false) }; SendInput(1, a, Marshal.SizeOf(typeof(INPUT))); }
  public static void Up(ushort vk) { var a = new INPUT[]{ Ki(vk,true) }; SendInput(1, a, Marshal.SizeOf(typeof(INPUT))); }
  public static void Tap(ushort vk) { Down(vk); Up(vk); }
  public static void Chord(ushort mod, ushort vk) { Down(mod); Down(vk); Up(vk); Up(mod); }
  public static void Chord2(ushort mod1, ushort mod2, ushort vk) { Down(mod1); Down(mod2); ThreadSleep(); Down(vk); Up(vk); ThreadSleep(); Up(mod2); Up(mod1); }
  static void SleepMs(int ms) { System.Threading.Thread.Sleep(ms); }
  static void ThreadSleep() { SleepMs(200); }
  public static void Text(string s) {
    foreach (char c in s) {
      if (c == ' ') { Tap(0x20); continue; }
      if (c == ';') { Tap(0xBA); continue; }
      if (c == ((char)39)) { Tap(0xDE); continue; }
      var vk = VkKeyScan(c);
      if (vk == -1) continue;
      Tap((ushort)(vk & 0xFF));
    }
  }
}
"@
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Fg {
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
}
"@
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName Microsoft.VisualBasic

# 水杉自己的中英文切换键：读用户配置，缺省按出厂值（Shift / Ctrl+Alt+Space 开，Ctrl 关）。
# 注意 Ctrl+Space 是 Windows 的系统级输入法切换，不能当作水杉的中英文开关。
$script:imeSwitchToggles = New-Object System.Collections.Generic.List[string]
try {
    $cfgText = if (Test-Path -LiteralPath $ImeConfigPath) { Get-Content -LiteralPath $ImeConfigPath -Raw } else { '' }
    function Get-CfgBool([string]$text, [string]$key, [bool]$fallback) {
        $m = [regex]::Match($text, "(?m)^\s*" + [regex]::Escape($key) + "\s*=\s*(true|false)\s*$")
        if ($m.Success) { return ($m.Groups[1].Value -eq 'true') }
        return $fallback
    }
    if (Get-CfgBool $cfgText 'switch_language_ctrl_alt_space' $true) { $script:imeSwitchToggles.Add('ctrl_alt_space') }
    if (Get-CfgBool $cfgText 'switch_language_shift' $true) { $script:imeSwitchToggles.Add('shift') }
    if (Get-CfgBool $cfgText 'switch_language_ctrl' $false) { $script:imeSwitchToggles.Add('ctrl') }
}
catch { }

$script:testHost = $null
$script:chineseHostKeys = New-Object System.Collections.Generic.HashSet[string]

function Start-TestHost {
    if ($useReuseHost -and $script:testHost) { return $script:testHost }
    Stop-TestHost
    $script:hostStderr = Join-Path $env:TEMP 'tsf-harness-host-stderr.txt'
    Remove-Item -LiteralPath $script:hostStderr -ErrorAction SilentlyContinue
    $proc = Start-Process -FilePath $hostExe -ArgumentList $hostArgs -PassThru -WindowStyle Hidden -RedirectStandardError $script:hostStderr
    Start-Sleep -Milliseconds 1400
    $hwnd = [IntPtr]::Zero
    for ($k = 0; $k -lt 25 -and $hwnd -eq [IntPtr]::Zero; $k++) {
        $proc.Refresh()
        $hwnd = $proc.MainWindowHandle
        if ($hwnd -eq [IntPtr]::Zero) { Start-Sleep -Milliseconds 200 }
    }
    if ($useBuiltinHost) {
        # 等宿主把真实窗体句柄写出来（比 Process.MainWindowHandle 可靠：
        # -WindowStyle Hidden + 重定向 stderr 时它可能一直为 0）。
        for ($k = 0; $k -lt 80 -and $hwnd -eq [IntPtr]::Zero; $k++) {
            if (Test-Path -LiteralPath $handleFile) {
                $parts = (Get-Content -LiteralPath $handleFile -Raw).Trim() -split '\s+'
                if ($parts.Count -ge 2) { $hwnd = [IntPtr]([Convert]::ToInt64($parts[1].TrimStart('0x'), 16)) }
            }
            if ($hwnd -eq [IntPtr]::Zero) { Start-Sleep -Milliseconds 100 }
        }
    }
    $script:testHost = [pscustomobject]@{ Kind = $(if ($useBuiltinHost) { 'builtin' } else { 'external' }); Pid = $proc.Id; Hwnd = $hwnd; Proc = $proc }
    return $script:testHost
}

function Stop-TestHost {
    if (-not $script:testHost) { return }
    Stop-Process -Id $script:testHost.Pid -Force -ErrorAction SilentlyContinue
    $script:testHost = $null
}

function Get-HostWindowHandle {
    if (-not $script:testHost) { return [IntPtr]::Zero }
    if ($script:testHost.Hwnd -ne [IntPtr]::Zero) { return [IntPtr]$script:testHost.Hwnd }
    try { $script:testHost.Proc.Refresh(); return $script:testHost.Proc.MainWindowHandle } catch { return [IntPtr]::Zero }
}

# 内置宿主：touch 聚焦文件，让宿主把窗体拉回前台并把焦点放回文本框。
function Request-HostFocus {
    if (-not $useBuiltinHost -or -not $script:testHost) { return }
    Set-Content -LiteralPath $focusFile -Value ([DateTime]::UtcNow.Ticks) -Encoding ascii
    Start-Sleep -Milliseconds 250
}

function Test-HostForeground {
    if (-not $script:testHost) { return $false }
    $h = Get-HostWindowHandle
    if ($h -eq [IntPtr]::Zero) { return $false }
    return ([Fg]::GetForegroundWindow() -eq $h)
}

# 注入键会打到当前前台窗口：每个用例与每个步骤前都确认宿主在前台，
# 否则结果混入其它窗口（典型症状是用例文本互相粘连）。已在前台时零开销返回。
function Ensure-Foreground {
    if (Test-HostForeground) { return $true }
    if ($useBuiltinHost) { Request-HostFocus }
    for ($n = 0; $n -lt 20; $n++) {
        $h = Get-HostWindowHandle
        if ($h -eq [IntPtr]::Zero) { Start-Sleep -Milliseconds 150; continue }
        [Fg]::ShowWindow($h, 5) | Out-Null
        [Microsoft.VisualBasic.Interaction]::AppActivate($script:testHost.Pid) | Out-Null
        [Fg]::SetForegroundWindow($h) | Out-Null
        Start-Sleep -Milliseconds 150
        if (Test-HostForeground) { return $true }
    }
    return $false
}

function Read-All {
    [System.Windows.Forms.SendKeys]::SendWait('^a'); Start-Sleep -Milliseconds 180
    [System.Windows.Forms.SendKeys]::SendWait('^c'); Start-Sleep -Milliseconds 350
    (Get-Clipboard -Raw)
}

function Clear-Doc {
    [System.Windows.Forms.SendKeys]::SendWait('^a'); Start-Sleep -Milliseconds 150
    [Kbd]::Tap(0x2E); Start-Sleep -Milliseconds 250
}

# 每个宿主进程只校准一次：用 `ni ` 探针判断当前是否中文态（真机默认可能是 english，
# 键盘关闭时按键完全不进 TSF）；不是中文就按 Ctrl+Space（系统级开关）。
# 中英文态探针：输入 `ni ` 看是否上屏汉字。
function Test-ChineseComposition {
    Clear-Doc
    [Kbd]::Text('ni '); Start-Sleep -Milliseconds 700
    $probe = Read-All
    if ($env:TSF_HARNESS_DEBUG) { Write-Host ("[debug] probe=[" + ($probe -replace "`r?`n", "\n") + "]") }
    return ($probe -match '[\u4e00-\u9fff]')
}

# 按下水杉自己的中英文切换键（Ctrl+Space 只是系统级输入法切换，不算）。
function Invoke-ImeModeToggle([string]$Toggle) {
    switch ($Toggle) {
        'ctrl_alt_space' { [Kbd]::Chord2(0x11, 0x12, 0x20) }
        'shift' { [Kbd]::Tap(0x10) }
        'ctrl' { [Kbd]::Tap(0x11) }
    }
    Start-Sleep -Milliseconds 600
    Request-HostFocus
}

# 校准顺序：已是中文 → 水杉自己的切换键（按配置启用的）→ 系统 Ctrl+Space 激活/切换输入法 →
# 再试一次水杉切换键。都失败则不标记，预检/调用方会报 HOST_NOT_CHINESE。
function Enter-ChineseMode {
    if (-not $useImeToggle) { return }
    $key = "$($script:testHost.Kind):$($script:testHost.Pid)"
    if ($script:chineseHostKeys.Contains($key)) { return }
    Request-HostFocus
    if (Test-ChineseComposition) { [void]$script:chineseHostKeys.Add($key); Clear-Doc; return }
    foreach ($toggle in $script:imeSwitchToggles) {
        Invoke-ImeModeToggle $toggle
        if (Test-ChineseComposition) { [void]$script:chineseHostKeys.Add($key); Clear-Doc; return }
    }
    # 系统级：切换到本输入法（Windows 标准快捷键）。
    [Kbd]::Chord(0x11, 0x20); Start-Sleep -Milliseconds 800; Request-HostFocus
    if (Test-ChineseComposition) { [void]$script:chineseHostKeys.Add($key); Clear-Doc; return }
    foreach ($toggle in $script:imeSwitchToggles) {
        Invoke-ImeModeToggle $toggle
        if (Test-ChineseComposition) { [void]$script:chineseHostKeys.Add($key); Clear-Doc; return }
    }
    Request-HostFocus
    Clear-Doc
}

# 修饰键必须按住跨越目标键：TSF 用 GetAsyncKeyState 在按键处理时采样，
# 一次连发会让 Ctrl+Backspace 退化成普通 Backspace。
function Invoke-Chord([string]$Key, [string[]]$Mods) {
    $vk = Resolve-Key $Key
    $modVks = @($Mods | ForEach-Object { $MOD[$_.ToUpper()] })
    foreach ($m in $modVks) {
        if ($null -eq $m) { throw "未知修饰键：$Mods" }
        [Kbd]::Down([ushort]$m); Start-Sleep -Milliseconds 120
    }
    Start-Sleep -Milliseconds 80
    [Kbd]::Down([ushort]$vk); Start-Sleep -Milliseconds 60; [Kbd]::Up([ushort]$vk)
    Start-Sleep -Milliseconds 200
    for ($j = $modVks.Count - 1; $j -ge 0; $j--) { [Kbd]::Up([ushort]$modVks[$j]); Start-Sleep -Milliseconds 80 }
    Start-Sleep -Milliseconds 120
}

function Resolve-Key([string]$Key) {
    $upper = $Key.ToUpper()
    if ($VK.ContainsKey($upper)) { return $VK[$upper] }
    if ($Key.Length -eq 1) {
        $scan = [Kbd]::VkKeyScan($Key)
        if ($scan -ne -1) { return ($scan -band 0xFF) }
    }
    throw "未知按键：$Key"
}

function Invoke-Step($step) {
    if (-not (Ensure-Foreground)) { throw 'FOCUS_FAIL' }
    switch ($step.type) {
        'text' { [Kbd]::Text([string]$step.value); Start-Sleep -Milliseconds 500 }
        'key' { [Kbd]::Tap([ushort](Resolve-Key ([string]$step.key))); Start-Sleep -Milliseconds 300 }
        'chord' { Invoke-Chord ([string]$step.key) @($step.mods) }
        'shiftKey' {
            [Kbd]::Down(0x10); [Kbd]::Tap([ushort](Resolve-Key ([string]$step.key))); [Kbd]::Up(0x10)
            Start-Sleep -Milliseconds 400
        }
        'paste' {
            Set-Clipboard -Value ([string]$step.value); Start-Sleep -Milliseconds 200
            [System.Windows.Forms.SendKeys]::SendWait('^v'); Start-Sleep -Milliseconds 400
        }
        'repeat' {
            $vk = [ushort](Resolve-Key ([string]$step.key))
            $count = if ($step.count) { [int]$step.count } else { 20 }
            $interval = if ($step.intervalMs) { [int]$step.intervalMs } else { 35 }
            for ($k = 0; $k -lt $count; $k++) { [Kbd]::Down($vk); Start-Sleep -Milliseconds $interval }
            [Kbd]::Up($vk); Start-Sleep -Milliseconds 800
        }
        'wait' { Start-Sleep -Milliseconds ([int]$step.ms) }
        default { throw "未知步骤类型：$($step.type)" }
    }
}

function Test-Expectation($case) {
    # 返回 @{ ok = bool; observed = string; note = string }
    $observed = Read-All
    $observed = $observed -replace "`r?`n", "`n"
    $ok = $true
    $note = ''
    if ($case.hasExpectation) {
        if ($case.expect -is [hashtable]) {
            if ($case.expect.ContainsKey('regex')) { $ok = ($observed -match $case.expect.regex) }
            elseif ($case.expect.ContainsKey('contains')) { $ok = ($observed.Contains($case.expect.contains)) }
        }
        else { $ok = ($observed -eq ([string]$case.expect -replace "`r?`n", "`n")) }
    }
    if ($case.diag) {
        $count = Get-DiagEventCount $case.diag.event $script:diagStartLine
        $min = if ($case.diag.minCount) { [int]$case.diag.minCount } else { 1 }
        $note = " [diag $($case.diag.event)=$count]"
        if ($count -lt $min) { $ok = $false; $note += " (期望 >=$min)" }
    }
    return @{ ok = $ok; observed = $observed; note = $note }
}

function Get-DiagLineCount {
    if (-not $diagLog) { return 0 }
    if (-not (Test-Path -LiteralPath $diagLog)) { return 0 }
    return (Get-Content -LiteralPath $diagLog -ReadCount 0 | Measure-Object -Line).Lines
}

function Get-DiagEventCount([string]$Event, [int]$fromLine) {
    if (-not $diagLog) { return 0 }
    if (-not (Test-Path -LiteralPath $diagLog)) { return 0 }
    $lines = Get-Content -LiteralPath $diagLog -ReadCount 0
    if ($fromLine -ge $lines.Count) { return 0 }
    $new = $lines[$fromLine..($lines.Count - 1)]
    return ($new | Select-String -SimpleMatch $Event -AllMatches).Count
}

# ---------------------------------------------------------------- 预检

$lines = New-Object System.Collections.Generic.List[string]
$lines.Add("# TSF key behavior report  $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')")
$lines.Add("# host: $(if ($useBuiltinHost) { 'builtin (WinForms TextBox)' } else { $hostExe })")
$lines.Add("# config: $ConfigFile")
if ($diagLog) { $lines.Add("# diagnostic log: $diagLog") }

$pre = Start-TestHost
if (-not (Ensure-Foreground)) { $lines.Add('# FOCUS_FAIL_AT_PREFLIGHT') }
# 先把宿主输入语言切到 zh-CN：Ctrl+Space 也是系统「切换输入语言」热键，
# 之前的操作可能把语言切到英文，此时宿主不会加载本 DLL。
$zhHkl = 0x08040804
$imeModule = $null
for ($attempt = 0; $attempt -lt 6 -and -not $imeModule; $attempt++) {
    $h = Get-HostWindowHandle
    if ($h -ne [IntPtr]::Zero) { [Fg]::PostMessage($h, 0x0050, [IntPtr]::Zero, [IntPtr]$zhHkl) | Out-Null }
    Start-Sleep -Milliseconds 500
    $imeModule = (Get-Process -Id $script:testHost.Pid -ErrorAction SilentlyContinue).Modules |
        Where-Object { $_.ModuleName -like 'Metasequoia*' } | Select-Object -First 1
    if (-not $imeModule) { Start-Sleep -Milliseconds 250 }
}
if (-not $imeModule) {
    $lines.Add("PREFLIGHT_FAIL: 宿主进程未加载 MetasequoiaImeTsf.dll —— 该宿主没有启用本输入法。")
    $lines.Add("  hwnd=$('0x{0:X}' -f [int64](Get-HostWindowHandle)) foreground=$('0x{0:X}' -f [int64][Fg]::GetForegroundWindow()) host_pid=$($script:testHost.Pid) exited=$($script:testHost.Proc.HasExited)")
    if ($script:hostStderr -and (Test-Path -LiteralPath $script:hostStderr)) {
        $errLines = Get-Content -LiteralPath $script:hostStderr | Select-Object -First 8
        foreach ($e in $errLines) { $lines.Add("  host stderr: $e") }
    }
    $lines.Add('  已尝试切到 zh-CN。请在宿主里手动选中本输入法（或确认它仍是默认中文输入法）后重跑。')
    $lines | Set-Content -LiteralPath $report -Encoding utf8
    Write-Output 'PREFLIGHT_FAIL'
    Write-Output "report: $report"
    Stop-TestHost
    exit 2
}
$lines.Add("# ime dll: $($imeModule.FileName)")
$preKey = "$($script:testHost.Kind):$($script:testHost.Pid)"
if ($useImeToggle) {
    Enter-ChineseMode
    if (-not $script:chineseHostKeys.Contains($preKey)) {
        $lines.Add('PREFLIGHT_FAIL: 宿主无法进入中文组词态。')
        $lines.Add('  注意 Ctrl+Space 是 Windows 的系统级输入法切换，不是本输入法的中英文开关。')
        $lines.Add('  请先在 设置 -> 快捷键 -> 中英文切换 里启用 Shift / Ctrl / Ctrl+Alt+Space 任一')
        $lines.Add('  （或改 config.toml 的 keybindings.switch_language_*），再重跑；')
        $lines.Add('  也可用 -HostPath 指定一个宿主程序。')
        $lines | Set-Content -LiteralPath $report -Encoding utf8
        Write-Output 'PREFLIGHT_FAIL'
        Write-Output "report: $report"
        Stop-TestHost
        exit 2
    }
}
if (-not $useReuseHost) { Stop-TestHost }

# ---------------------------------------------------------------- 执行

$assertFailures = 0
$focusFailures = 0
try {
    foreach ($case in $caseList) {
        if ($case.skip) { $lines.Add("SKIP $($case.name)：$($case.skip)"); continue }
        try {
            Start-TestHost | Out-Null
            if (-not (Ensure-Foreground)) { throw 'FOCUS_FAIL' }
            Enter-ChineseMode
            # 内置宿主必须显式聚焦文本框：前台窗口是窗体，但焦点可能在窗体/控制台上。
            Request-HostFocus
            if (-not (Ensure-Foreground)) { throw 'FOCUS_FAIL' }
            $script:diagStartLine = Get-DiagLineCount
            foreach ($step in $case.steps) { Invoke-Step $step }
            if ($env:TSF_HARNESS_DEBUG) { Write-Host ("[debug] after steps=[" + ((Get-Clipboard -Raw) -replace "`r?`n", "
") + "]") }
            if (-not (Ensure-Foreground)) { throw 'FOCUS_FAIL' }
            $result = Test-Expectation $case
            $expectedText = if ($case.hasExpectation) {
                if ($case.expect -is [hashtable]) { ($case.expect.Keys | Select-Object -First 1) + ':' + ($case.expect.Values | Select-Object -First 1) }
                else { $case.expect }
            } else { '(未断言)' }
            if ($result.ok) { $lines.Add("PASS $($case.name) => [$($result.observed)]$($result.note)") }
            else {
                $assertFailures++
                $lines.Add("FAIL $($case.name) => [$($result.observed)]，期望 [$expectedText]$($result.note)")
            }
        }
        catch {
            if ($_.Exception.Message -eq 'FOCUS_FAIL') {
                $focusFailures++
                $lines.Add("FOCUS_FAIL $($case.name) —— 宿主没拿到前台，本条不是产品结论，请关掉抢焦点窗口后重跑")
            }
            else { $assertFailures++; $lines.Add("ERROR $($case.name)：$($_.Exception.Message)") }
        }
        finally { if (-not $useReuseHost) { Stop-TestHost } }
    }
}
finally { Stop-TestHost }

$lines.Add('')
$lines.Add("# 断言失败=$assertFailures  焦点失败=$focusFailures")
$lines | Set-Content -LiteralPath $report -Encoding utf8
Write-Output "report: $report"
Write-Output "assert failures: $assertFailures, focus failures: $focusFailures"

if ($assertFailures -gt 0) { exit 1 }
if ($focusFailures -gt 0) { exit 2 }
exit 0
