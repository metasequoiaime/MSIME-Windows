# Exercise the real orchestration with synthetic command failures; never sign or install.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$source = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$fixture = Join-Path ([IO.Path]::GetTempPath()) ('msime build failures ' + [Guid]::NewGuid())
$originalLocation = Get-Location
try {
    foreach ($component in @('windows', 'server')) {
        $scripts = Join-Path $fixture "$component/scripts"
        New-Item -ItemType Directory -Force $scripts | Out-Null
        Copy-Item (Join-Path $source "$component/scripts/lcompile-release.ps1") $scripts
    }
    $ciScripts = Join-Path $fixture 'scripts/ci'
    New-Item -ItemType Directory -Force $ciScripts | Out-Null
    Copy-Item (Join-Path $source 'scripts/ci/embed-server-manifest.ps1') $ciScripts
    function global:cmake {
        $global:MsimeBuildCalls += 1
        $global:LASTEXITCODE = if ($global:MsimeBuildCalls -eq $global:MsimeFailAt) { 17 } else { 0 }
    }
    foreach ($component in @('windows', 'server')) {
        foreach ($failure in @(1, 2)) {
            $global:MsimeBuildCalls = 0
            $global:MsimeFailAt = $failure
            $rejected = $false
            try { & (Join-Path $fixture "$component/scripts/lcompile-release.ps1") }
            catch { $rejected = $_.Exception.Message -match '(configure|build) failed \(17\)' }
            if (-not $rejected) { throw "$component did not reject failed build stage $failure" }
            if ($global:MsimeBuildCalls -ne $failure) { throw 'Continued after a failed native command' }
        }
    }
    function global:Invoke-MsimeManifestProbe {
        $global:MsimeManifestArguments = @($args)
        $global:LASTEXITCODE = $global:MsimeManifestExit
    }
    $global:MsimeFailAt = 0
    $global:MsimeManifestExit = 0
    & (Join-Path $fixture 'server/scripts/lcompile-release.ps1') -ManifestTool Invoke-MsimeManifestProbe
    $binary = Join-Path $fixture 'server/build-release/bin/Release/MetasequoiaImeServer.exe'
    if ($global:MsimeManifestArguments.Count -ne 3 -or $global:MsimeManifestArguments[2] -ne "-outputresource:$binary;1") {
        throw 'Manifest resource id or path with spaces was split into separate commands'
    }
    $global:MsimeManifestExit = 31
    $rejected = $false
    try { & (Join-Path $fixture 'server/scripts/lcompile-release.ps1') -ManifestTool Invoke-MsimeManifestProbe }
    catch { $rejected = $_.Exception.Message -eq 'Server manifest embedding failed (31)' }
    if (-not $rejected) { throw 'Failed manifest embedding was accepted' }

    # The release workflow builds Server directly instead of calling lcompile-release.ps1, so
    # exercise the standalone CI embedding step too. This keeps the manifest-before-signing
    # contract covered without requiring a Windows SDK in this orchestration test.
    $releaseBinary = Join-Path $fixture 'server/build-release/bin/Release/MetasequoiaImeServer.exe'
    $releaseManifest = Join-Path $fixture 'server/MetasequoiaImeServer.manifest'
    New-Item -ItemType Directory -Force (Split-Path $releaseBinary) | Out-Null
    New-Item -ItemType Directory -Force (Split-Path $releaseManifest) | Out-Null
    [IO.File]::WriteAllText($releaseBinary, 'binary')
    [IO.File]::WriteAllText($releaseManifest, '<assembly><trustInfo><requestedPrivileges><requestedExecutionLevel uiAccess="true" /></requestedPrivileges></trustInfo></assembly>')
    $global:MsimeManifestArguments = @()
    $global:MsimeManifestExit = 0
    $expectedInputResource = '-inputresource:' + $releaseBinary + ';#1'
    function global:Invoke-MsimeManifestProbe {
        $global:MsimeManifestArguments = @($args)
        $global:LASTEXITCODE = 0
        if ($args.Count -eq 2 -and $args[0] -like '-inputresource:*;#1') {
            $outPath = ($args[1] -replace '^-out:', '')
            [IO.File]::WriteAllText($outPath, '<assembly><requestedExecutionLevel uiAccess="true" /></assembly>')
        }
    }
    & (Join-Path $ciScripts 'embed-server-manifest.ps1') -BuildDir 'server/build-release/bin/Release' -ManifestTool Invoke-MsimeManifestProbe
    if ($global:MsimeManifestArguments.Count -ne 2 -or $global:MsimeManifestArguments[0] -ne $expectedInputResource) {
        throw 'CI manifest embedding did not verify resource 1'
    }
    Remove-Item Function:/cmake
    # Each installer entry must stop before staging/signing when the settings build fails.
    foreach ($relative in @('windows/scripts/lcompile-release-both.ps1', 'server/scripts/lcompile-release.ps1')) {
        [IO.File]::WriteAllText((Join-Path $fixture $relative), '# successful synthetic component build')
    }
    $settings = Join-Path $fixture 'ui-html/webview2/settings/ime-settings'
    New-Item -ItemType Directory -Force $settings | Out-Null
    [IO.File]::WriteAllText((Join-Path $settings 'package.json'), '{}')
    $installer = Join-Path $fixture 'installer'
    New-Item -ItemType Directory -Force $installer | Out-Null
    [IO.File]::WriteAllText((Join-Path $installer 'Prepare-PackageFiles.ps1'), "throw 'Reached package staging after failure'")
    function global:pnpm { $global:LASTEXITCODE = 23 }
    Copy-Item (Join-Path $source 'installer/Invoke-LocalTest.ps1') $installer
    foreach ($entry in @('test.ps1', 'test-light.ps1', 'test-symbols.ps1')) {
        Copy-Item (Join-Path $source "installer/$entry") $installer
        $rejected = $false
        try { & (Join-Path $installer $entry) }
        catch { $rejected = $_.Exception.Message -eq 'Settings page build failed (23)' }
        if (-not $rejected) { throw "$entry continued after the settings build failed" }
    }
    $global:LASTEXITCODE = 0
    Write-Host 'Configure/build failures stop local packaging before staging, signing or installation'
} finally {
    Set-Location $originalLocation
    Remove-Item Function:/cmake, Function:/pnpm, Function:/Invoke-MsimeManifestProbe -ErrorAction SilentlyContinue
    Remove-Variable MsimeBuildCalls, MsimeFailAt, MsimeManifestExit, MsimeManifestArguments -Scope Global -ErrorAction SilentlyContinue
    if (Test-Path $fixture) { Remove-Item $fixture -Recurse -Force }
}
