# 最小 TSF 测试宿主：一个 WinForms 多行文本框。
#
# 供 Invoke-TsfKeyBehaviorTests.ps1 在目标机器没有 notepad / 编辑器时使用：
# 由测试脚本以独立 pwsh 子进程启动（子进程享有"新进程前台权"，能正常拿到焦点），
# 交互与其他外部宿主完全一致 —— 注入按键 + 剪贴板读写。
param([string]$Title = 'TSF key behavior host', [string]$HandleFile, [string]$FocusFile)

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class HostWindow {
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
}
"@

Add-Type @"
using System;
using System.Runtime.InteropServices;
// 让宿主自己把本输入法切成活动输入法：Ctrl+Space 只能切换系统语言，
// 没选中本 TIP 时按键根本不会进 TSF。CLSID/profile GUID 与
// windows/src/Global/Globals.cpp 保持一致。
[ComImport, Guid("1F02B6C5-7842-4EE6-8A0B-9A24183A95CA"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface ITfInputProcessorProfiles {
  void Register();
  void Unregister();
  void AddLanguageProfile(ref Guid rclsid, ushort langid, ref Guid guidProfile, [MarshalAs(UnmanagedType.LPWStr)] string desc, uint cchDesc, [MarshalAs(UnmanagedType.LPWStr)] string iconFile, uint cchIconFile, uint iconIndex);
  void RemoveLanguageProfile(ref Guid rclsid, ushort langid, ref Guid guidProfile);
  void EnumInputProcessorInfo(out IntPtr ppEnum);
  void GetDefaultLanguageProfile(ushort langid, ref Guid catid, out Guid pclsid, out Guid pguidProfile);
  void SetDefaultLanguageProfile(ushort langid, ref Guid rclsid, ref Guid guidProfile);
  void ActivateLanguageProfile(ref Guid rclsid, ushort langid, ref Guid guidProfile);
}
[ComImport, Guid("33C53A50-F456-4884-B049-85FD643ECFED")] internal class InputProcessorProfiles { }
public static class ImeActivator {
  public static void Activate() {
    var profiles = (ITfInputProcessorProfiles)new InputProcessorProfiles();
    var clsid = new Guid("E3062E9A-D834-4637-8958-ED8CFA427D01");
    var profile = new Guid("4D59B1B4-D503-44AE-9259-BAD9BB2778AB");
    profiles.ActivateLanguageProfile(ref clsid, 0x0804, ref profile);
  }
}
"@

$form = New-Object System.Windows.Forms.Form
$form.Text = $Title
$form.Width = 900
$form.Height = 220
$form.StartPosition = 'CenterScreen'

$textBox = New-Object System.Windows.Forms.TextBox
$textBox.Multiline = $true
$textBox.Dock = 'Fill'
$textBox.Font = New-Object System.Drawing.Font('Consolas', 12)
$textBox.AcceptsTab = $true
$form.Controls.Add($textBox)

$form.Show()
# 测试脚本用 -WindowStyle Hidden 启动本进程（避免控制台抢焦点），
# 其 STARTUPINFO 会让首次 Show 也继承隐藏：这里显式覆盖，并把窗体带到前台。
$form.Visible = $true
[HostWindow]::ShowWindow($form.Handle, 9) | Out-Null
$form.ActiveControl = $textBox
$textBox.Focus() | Out-Null
$form.Activate()
[HostWindow]::SetForegroundWindow($form.Handle) | Out-Null
[System.Windows.Forms.Application]::DoEvents()
# 把本输入法切成活动输入法（失败不致命：外部/手动已选好时不需要）。
try { [ImeActivator]::Activate() } catch { Write-Warning "激活输入法失败：$_" }
if ($HandleFile) {
    # 给测试脚本一个不依赖 Process.MainWindowHandle 的句柄来源。
    "$PID 0x$('{0:X}' -f [int64]$form.Handle)" | Set-Content -LiteralPath $HandleFile -Encoding ascii
}

# 按需聚焦：测试脚本每次要注入按键前 touch 一下 $FocusFile，
# 这里把窗体拉回前台并把焦点放回文本框（隐藏控制台 + 各种抢占会让焦点变随机）。
$script:lastFocusStamp = [DateTime]::MinValue
$focusTimer = New-Object System.Windows.Forms.Timer
$focusTimer.Interval = 150
$focusTimer.Add_Tick({
    if (-not (Test-Path -LiteralPath $FocusFile)) { return }
    $stamp = (Get-Item -LiteralPath $FocusFile).LastWriteTimeUtc
    if ($stamp -eq $script:lastFocusStamp) { return }
    $script:lastFocusStamp = $stamp
    $form.Activate()
    $form.ActiveControl = $textBox
    $textBox.Focus() | Out-Null
    [HostWindow]::SetForegroundWindow($form.Handle) | Out-Null
})
$focusTimer.Start()

[System.Windows.Forms.Application]::Run($form)
