# Fail early when the Server build is missing something the installer collects, rather than
# letting Prepare-PackageFiles.ps1 report it several jobs later.
param([string]$BuildDir = 'build/bin/Release')

$ErrorActionPreference = 'Stop'

$required = @(
    'MetasequoiaImeServer.exe',
    'MetasequoiaImeSettings.exe',
    'MetasequoiaImeWatchdog.exe',
    'MetasequoiaImeDictionaryReplay.exe'
)

$missing = $required | Where-Object { -not (Test-Path (Join-Path $BuildDir $_)) }
if ($missing) {
    throw "Missing from the Server build: $($missing -join ', ')"
}

$productionExecutables = @(
    Get-ChildItem -LiteralPath $BuildDir -Recurse -File -Filter '*.exe' |
        Where-Object { $_.BaseName -notlike '*Tests' -and $_.BaseName -notlike 'test_*' }
)
$missingPdb = @(
    $productionExecutables |
        Where-Object {
            -not (Test-Path (Join-Path $_.DirectoryName ([IO.Path]::ChangeExtension($_.Name, '.pdb'))))
        } |
        ForEach-Object { Join-Path $_.DirectoryName ([IO.Path]::ChangeExtension($_.Name, '.pdb')) }
)
if ($missingPdb.Count -gt 0) {
    throw "Missing matching PDB files from the Server build: $($missingPdb -join ', ')"
}

Get-ChildItem $BuildDir -File | Select-Object Name, Length | Format-Table
