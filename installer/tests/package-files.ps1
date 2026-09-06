$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$fixture = Join-Path ([IO.Path]::GetTempPath()) ('msime-package-' + [Guid]::NewGuid())
function Write-Fixture([string]$Relative, [string]$Text = 'fixture') {
    $path = Join-Path $fixture $Relative
    New-Item -ItemType Directory -Force -Path (Split-Path $path) | Out-Null
    [IO.File]::WriteAllText($path, $Text)
}
try {
    $installer = Join-Path $fixture 'installer'
    New-Item -ItemType Directory -Force -Path $installer | Out-Null
    Copy-Item (Join-Path $PSScriptRoot '../Prepare-PackageFiles.ps1') $installer
    Copy-Item (Join-Path $PSScriptRoot '../msime_setup.iss') $installer
    Copy-Item (Join-Path $PSScriptRoot '../default_config') $installer -Recurse
    foreach ($file in @(
        'server/build-release/bin/Release/MetasequoiaImeServer.exe',
        'server/build-release/bin/Release/MetasequoiaImeServer.pdb',
        'server/build-release/bin/Release/MetasequoiaImeDictionaryReplay.exe',
        'server/build-release/bin/Release/MetasequoiaImeDictionaryReplay.pdb',
        'server/build-release/bin/Release/MetasequoiaImeServerTests.exe',
        'server/build-release/bin/Release/MetasequoiaImeServerTests.pdb',
        'server/build-release/bin/Release/test_webview_contract.exe',
        'server/build-release/bin/Release/test_webview_contract.pdb',
        'windows/build32-release/Release/MetasequoiaImeTsf.dll',
        'windows/build32-release/Release/MetasequoiaImeTsf.pdb',
        'windows/build64-release/Release/MetasequoiaImeTsf.dll',
        'windows/build64-release/Release/MetasequoiaImeTsf.pdb',
        'THIRD_PARTY_NOTICES.txt',
        'LICENSE',
        'server/assets/config/config.toml',
        'server/src/resource/MetasequoiaIME.ico',
        'vendor/MetasequoiaImeEngine/helpcode/helpcodes/helpcode.txt',
        'MetasequoiaImeDict/out/msime.db',
        'MetasequoiaImeDict/out/others.db',
        'MetasequoiaImeDict/out/dict_japanese.dat',
        'MetasequoiaImeDict/source/mozc_dictionary_oss/README.txt',
        'ui-html/webview2/shared/runtime.js',
        'ui-html/webview2/candwnd/index.html',
        'ui-html/webview2/menu/index.html',
        'ui-html/webview2/ftb/index.html',
        'ui-html/webview2/settings/ime-settings/dist/index.html'
    )) { Write-Fixture $file }
    Write-Fixture 'server/assets/tables/pinyin.txt' 'xing'
    Write-Fixture 'MetasequoiaImeDict/out/dictionary-manifest.json' '{"manifest_version":1}'
    $english = Join-Path $fixture 'MetasequoiaImeDict/out/english.db'
    python -c "import sqlite3,sys; sqlite3.connect(sys.argv[1]).execute('CREATE TABLE english_words(word TEXT,display TEXT,weight INTEGER,PRIMARY KEY(word,display))')" $english
    if ($LASTEXITCODE -ne 0) { throw 'Failed to create packaging fixture' }
    & (Join-Path $installer 'Prepare-PackageFiles.ps1') -RepoRoot $fixture -TargetVersion '2026.9.1'
    foreach ($file in @('app_data/html/webview2/shared/runtime.js', 'app_data/dictionary-manifest.json',
                         'tsf_dll/32/MetasequoiaImeTsf.dll', 'tsf_dll/32/MetasequoiaImeTsf.pdb',
                         'tsf_dll/64/MetasequoiaImeTsf.dll', 'tsf_dll/64/MetasequoiaImeTsf.pdb',
                         'server_exe/MetasequoiaImeServer.pdb',
                         'server_exe/MetasequoiaImeDictionaryReplay.pdb',
                         'app_data/helpcodes/helpcode.txt', 'THIRD_PARTY_NOTICES.txt', 'LICENSE.txt')) {
        if (-not (Test-Path (Join-Path $installer $file))) { throw "Missing packaged file: $file" }
    }
    foreach ($testFile in @(
        'server_exe/MetasequoiaImeServerTests.exe',
        'server_exe/MetasequoiaImeServerTests.pdb',
        'server_exe/test_webview_contract.exe',
        'server_exe/test_webview_contract.pdb'
    )) {
        if (Test-Path (Join-Path $installer $testFile)) { throw "Packaged a test file: $testFile" }
    }
    $serverPdbFixture = Join-Path $fixture 'server/build-release/bin/Release/MetasequoiaImeServer.pdb'
    Remove-Item $serverPdbFixture -Force
    $rejected = $false
    try { & (Join-Path $installer 'Prepare-PackageFiles.ps1') -RepoRoot $fixture } catch { $rejected = $_.Exception.Message -match 'PDB' }
    if (-not $rejected) { throw 'Missing production PDB was accepted' }
    [IO.File]::WriteAllText($serverPdbFixture, 'fixture')
    $database = Join-Path $installer 'app_data/msime.db'
    [IO.File]::WriteAllText($database, 'preserved user data')
    & (Join-Path $installer 'Prepare-PackageFiles.ps1') -RepoRoot $fixture -TsfDirectory windows -ServerDirectory server -UiHtmlDirectory ui-html -NoticesDirectory . -Light
    if ([IO.File]::ReadAllText($database) -ne 'preserved user data') { throw 'Light package replaced dictionary data' }
    if (-not (Test-Path (Join-Path $installer 'app_data/html/webview2/shared/runtime.js'))) { throw 'Light package lost shared contracts' }
    Remove-Item (Join-Path $fixture 'ui-html/webview2/shared') -Recurse -Force
    $rejected = $false
    try { & (Join-Path $installer 'Prepare-PackageFiles.ps1') -RepoRoot $fixture -TsfDirectory windows -ServerDirectory server -UiHtmlDirectory ui-html -NoticesDirectory . } catch { $rejected = $true }
    if (-not $rejected) { throw 'Missing shared contracts were accepted' }
    if ([IO.File]::ReadAllText($database) -ne 'preserved user data') { throw 'Rejected package damaged previous staging' }
    Write-Host 'Full/light package contracts, provenance, exclusions and failure staging passed'
} finally {
    if (Test-Path $fixture) { Remove-Item $fixture -Recurse -Force }
}
