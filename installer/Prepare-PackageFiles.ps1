[CmdletBinding()]
param(
    [string]$TargetVersion = '0.0.1',
    [string]$RepoRoot = (Split-Path -Parent $PSScriptRoot),
    # Component paths are relative to RepoRoot and default to the consolidated layout.
    # Historical or custom layouts remain available through explicit overrides.
    [string]$TsfDirectory = 'windows',
    [string]$ServerDirectory = 'server',
    [string]$UiHtmlDirectory = 'ui-html',
    [string]$HelpCodeDirectory = 'vendor/MetasequoiaImeEngine/helpcode',
    [string]$DictionaryDirectory = 'MetasequoiaImeDict',
    # THIRD_PARTY_NOTICES.txt used to sit next to the tip's sources. In the consolidated repository
    # the notice covers the whole product and lives at the root, one level above windows/, so where
    # to read it is no longer answered by where the tip is.
    [string]$NoticesDirectory = '.',
    [switch]$Light
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Assert-PathExists {
    param([Parameter(Mandatory)][string]$LiteralPath, [Parameter(Mandatory)][string]$Description)
    if (-not (Test-Path -LiteralPath $LiteralPath)) {
        throw "$Description 不存在：$LiteralPath"
    }
}

function Copy-DirectoryContents {
    param([Parameter(Mandatory)][string]$Source, [Parameter(Mandatory)][string]$Destination)
    Assert-PathExists -LiteralPath $Source -Description '源目录'
    New-Item -ItemType Directory -Path $Destination -Force | Out-Null
    Get-ChildItem -LiteralPath $Source -Force | Copy-Item -Destination $Destination -Recurse -Force
}

function Reset-Directory {
    param([Parameter(Mandatory)][string]$LiteralPath)
    if (Test-Path -LiteralPath $LiteralPath) {
        Remove-Item -LiteralPath $LiteralPath -Recurse -Force
    }
    New-Item -ItemType Directory -Path $LiteralPath -Force | Out-Null
}

$serverRelease = Join-Path $RepoRoot (Join-Path $ServerDirectory 'build-release\bin\Release')
$dictionaryReplayRelease = Join-Path $serverRelease 'MetasequoiaImeDictionaryReplay.exe'
$tsf32Release = Join-Path $RepoRoot (Join-Path $TsfDirectory 'build32-release\Release\MetasequoiaImeTsf.dll')
$tsf64Release = Join-Path $RepoRoot (Join-Path $TsfDirectory 'build64-release\Release\MetasequoiaImeTsf.dll')
$tsf32Pdb = Join-Path $RepoRoot (Join-Path $TsfDirectory 'build32-release\Release\MetasequoiaImeTsf.pdb')
$tsf64Pdb = Join-Path $RepoRoot (Join-Path $TsfDirectory 'build64-release\Release\MetasequoiaImeTsf.pdb')
$webviewRoot = Join-Path $RepoRoot (Join-Path $UiHtmlDirectory 'webview2')
$serverConfig = Join-Path $RepoRoot (Join-Path $ServerDirectory 'assets\config\config.toml')
$factoryConfig = Join-Path $PSScriptRoot 'default_config\config.default.toml'
$pinyinTable = Join-Path $RepoRoot (Join-Path $ServerDirectory 'assets\tables\pinyin.txt')
$helpcodeSource = Join-Path $RepoRoot (Join-Path $HelpCodeDirectory 'helpcodes')
$appIcon = Join-Path $RepoRoot (Join-Path $ServerDirectory 'src\resource\MetasequoiaIME.ico')
$thirdPartyNotices = Join-Path $RepoRoot (Join-Path $NoticesDirectory 'THIRD_PARTY_NOTICES.txt')
$license = Join-Path $RepoRoot 'LICENSE'
$dictionaryDb = Join-Path $RepoRoot (Join-Path $DictionaryDirectory 'out\msime.db')
$dictionaryManifest = Join-Path $RepoRoot (Join-Path $DictionaryDirectory 'out\dictionary-manifest.json')
$japaneseModel = Join-Path $RepoRoot (Join-Path $DictionaryDirectory 'out\dict_japanese.dat')
$japaneseModelLicense = Join-Path $RepoRoot (Join-Path $DictionaryDirectory 'source\mozc_dictionary_oss\README.txt')
$englishDb = Join-Path $RepoRoot (Join-Path $DictionaryDirectory 'out\english.db')
$othersDb = Join-Path $RepoRoot (Join-Path $DictionaryDirectory 'out\others.db')

Assert-PathExists -LiteralPath $RepoRoot -Description '源码仓库根目录'
Assert-PathExists -LiteralPath $serverRelease -Description 'Server Release 输出目录'
Assert-PathExists -LiteralPath $dictionaryReplayRelease -Description '用户词库回放程序 Release EXE'
Assert-PathExists -LiteralPath $tsf32Release -Description '32 位 TSF Release DLL'
Assert-PathExists -LiteralPath $tsf64Release -Description '64 位 TSF Release DLL'
Assert-PathExists -LiteralPath $tsf32Pdb -Description '32 位 TSF Release PDB'
Assert-PathExists -LiteralPath $tsf64Pdb -Description '64 位 TSF Release PDB'
$serverExecutables = @(
    Get-ChildItem -LiteralPath $serverRelease -Recurse -File -Filter '*.exe' |
        Where-Object { $_.BaseName -notlike '*Tests' -and $_.BaseName -notlike 'test_*' }
)
$missingServerPdb = @(
    $serverExecutables |
        Where-Object {
            -not (Test-Path -LiteralPath (Join-Path $_.DirectoryName "$($_.BaseName).pdb"))
        } |
        ForEach-Object { Join-Path $_.DirectoryName "$($_.BaseName).pdb" }
)
if ($missingServerPdb.Count -gt 0) {
    throw "Server Release 缺少同名 PDB：$($missingServerPdb -join ', ')"
}
Assert-PathExists -LiteralPath $serverConfig -Description 'Server config.toml'
Assert-PathExists -LiteralPath $appIcon -Description '应用图标'
Assert-PathExists -LiteralPath $thirdPartyNotices -Description '第三方声明 THIRD_PARTY_NOTICES.txt'
Assert-PathExists -LiteralPath $license -Description '许可证 LICENSE'
Assert-PathExists -LiteralPath (Join-Path $webviewRoot 'shared') -Description '共享 WebView 消息契约'
Assert-PathExists -LiteralPath (Join-Path $webviewRoot 'candwnd') -Description '候选窗 HTML 目录'
Assert-PathExists -LiteralPath (Join-Path $webviewRoot 'ftb') -Description '悬浮工具栏 HTML 目录'
Assert-PathExists -LiteralPath (Join-Path $webviewRoot 'menu') -Description '菜单 HTML 目录'
Assert-PathExists -LiteralPath (Join-Path $webviewRoot 'settings\ime-settings\dist') -Description '设置页面 dist 目录'

if (-not $Light) {
    Assert-PathExists -LiteralPath $factoryConfig -Description '出厂配置 default_config\config.default.toml'
    Assert-PathExists -LiteralPath $pinyinTable -Description '完整拼音音节表 pinyin.txt'
    Assert-PathExists -LiteralPath $helpcodeSource -Description '辅助码目录'
    Assert-PathExists -LiteralPath $dictionaryDb -Description '词库数据库 msime.db'
    Assert-PathExists -LiteralPath $japaneseModel -Description '日语整句模型 dict_japanese.dat'
    Assert-PathExists -LiteralPath $japaneseModelLicense -Description 'Mozc 日语词典授权声明'
    Assert-PathExists -LiteralPath $englishDb -Description '英文词库数据库 english.db'
    python -c @"
import sqlite3, sys
cols = list(sqlite3.connect(sys.argv[1]).execute('PRAGMA table_info(english_words)'))
names = {row[1] for row in cols}
pk = [row[1] for row in cols if row[5] > 0]
if 'weight' not in names or pk != ['word', 'display']:
    raise SystemExit('english.db schema is stale; rebuild with weight and PRIMARY KEY(word, display)')
"@ $englishDb
    if ($LASTEXITCODE -ne 0) {
        throw "英文词库数据库 schema 检查失败：$englishDb"
    }
    Assert-PathExists -LiteralPath $othersDb -Description '杂项数据库 others.db'
}

$targetAppData = Join-Path $PSScriptRoot 'app_data'
$targetServer = Join-Path $PSScriptRoot 'server_exe'
$targetTsf = Join-Path $PSScriptRoot 'tsf_dll'

if ($Light) {
    Write-Host '轻量模式：跳过词库、辅助码、拼音表和出厂配置，只刷新 TSF、Server、HTML。'
    New-Item -ItemType Directory -Path $targetAppData -Force | Out-Null
}
else {
    Reset-Directory -LiteralPath $targetAppData
    if (-not (Get-Content -LiteralPath $pinyinTable | Where-Object { $_.Trim() -eq 'xing' })) {
        throw "完整拼音音节表缺少 xing：$pinyinTable"
    }
    Copy-Item -LiteralPath $pinyinTable -Destination (Join-Path $targetAppData 'pinyin.txt') -Force
    Copy-Item -LiteralPath $dictionaryDb -Destination (Join-Path $targetAppData 'msime.db') -Force
    if (Test-Path -LiteralPath $dictionaryManifest) {
        Copy-Item -LiteralPath $dictionaryManifest -Destination (Join-Path $targetAppData 'dictionary-manifest.json') -Force
    }
    Copy-Item -LiteralPath $japaneseModel -Destination (Join-Path $targetAppData 'dict_japanese.dat') -Force
    Copy-Item -LiteralPath $japaneseModelLicense -Destination (Join-Path $targetAppData 'MOZC_DICTIONARY_LICENSE.txt') -Force
    Copy-Item -LiteralPath $englishDb -Destination (Join-Path $targetAppData 'english.db') -Force
    Copy-Item -LiteralPath $othersDb -Destination (Join-Path $targetAppData 'others.db') -Force

    $defaultConfigPath = Join-Path $targetAppData 'config.default.toml'
    # 出厂配置来自本仓库的 default_config，不依赖本机是否已安装输入法。
    # 安装脚本用 onlyifdoesntexist 生成用户 config.toml，升级不会覆盖已有方案/主题。
    Copy-Item -LiteralPath $factoryConfig -Destination $defaultConfigPath -Force
    $defaultConfig = Get-Content -LiteralPath $defaultConfigPath -Raw
    if ($defaultConfig -notmatch '(?m)^schema\s*=\s*"quanpin"\s*$') {
        throw '出厂配置的 input.schema 必须是 quanpin。'
    }
    if ($defaultConfig -notmatch '(?m)^theme_mode\s*=\s*"system"\s*$') {
        throw '出厂配置的 appearance.theme_mode 必须是 system。'
    }
    if ($defaultConfig -match '(?m)^diagnostic_log\s*=\s*true\s*$') {
        throw '出厂配置不应默认打开 diagnostic_log。'
    }
    $defaultConfig = $defaultConfig.TrimEnd("`r", "`n") + "`r`n"
    Set-Content -LiteralPath $defaultConfigPath -Value $defaultConfig -Encoding utf8NoBOM -NoNewline
    foreach ($stagedUserConfig in @('config.toml', 'config.base.toml')) {
        $stagedPath = Join-Path $targetAppData $stagedUserConfig
        if (Test-Path -LiteralPath $stagedPath) {
            Remove-Item -LiteralPath $stagedPath -Force
        }
    }

    $targetHelpcodes = Join-Path $targetAppData 'helpcodes'
    Reset-Directory -LiteralPath $targetHelpcodes
    Copy-DirectoryContents -Source $helpcodeSource -Destination $targetHelpcodes
}

$targetHtml = Join-Path $targetAppData 'html'
if (Test-Path -LiteralPath $targetHtml) {
    Remove-Item -LiteralPath $targetHtml -Recurse -Force
}
$targetWebview = Join-Path $targetHtml 'webview2'
Copy-DirectoryContents -Source (Join-Path $webviewRoot 'shared') -Destination (Join-Path $targetWebview 'shared')
Copy-DirectoryContents -Source (Join-Path $webviewRoot 'candwnd') -Destination (Join-Path $targetWebview 'candwnd')
Copy-DirectoryContents -Source (Join-Path $webviewRoot 'ftb') -Destination (Join-Path $targetWebview 'ftb')
Copy-DirectoryContents -Source (Join-Path $webviewRoot 'menu') -Destination (Join-Path $targetWebview 'menu')
Copy-DirectoryContents -Source (Join-Path $webviewRoot 'settings\ime-settings\dist') `
    -Destination (Join-Path $targetWebview 'settings\ime-settings\dist')

# Server Release 输出整体复制，但测试程序及其 PDB 绝不能进入安装包。
# 其他 PDB 保留在对应 EXE 旁边，方便安装后直接进行崩溃分析。
Reset-Directory -LiteralPath $targetServer
Copy-DirectoryContents -Source $serverRelease -Destination $targetServer
Get-ChildItem -LiteralPath $targetServer -Recurse -File -Filter '*Tests.exe' |
    Remove-Item -Force
Get-ChildItem -LiteralPath $targetServer -Recurse -File -Filter '*Tests.pdb' |
    Remove-Item -Force
Get-ChildItem -LiteralPath $targetServer -Recurse -File -Filter 'test_*.exe' |
    Remove-Item -Force
Get-ChildItem -LiteralPath $targetServer -Recurse -File -Filter 'test_*.pdb' |
    Remove-Item -Force

Reset-Directory -LiteralPath $targetTsf
$targetTsf32 = Join-Path $targetTsf '32'
$targetTsf64 = Join-Path $targetTsf '64'
New-Item -ItemType Directory -Path $targetTsf32, $targetTsf64 -Force | Out-Null
Copy-Item -LiteralPath $tsf32Release -Destination $targetTsf32 -Force
Copy-Item -LiteralPath $tsf32Pdb -Destination $targetTsf32 -Force
Copy-Item -LiteralPath $tsf64Release -Destination $targetTsf64 -Force
Copy-Item -LiteralPath $tsf64Pdb -Destination $targetTsf64 -Force
Copy-Item -LiteralPath $appIcon -Destination (Join-Path $PSScriptRoot 'MetasequoiaIME.ico') -Force
# rime-ice is GPL-3.0 and requires attribution, and its content forms the bulk of msime.db, so the
# notice has to reach the user's disk rather than only exist in the source repository.
Copy-Item -LiteralPath $thirdPartyNotices -Destination (Join-Path $PSScriptRoot 'THIRD_PARTY_NOTICES.txt') -Force
# GPLv3 sections 4 and 6 require a copy of the licence to reach whoever receives the program, and the
# packaged product includes third-party GPL-3.0 dictionary data. macOS and Linux already install the
# licence text (CMakeLists.txt in MSIME-Apple and MSIME-Linux); Windows is the platform that actually
# ships at volume and was the only one omitting it. THIRD_PARTY_NOTICES.txt does not cover this: it
# points at "the LICENSE file" without carrying the GPL text itself.
Copy-Item -LiteralPath $license -Destination (Join-Path $PSScriptRoot 'LICENSE.txt') -Force

$targetIss = Join-Path $PSScriptRoot 'msime_setup.iss'
Assert-PathExists -LiteralPath $targetIss -Description '安装脚本'
$issContent = Get-Content -LiteralPath $targetIss -Raw
if ($issContent -notmatch '(?m)^#define\s+MyAppVersion\s+"[^"]+"\s*$') {
    throw '未能在安装脚本中找到 MyAppVersion。'
}
$updatedIss = [regex]::Replace(
    $issContent,
    '(?m)^#define\s+MyAppVersion\s+"[^"]+"\s*$',
    "#define MyAppVersion   `"$TargetVersion`""
)
$updatedIss = $updatedIss.TrimEnd("`r", "`n") + "`r`n"
Set-Content -LiteralPath $targetIss -Value $updatedIss -Encoding utf8NoBOM -NoNewline

# 设置页是已构建的静态资源，同步其“当前版本”展示。
$settingsDist = Join-Path $targetWebview 'settings\ime-settings\dist'
Get-ChildItem -LiteralPath $settingsDist -Recurse -File -Include '*.js', '*.html' | ForEach-Object {
    $content = Get-Content -LiteralPath $_.FullName -Raw
    $updated = [regex]::Replace(
        $content,
        '(<div class="about-version">)v\d+(?:\.\d+)+(</div>)',
        "`$1v$TargetVersion`$2"
    )
    if ($updated -ne $content) {
        Set-Content -LiteralPath $_.FullName -Value $updated -Encoding utf8NoBOM
    }
}

$serverBinaryCount = @(Get-ChildItem -LiteralPath $targetServer -Recurse -File -Include '*.exe', '*.dll').Count
$tsfBinaryCount = @(Get-ChildItem -LiteralPath $targetTsf -Recurse -File -Include '*.exe', '*.dll').Count
$symbolCount = @(
    Get-ChildItem -LiteralPath $targetServer, $targetTsf -Recurse -File -Filter '*.pdb'
).Count
$modeLabel = if ($Light) { '轻量' } else { '完整' }
Write-Host "安装文件准备完成（$modeLabel）：$PSScriptRoot"
Write-Host "版本：$TargetVersion；Server EXE/DLL：$serverBinaryCount 个；TSF EXE/DLL：$tsfBinaryCount 个；PDB：$symbolCount 个。"
