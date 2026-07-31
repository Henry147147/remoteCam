[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $VersionFile,

    [Parameter(Mandatory = $true)]
    [string[]] $Binaries,

    [switch] $RequireSignature
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $VersionFile -PathType Leaf)) {
    throw "Version file does not exist: $VersionFile"
}
$expectedVersion = (Get-Content -LiteralPath $VersionFile -Raw).Trim()
if ($expectedVersion -notmatch '^\d+\.\d+\.\d+$') {
    throw "Expected a semantic project version, got '$expectedVersion'"
}

foreach ($binary in $Binaries) {
    $resolved = Resolve-Path -LiteralPath $binary -ErrorAction Stop
    $info = [Diagnostics.FileVersionInfo]::GetVersionInfo($resolved.Path)
    if ($info.FileVersion -ne "$expectedVersion.0") {
        throw "$binary has FileVersion '$($info.FileVersion)', expected '$expectedVersion.0'"
    }
    if ($info.ProductVersion -ne $expectedVersion) {
        throw "$binary has ProductVersion '$($info.ProductVersion)', expected '$expectedVersion'"
    }
    if ([string]::IsNullOrWhiteSpace($info.FileDescription) -or
        [string]::IsNullOrWhiteSpace($info.ProductName)) {
        throw "$binary is missing Windows version-description metadata"
    }

    if ($RequireSignature) {
        $signature = Get-AuthenticodeSignature -LiteralPath $resolved.Path
        if ($signature.Status -ne [Management.Automation.SignatureStatus]::Valid) {
            throw "$binary does not have a valid Authenticode signature: $($signature.Status)"
        }
    }

    Write-Host "Verified $binary ($expectedVersion)"
}
