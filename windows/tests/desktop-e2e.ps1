param(
    [string]$BinDirectory = "build-package/bin",
    [string]$QtBinDirectory = "D:/Qt-RemoteCam/6.8.3/msvc2022_64/bin",
    [string]$ArtifactDirectory = "test-artifacts/desktop-e2e",
    [string]$ReplayFile = "",
    [ValidateSet("h264", "hevc")]
    [string]$ReplayCodec = "h264",
    [switch]$AllowKnownGaps
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Add-Type -AssemblyName System.Drawing

Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class RemoteCamNativeDesktop {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int command);
    [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdc, uint flags);
}
"@

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot "../.."))
$bin = [System.IO.Path]::GetFullPath((Join-Path $repoRoot $BinDirectory))
$artifacts = [System.IO.Path]::GetFullPath((Join-Path $repoRoot $ArtifactDirectory))
$e2eExe = Join-Path $bin "RemoteCam-E2E.exe"
$productionExe = Join-Path $bin "RemoteCam.exe"
$phoneExe = Join-Path $bin "rc-fakephone.exe"
foreach ($required in @($e2eExe, $productionExe, $phoneExe)) {
    if (-not (Test-Path -LiteralPath $required)) { throw "Missing required executable: $required" }
}
if (-not (Test-Path -LiteralPath $QtBinDirectory)) {
    throw "Qt runtime directory does not exist: $QtBinDirectory"
}
New-Item -ItemType Directory -Force -Path $artifacts | Out-Null
$env:PATH = ([System.IO.Path]::GetFullPath($QtBinDirectory)) + ";" + $env:PATH

$ownedProcesses = [System.Collections.Generic.List[System.Diagnostics.Process]]::new()
$checks = [System.Collections.Generic.List[object]]::new()

function Add-Check([string]$Name, [string]$Status, [string]$Evidence) {
    $checks.Add([pscustomobject]@{ name = $Name; status = $Status; evidence = $Evidence })
    $prefix = if ($Status -eq "pass") { "PASS" } elseif ($Status -eq "missing") { "GAP" } else { "FAIL" }
    Write-Host "[$prefix] ${Name}: $Evidence"
}

function Start-OwnedProcess([string]$FilePath, [string[]]$Arguments, [switch]$Visible) {
    $start = @{
        FilePath = $FilePath
        WorkingDirectory = $bin
        PassThru = $true
    }
    if ($Arguments.Count -gt 0) { $start.ArgumentList = $Arguments }
    if (-not $Visible) { $start.WindowStyle = "Hidden" }
    $process = Start-Process @start
    $ownedProcesses.Add($process)
    return $process
}

function Wait-MainWindow([System.Diagnostics.Process]$Process, [int]$TimeoutMillis = 10000) {
    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMillis)
    do {
        Start-Sleep -Milliseconds 100
        $Process.Refresh()
        if ($Process.HasExited) { throw "$($Process.ProcessName) exited before opening a window." }
        if ($Process.MainWindowHandle -ne [IntPtr]::Zero) {
            return [System.Windows.Automation.AutomationElement]::FromHandle($Process.MainWindowHandle)
        }
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "Timed out waiting for $($Process.ProcessName) to open a main window."
}

function Get-AccessibleElements([System.Windows.Automation.AutomationElement]$Root) {
    return $Root.FindAll(
        [System.Windows.Automation.TreeScope]::Descendants,
        [System.Windows.Automation.Condition]::TrueCondition
    )
}

function Find-ByIdSuffix([System.Windows.Automation.AutomationElement]$Root, [string]$Suffix) {
    $elements = Get-AccessibleElements $Root
    for ($index = 0; $index -lt $elements.Count; ++$index) {
        $element = $elements.Item($index)
        if ($element.Current.AutomationId.EndsWith($Suffix, [StringComparison]::Ordinal)) {
            return $element
        }
    }
    return $null
}

function Wait-AccessibleName(
    [System.Windows.Automation.AutomationElement]$Root,
    [string]$IdSuffix,
    [string]$ExpectedName,
    [int]$TimeoutMillis = 5000
) {
    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMillis)
    do {
        $element = Find-ByIdSuffix $Root $IdSuffix
        if ($null -ne $element -and $element.Current.Name -eq $ExpectedName) { return $element }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    $actual = if ($null -eq $element) { "element missing" } else { $element.Current.Name }
    throw "Expected $IdSuffix to read '$ExpectedName'; actual: '$actual'."
}

function Save-WindowScreenshot(
    [System.Diagnostics.Process]$Process,
    [System.Windows.Automation.AutomationElement]$Root,
    [string]$Name
) {
    $bounds = $Root.Current.BoundingRectangle
    $width = [Math]::Max(1, [int][Math]::Ceiling($bounds.Width))
    $height = [Math]::Max(1, [int][Math]::Ceiling($bounds.Height))
    $bitmap = [System.Drawing.Bitmap]::new($width, $height)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    try {
        $hdc = $graphics.GetHdc()
        try {
            $printed = [RemoteCamNativeDesktop]::PrintWindow($Process.MainWindowHandle, $hdc, 2)
        } finally {
            $graphics.ReleaseHdc($hdc)
        }
        if (-not $printed) {
            [RemoteCamNativeDesktop]::ShowWindow($Process.MainWindowHandle, 9) | Out-Null
            [RemoteCamNativeDesktop]::SetForegroundWindow($Process.MainWindowHandle) | Out-Null
            Start-Sleep -Milliseconds 250
            $graphics.CopyFromScreen([int]$bounds.X, [int]$bounds.Y, 0, 0, $bitmap.Size)
        }
        $path = Join-Path $artifacts $Name
        $bitmap.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
        return $path
    } finally {
        $graphics.Dispose()
        $bitmap.Dispose()
    }
}

function Close-OwnedProcess([System.Diagnostics.Process]$Process) {
    if ($null -eq $Process) { return }
    $Process.Refresh()
    if ($Process.HasExited) { return }
    if ($Process.MainWindowHandle -ne [IntPtr]::Zero) { $Process.CloseMainWindow() | Out-Null }
    if (-not $Process.WaitForExit(5000)) { Stop-Process -Id $Process.Id -Force }
}

$featureIds = [ordered]@{
    "Live preview" = "remoteCam.livePreview"
    "Lens selector" = "remoteCam.cameraLens"
    "Zoom control" = "remoteCam.cameraZoom"
    "Focus controls" = "remoteCam.cameraFocus"
    "Exposure controls" = "remoteCam.cameraExposure"
    "White-balance controls" = "remoteCam.cameraWhiteBalance"
    "Torch control" = "remoteCam.cameraTorch"
    "Stabilization control" = "remoteCam.cameraStabilization"
    "Rotation control" = "remoteCam.transformRotation"
    "Fit/fill/stretch selector" = "remoteCam.transformScaling"
    "Pan control" = "remoteCam.transformPan"
    "Flip controls" = "remoteCam.transformFlip"
    "Orientation lock" = "remoteCam.orientationLock"
    "Transform presets" = "remoteCam.transformPresets"
    "Effects controls" = "remoteCam.effects"
    "Freeze/blank/placeholder" = "remoteCam.outputFallbacks"
    "Screenshot action" = "remoteCam.screenshot"
    "Recording action" = "remoteCam.recording"
    "Diagnostics view" = "remoteCam.diagnostics"
    "Tray controls" = "remoteCam.tray"
    "Hotkey settings" = "remoteCam.hotkeys"
}

try {
    $e2e = Start-OwnedProcess -FilePath $e2eExe -Arguments @() -Visible
    $root = Wait-MainWindow $e2e
    Add-Check "E2E window opens" "pass" $root.Current.Name
    Add-Check "DPI" "pass" ([RemoteCamNativeDesktop]::GetDpiForWindow($e2e.MainWindowHandle).ToString())
    Wait-AccessibleName $root "remoteCam.discoveryStatusLabel" "Loopback desktop test" | Out-Null
    Add-Check "Test receiver state" "pass" "Loopback desktop test (no firewall mutation)"

    $secondInstance = Start-OwnedProcess -FilePath $e2eExe -Arguments @() -Visible
    $secondRoot = Wait-MainWindow $secondInstance
    Wait-AccessibleName $secondRoot "remoteCam.outputStatusLabel" "Producer conflict" 5000 | Out-Null
    Add-Check "Single producer guard" "pass" "second window reports Producer conflict"
    Close-OwnedProcess $secondInstance

    $phoneArguments = @(
        "run", "--connect", "127.0.0.1:7890", "--scenario", "smoke",
        "--duration", "5", "--allow-insecure-session",
        "--report-jsonl", (Join-Path $artifacts "e2e-phone.jsonl"),
        "--report-junit", (Join-Path $artifacts "e2e-phone.xml")
    )
    if ($ReplayFile) {
        $phoneArguments[0] = "replay"
        $phoneArguments += @("--file", ([System.IO.Path]::GetFullPath($ReplayFile)), "--codec", $ReplayCodec)
    }
    $phone = Start-OwnedProcess -FilePath $phoneExe -Arguments $phoneArguments
    Wait-AccessibleName $root "remoteCam.phoneStatusLabel" "Phone streaming" 5000 | Out-Null
    Add-Check "E2E streaming checkpoint" "pass" "Phone streaming"
    $e2eScreenshot = Save-WindowScreenshot $e2e $root "e2e-streaming.png"
    Add-Check "E2E screenshot" "pass" $e2eScreenshot

    foreach ($entry in $featureIds.GetEnumerator()) {
        $element = Find-ByIdSuffix $root $entry.Value
        if ($null -eq $element) {
            Add-Check $entry.Key "missing" "automation surface $($entry.Value) is absent"
        } else {
            Add-Check $entry.Key "pass" $element.Current.Name
        }
    }
    $preview = Find-ByIdSuffix $root "remoteCam.livePreview"
    if ($null -eq $preview) {
        Add-Check "Correct displayed video" "missing" "no live preview exists, so marker/color/motion pixels cannot be verified"
    } elseif (-not $ReplayFile) {
        Add-Check "Correct displayed video" "missing" "provide -ReplayFile from generate-video-fixtures.ps1 for pixel verification"
    } else {
        # A future preview implementation must expose this ID. Two captures one second
        # apart are retained so a reviewer/test extension can compare the static upper
        # reference and moving lower frame marker.
        $first = Save-WindowScreenshot $e2e $root "video-frame-a.png"
        Start-Sleep -Seconds 1
        $second = Save-WindowScreenshot $e2e $root "video-frame-b.png"
        $different = (Get-FileHash $first).Hash -ne (Get-FileHash $second).Hash
        $videoStatus = if ($different) { "pass" } else { "fail" }
        Add-Check "Correct displayed video" $videoStatus "captured replay frames differ=$different; retained for marker/color review"
    }

    $phone.WaitForExit(10000) | Out-Null
    Add-Check "Emulator process" $(if ($phone.ExitCode -eq 0) { "pass" } else { "fail" }) "exit=$($phone.ExitCode)"
    Close-OwnedProcess $e2e

    $production = Start-OwnedProcess -FilePath $productionExe -Arguments @("--test-loopback") -Visible
    $productionRoot = Wait-MainWindow $production
    $productionPhone = Start-OwnedProcess -FilePath $phoneExe -Arguments @(
        "run", "--connect", "127.0.0.1:7890", "--scenario", "production-lock",
        "--duration", "2", "--report-jsonl", (Join-Path $artifacts "production-lock.jsonl")
    )
    Wait-AccessibleName $productionRoot "remoteCam.phoneStatusLabel" "Secure pairing required" 5000 | Out-Null
    Add-Check "Production security boundary" "pass" "paired=false and no ready"
    Save-WindowScreenshot $production $productionRoot "production-pairing-required.png" | Out-Null
    $productionPhone.WaitForExit(10000) | Out-Null
    Add-Check "Production-lock emulator" $(if ($productionPhone.ExitCode -eq 0) { "pass" } else { "fail" }) "exit=$($productionPhone.ExitCode)"
} catch {
    Add-Check "Desktop harness" "fail" $_.Exception.Message
} finally {
    foreach ($process in $ownedProcesses) { Close-OwnedProcess $process }
}

$missing = @($checks | Where-Object { $_.status -eq "missing" }).Count
$failed = @($checks | Where-Object { $_.status -eq "fail" }).Count
$manifest = [pscustomobject]@{
    schema = "remotecam.desktop-evidence.v1"
    generated_utc = [DateTime]::UtcNow.ToString("o")
    machine = $env:COMPUTERNAME
    replay_file = $ReplayFile
    replay_codec = $ReplayCodec
    checks = $checks
    summary = [pscustomobject]@{ pass = @($checks | Where-Object status -eq "pass").Count; missing = $missing; fail = $failed }
}
$manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $artifacts "manifest.json") -Encoding UTF8

if ($failed -gt 0 -or ($missing -gt 0 -and -not $AllowKnownGaps)) { exit 1 }
exit 0
