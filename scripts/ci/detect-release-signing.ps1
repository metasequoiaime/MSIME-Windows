# Decide whether this release can be signed, and say so in the log either way.
#
# Signing is optional so the pipeline stays runnable without a certificate, but an unsigned build is degraded in a way users will notice: per AGENTS.md, uiAccess="true" only takes effect for a correctly signed binary installed somewhere Windows trusts, so the candidate window cannot float over elevated hosts. Unsigned artifacts therefore carry a suffix and the release notes say so.
#
# SignPath is opt-in through the WINDOWS_SIGNING_PROVIDER repository variable. Unset, the certificate path below is the one that runs. Set to signpath, the SignPath settings must all be present: a half-configured SignPath fails the release instead of quietly falling back to the certificate or to an unsigned build.
#
# Writes signing_enabled, signing_provider (signpath, certificate or none) and asset_suffix to GITHUB_OUTPUT.
$ErrorActionPreference = 'Stop'

if ($env:SIGNING_PROVIDER -eq 'signpath') {
    $missing = @(
        'SIGNPATH_API_TOKEN', 'SIGNPATH_ORGANIZATION_ID', 'SIGNPATH_PROJECT_SLUG', 'SIGNPATH_SIGNING_POLICY_SLUG'
    ) | Where-Object { -not [Environment]::GetEnvironmentVariable($_) }
    if ($missing) {
        throw "WINDOWS_SIGNING_PROVIDER is signpath, but these are not configured: $($missing -join ', ')"
    }
    "signing_enabled=true" >> $env:GITHUB_OUTPUT
    "signing_provider=signpath" >> $env:GITHUB_OUTPUT
    "asset_suffix=" >> $env:GITHUB_OUTPUT
    Write-Host 'Signing through SignPath: the payload binaries and the installer will be submitted as signing requests.'
    return
}
if ($env:SIGNING_PROVIDER) {
    throw "Unknown WINDOWS_SIGNING_PROVIDER '$($env:SIGNING_PROVIDER)'. Leave it unset for the certificate, or set it to signpath."
}

$storeCert = $null
if ($env:CERTIFICATE_THUMBPRINT) {
    $normalized = $env:CERTIFICATE_THUMBPRINT -replace '\s', ''
    $storeCert = Get-ChildItem Cert:\CurrentUser\My\$normalized -ErrorAction SilentlyContinue
    if (-not $storeCert -or -not $storeCert.HasPrivateKey) { $storeCert = $null }
    if (-not $storeCert -and -not ($env:CERTIFICATE_BASE64 -and $env:CERTIFICATE_PASSWORD)) {
        throw "A signing certificate thumbprint is configured, but the certificate or private key is unavailable in the runner user store."
    }
}

if ($storeCert -or ($env:CERTIFICATE_BASE64 -and $env:CERTIFICATE_PASSWORD)) {
    "signing_enabled=true" >> $env:GITHUB_OUTPUT
    "signing_provider=certificate" >> $env:GITHUB_OUTPUT
    "asset_suffix=" >> $env:GITHUB_OUTPUT
    if ($storeCert) { Write-Host 'Signing certificate present in the runner user certificate store.' }
    else { Write-Host 'Signing certificate secret present: the installer and its binaries will be signed.' }
}
else {
    "signing_enabled=false" >> $env:GITHUB_OUTPUT
    "signing_provider=none" >> $env:GITHUB_OUTPUT
    "asset_suffix=-unsigned" >> $env:GITHUB_OUTPUT
    Write-Host '::warning::No signing certificate configured. Publishing an unsigned installer; uiAccess will not take effect.'
}
