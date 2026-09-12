# Build the TSF DLL without depending on the caller's working directory.
[CmdletBinding()]
param(
    [ValidateSet('32', '64')][string]$Architecture = '64',
    # ALL_BUILD also builds test_windows_ipc_contract and the tests/ subdirectory, neither of which
    # is packaged. Naming the DLL target keeps a local packaging run from paying for them; the test
    # jobs still build ALL_BUILD by passing -Target ALL_BUILD.
    [string]$Target = 'MetasequoiaImeTsf',
    # The Visual Studio generator wires ZERO_CHECK into every target, so a build regenerates itself
    # whenever CMakeLists.txt changes. Re-running configure up front only costs ~10s per build
    # directory without changing the result, so do it once and let ZERO_CHECK handle the rest.
    [switch]$Reconfigure
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$buildDirectory = Join-Path $projectRoot "build$Architecture-release"
Push-Location $projectRoot
try {
    if ($Reconfigure -or -not (Test-Path -LiteralPath (Join-Path $buildDirectory 'CMakeCache.txt'))) {
        cmake "--preset=for$Architecture-release"
        if ($LASTEXITCODE -ne 0) { throw "TSF $Architecture configure failed ($LASTEXITCODE)" }
    }
    cmake --build $buildDirectory --config Release --target $Target --parallel
    if ($LASTEXITCODE -ne 0) { throw "TSF $Architecture build failed ($LASTEXITCODE)" }
} finally { Pop-Location }
