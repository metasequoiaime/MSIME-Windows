# Configure and build the Server; do not reuse old binaries after a failed build.
[CmdletBinding()]
param(
    [string]$ManifestTool = 'C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\mt.exe',
    # ALL_BUILD also builds MetasequoiaImeServerTests and test_webview_contract, which the packaging
    # step deletes again. The server target already depends on the watchdog, the three panels,
    # Settings, DictionaryReplay and the OpenCC dictionaries, so it produces exactly the packaged
    # set. The test jobs still build everything by passing -Target ALL_BUILD.
    [string]$Target = 'MetasequoiaImeServer',
    # The Visual Studio generator wires ZERO_CHECK into every target, so a build regenerates itself
    # whenever CMakeLists.txt changes. Re-running configure up front only costs ~10s without
    # changing the result, so do it once and let ZERO_CHECK handle the rest.
    [switch]$Reconfigure
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$buildDirectory = Join-Path $projectRoot 'build-release'
Push-Location $projectRoot
try {
    if ($Reconfigure -or -not (Test-Path -LiteralPath (Join-Path $buildDirectory 'CMakeCache.txt'))) {
        cmake --preset=vcpkg-release
        if ($LASTEXITCODE -ne 0) { throw "Server configure failed ($LASTEXITCODE)" }
    }
    cmake --build $buildDirectory --config Release --target $Target --parallel
    if ($LASTEXITCODE -ne 0) { throw "Server build failed ($LASTEXITCODE)" }
    # Keep the local SDK selection; the complete resource argument must reach mt.exe as one value.
    $binary = Join-Path $buildDirectory 'bin/Release/MetasequoiaImeServer.exe'
    & $ManifestTool `
        -manifest (Join-Path $projectRoot 'MetasequoiaImeServer.manifest') "-outputresource:$binary;1"
    if ($LASTEXITCODE -ne 0) { throw "Server manifest embedding failed ($LASTEXITCODE)" }
} finally { Pop-Location }
