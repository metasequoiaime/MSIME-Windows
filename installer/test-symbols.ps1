# 带符号的本地测试安装：和 test-light.ps1 / test.ps1 走同一条流水线，但把 PDB 一起打进安装包，
# 安装后 PDB 就躺在对应 EXE / DLL 旁边，调试器能按二进制里内嵌的路径直接找到符号。
#
#   .\test-symbols.ps1          轻量包（不含词库）+ PDB
#   .\test-symbols.ps1 -Full    完整包（含词库）+ PDB
#
# 只在需要对装好的那一份做崩溃分析时才用这个脚本：PDB 有 ~140 MB，而真正的二进制只有 ~20 MB，
# Inno Setup 还要对它们做固实 LZMA2 压缩，打包会明显比 test.ps1 / test-light.ps1 慢。
#
# 编译是增量的，实现见 Invoke-LocalTest.ps1。

[CmdletBinding()]
param(
    # 默认走轻量包，和日常改 TSF / Server / HTML 的节奏一致；要连词库一起装就加 -Full。
    [switch]$Full,
    [switch]$Reconfigure
)
$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'Invoke-LocalTest.ps1') -Light:(-not $Full) -IncludeSymbols -Reconfigure:$Reconfigure
