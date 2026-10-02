# Move the files SignPath signs out of the installer tree and back.
#
# SignPath Foundation signs only a project's own binaries, so the payload submitted to it is not server_exe/ and tsf_dll/ as they stand: those also carry the third-party DLLs vcpkg builds for the Server, which stay as their upstream shipped them. Stage copies the project's binaries, keeping their paths under installer/, into a directory that is uploaded as the signing request's artifact. Restore copies the signed files from SignPath's output back over the originals and refuses any file that does not come back with a valid signature.
#
# The artifact configurations in installer/signpath/ list the same paths. Keep the two in step: SignPath fails a request whose configuration names a file the artifact lacks, and a file staged here but missing from the configuration does not come back signed, which Restore rejects.
#
# Run from installer/.
param(
    [Parameter(Mandatory)][ValidateSet('Stage', 'Restore')][string]$Mode,
    [Parameter(Mandatory)][ValidateSet('Payload', 'Installer')][string]$Set,
    [Parameter(Mandatory)][string]$Directory,
    [string]$Version
)

$ErrorActionPreference = 'Stop'

$files = if ($Set -eq 'Payload') {
    @(
        'server_exe/MetasequoiaImeServer.exe',
        'server_exe/MetasequoiaImeWatchdog.exe',
        'server_exe/MetasequoiaImeDictionaryReplay.exe',
        'server_exe/MetasequoiaImeEmojiPanel.exe',
        'server_exe/MetasequoiaImeKeyboardPanel.exe',
        'server_exe/MetasequoiaImeHandwritingPanel.exe',
        'server_exe/MetasequoiaImeSettings.exe',
        'tsf_dll/32/MetasequoiaImeTsf.dll',
        'tsf_dll/64/MetasequoiaImeTsf.dll'
    )
}
else {
    if (-not $Version) { throw 'The installer set needs -Version.' }
    @("Output/MetasequoiaIME_Setup_v$Version.exe")
}

if ($Mode -eq 'Stage') {
    if ($Set -eq 'Payload') {
        # A new executable of ours that is not in the list above would ship unsigned. Every binary of this project carries the MetasequoiaIme prefix, so that is what the check keys on.
        $listed = $files | ForEach-Object { (Resolve-Path $_).Path }
        $pe = @(Get-ChildItem -Recurse -File -Include '*.exe', '*.dll' -Path 'server_exe', 'tsf_dll')
        $unlisted = @($pe | Where-Object { $_.Name -like 'MetasequoiaIme*' -and $_.FullName -notin $listed })
        if ($unlisted) {
            throw "Project binaries missing from the SignPath list: $($unlisted.FullName -join ', ')"
        }
        Write-Host 'Left as shipped upstream, not submitted to SignPath:'
        $pe | Where-Object { $_.FullName -notin $listed } | ForEach-Object { Write-Host " - $($_.FullName)" }
    }

    New-Item -ItemType Directory -Path $Directory -Force | Out-Null
    foreach ($file in $files) {
        if (-not (Test-Path -LiteralPath $file)) { throw "Nothing to stage at $file" }
        $target = Join-Path $Directory $file
        New-Item -ItemType Directory -Path (Split-Path $target) -Force | Out-Null
        Copy-Item -LiteralPath $file -Destination $target
    }
    Write-Host "Staged $($files.Count) file(s) in $Directory."
}
else {
    foreach ($file in $files) {
        $signed = Join-Path $Directory $file
        if (-not (Test-Path -LiteralPath $signed)) { throw "SignPath returned no $file" }
        $signature = Get-AuthenticodeSignature -LiteralPath $signed
        if ($signature.Status -ne 'Valid') {
            throw "SignPath returned $file with signature status $($signature.Status)"
        }
        Copy-Item -LiteralPath $signed -Destination $file -Force
        Write-Host "$file signed by $($signature.SignerCertificate.Subject)"
    }
}
