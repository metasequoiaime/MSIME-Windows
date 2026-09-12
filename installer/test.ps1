# 本地测试安装流程：编译本仓组件→ 收集安装文件 →
# 本机自签名 → Inno Setup 打包 → 签安装包 → 启动安装程序。
#
# 不使用任何预置证书。签名脚本会在本机生成并复用自签名测试证书。
# 只改 TSF / Server / HTML、且本机已有词库时，用 .\test-light.ps1。
# 需要包内带 PDB 时用 .\test-symbols.ps1 -Full。
#
# 编译是增量的，实现见 Invoke-LocalTest.ps1。

[CmdletBinding()]
param([switch]$Reconfigure)
$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'Invoke-LocalTest.ps1') -Reconfigure:$Reconfigure
