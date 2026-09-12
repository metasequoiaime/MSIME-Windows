# 轻量本地测试：只编译 TSF / Server / 设置页，收集这三块产物，打不含词库的安装包。
#
# 适用：只改了 TSF、Server 或 HTML，本机已有完整安装（词库已在
# %LOCALAPPDATA%\metasequoiaime）。不要用这个脚本做首次安装。
#
# 完整流程（含词库）仍用 .\test.ps1；需要包内带 PDB 时用 .\test-symbols.ps1。
#
# 编译是增量的，实现见 Invoke-LocalTest.ps1。

[CmdletBinding()]
param([switch]$Reconfigure)
$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'Invoke-LocalTest.ps1') -Light -Reconfigure:$Reconfigure
