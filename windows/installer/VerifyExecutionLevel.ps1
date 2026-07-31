[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string[]] $Binaries,

    [string] $ManifestTool
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if ([string]::IsNullOrWhiteSpace($ManifestTool)) {
    $kitsRoot = Join-Path (Get-Item Env:'ProgramFiles(x86)').Value 'Windows Kits\10\bin'
    $manifestToolFile = Get-ChildItem -LiteralPath $kitsRoot -Filter mt.exe -File -Recurse |
        Where-Object { $_.FullName -match '\\x64\\mt\.exe$' } |
        Sort-Object FullName -Descending |
        Select-Object -First 1
    if (-not $manifestToolFile) {
        throw "Could not locate the x64 Windows SDK manifest tool under $kitsRoot"
    }
    $ManifestTool = $manifestToolFile.FullName
}

$ManifestTool = (Resolve-Path -LiteralPath $ManifestTool).Path
$temporaryRoot = Join-Path ([IO.Path]::GetTempPath()) ("RemoteCam-manifests-{0}" -f [guid]::NewGuid())
New-Item -ItemType Directory -Path $temporaryRoot | Out-Null

try {
    foreach ($binary in $Binaries) {
        $binaryPath = (Resolve-Path -LiteralPath $binary).Path
        $manifestPath = Join-Path $temporaryRoot (([IO.Path]::GetFileName($binaryPath)) + '.manifest.xml')

        & $ManifestTool "-inputresource:$binaryPath;#1" "-out:$manifestPath"
        if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $manifestPath)) {
            throw "Failed to extract RT_MANIFEST resource #1 from $binaryPath"
        }

        [xml] $manifest = Get-Content -LiteralPath $manifestPath -Raw
        $manager = [Xml.XmlNamespaceManager]::new($manifest.NameTable)
        $manager.AddNamespace('asmv1', 'urn:schemas-microsoft-com:asm.v1')
        $manager.AddNamespace('asmv3', 'urn:schemas-microsoft-com:asm.v3')
        $executionLevel = $manifest.SelectSingleNode(
            '/asmv1:assembly/asmv3:trustInfo/asmv3:security/asmv3:requestedPrivileges/asmv3:requestedExecutionLevel',
            $manager)
        if (-not $executionLevel) {
            throw "The embedded manifest in $binaryPath has no requestedExecutionLevel"
        }
        if ($executionLevel.level -ne 'requireAdministrator') {
            throw "The embedded manifest in $binaryPath requests '$($executionLevel.level)', not 'requireAdministrator'"
        }

        Write-Host "Verified requireAdministrator in embedded manifest: $binaryPath"
    }
} finally {
    if (Test-Path -LiteralPath $temporaryRoot) {
        Remove-Item -LiteralPath $temporaryRoot -Recurse -Force
    }
}
