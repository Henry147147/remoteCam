param(
    [Parameter(Mandatory = $false)]
    [string]$OutputDirectory = "test-artifacts/video-fixtures",
    [Parameter(Mandatory = $false)]
    [string]$Ffmpeg = "ffmpeg",
    [Parameter(Mandatory = $false)]
    [string]$Ffprobe = "ffprobe",
    [Parameter(Mandatory = $false)]
    [int]$DurationSeconds = 4,
    [switch]$RequireAllCodecs
)

$ErrorActionPreference = "Stop"
if ($DurationSeconds -lt 2 -or $DurationSeconds -gt 60) {
    throw "DurationSeconds must be between 2 and 60."
}

$resolvedOutput = [System.IO.Path]::GetFullPath((Join-Path $PWD $OutputDirectory))
New-Item -ItemType Directory -Force -Path $resolvedOutput | Out-Null

& $Ffmpeg -hide_banner -version | Out-Null
if ($LASTEXITCODE -ne 0) { throw "ffmpeg was not found or could not start." }
& $Ffprobe -hide_banner -version | Out-Null
if ($LASTEXITCODE -ne 0) { throw "ffprobe was not found or could not start." }

function New-RemoteCamFixture {
    param(
        [int]$Width,
        [int]$Height,
        [int]$Fps,
        [ValidateSet("h264", "hevc")]
        [string]$Codec
    )

    $encoder = if ($Codec -eq "h264") { "h264_mf" } else { "hevc_mf" }
    $metadataFilter = if ($Codec -eq "h264") { "h264_metadata=aud=insert" } else { "hevc_metadata=aud=insert" }
    $extension = if ($Codec -eq "h264") { "h264" } else { "hevc" }
    $bitrate = if ($Width -ge 1920) { "12M" } else { "6M" }
    $gop = $Fps * 2
    $name = "remotecam-${Width}x${Height}-${Fps}fps-${Codec}.${extension}"
    $path = Join-Path $resolvedOutput $name

    # Top half: fixed, high-contrast color/geometry reference. Bottom half: moving
    # marker plus an exact frame number. Orientation, crop, dropped/duplicated frames,
    # color-channel swaps and stale video are therefore visible in one short clip.
    $filter = @(
        "color=c=0x101827:s=${Width}x${Height}:r=${Fps}:d=${DurationSeconds}[base];",
        "color=c=0xff3158:s=160x84:r=${Fps}:d=${DurationSeconds}[moving];",
        "[base]drawgrid=w=iw/8:h=ih/8:t=2:c=white@0.20,",
        "drawbox=x=0:y=0:w=iw/3:h=ih/2:c=0xe53935:t=fill,",
        "drawbox=x=iw/3:y=0:w=iw/3:h=ih/2:c=0x43a047:t=fill,",
        "drawbox=x=2*iw/3:y=0:w=iw/3:h=ih/2:c=0x1e88e5:t=fill,",
        "drawtext=fontfile='C\:/Windows/Fonts/consola.ttf':text='TOP STATIC  ${Width}x${Height} ${Fps}FPS':x=32:y=32:fontsize=32:fontcolor=white:box=1:boxcolor=black@0.70[marked];",
        "[marked][moving]overlay=x='mod(n*19,W-w)':y='3*H/4-h/2',",
        "drawtext=fontfile='C\:/Windows/Fonts/consola.ttf':text='RC FRAME %{n}':x=32:y=h-72:fontsize=38:fontcolor=white:box=1:boxcolor=black@0.85,format=nv12[v]"
    ) -join ""

    Write-Host "Generating $name with Windows Media Foundation ($encoder)..."
    & $Ffmpeg -hide_banner -loglevel warning -y `
        -filter_complex $filter -map "[v]" -an -t $DurationSeconds `
        -c:v $encoder -b:v $bitrate -g $gop -bf 0 `
        -bsf:v "${metadataFilter},dump_extra=freq=keyframe" -f $Codec $path
    if ($LASTEXITCODE -ne 0) {
        throw "Encoding $name failed. The required Windows Media Foundation $Codec encoder may be unavailable on this test machine. No software/GPL encoder fallback is used."
    }

    $probeJson = & $Ffprobe -v error -select_streams v:0 `
        -show_entries "stream=codec_name,width,height,r_frame_rate" `
        -show_entries "packet=pts_time,flags" -of json $path
    if ($LASTEXITCODE -ne 0) { throw "ffprobe could not inspect $name." }
    $probe = $probeJson | ConvertFrom-Json
    $packets = @($probe.packets)
    if ($packets.Count -lt ($Fps * 2)) { throw "$name contains too few access units." }
    $keyframes = @($packets | Where-Object { $_.flags -like "K*" }).Count
    $requiredKeyframes = 1 + [Math]::Floor(($DurationSeconds - 1) / 2)
    if ($keyframes -lt $requiredKeyframes) { throw "$name does not contain the required two-second keyframe cadence." }

    # Decode the entire fixture. This catches a file that is structurally Annex-B but
    # not visually decodable before it reaches a desktop run.
    & $Ffmpeg -v error -i $path -f null -
    if ($LASTEXITCODE -ne 0) { throw "Full decode verification failed for $name." }

    $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash.ToLowerInvariant()
    [pscustomobject]@{
        file = $name
        codec = $Codec
        width = $Width
        height = $Height
        fps = $Fps
        duration_seconds = $DurationSeconds
        access_units = $packets.Count
        keyframes = $keyframes
        sha256 = $hash
    }
}

$manifest = [System.Collections.Generic.List[object]]::new()
$failures = [System.Collections.Generic.List[string]]::new()
$specifications = @(
    @{ Width = 1280; Height = 720; Fps = 30; Codec = "h264" }
    @{ Width = 1280; Height = 720; Fps = 30; Codec = "hevc" }
    @{ Width = 1920; Height = 1080; Fps = 60; Codec = "h264" }
    @{ Width = 1920; Height = 1080; Fps = 60; Codec = "hevc" }
)
foreach ($specification in $specifications) {
    try {
        $fixture = New-RemoteCamFixture @specification
        $fixture | Add-Member -NotePropertyName status -NotePropertyValue "verified"
        $manifest.Add($fixture)
    } catch {
        $name = "remotecam-$($specification.Width)x$($specification.Height)-$($specification.Fps)fps-$($specification.Codec).$($specification.Codec)"
        $failedPath = Join-Path $resolvedOutput $name
        if (Test-Path -LiteralPath $failedPath) { Remove-Item -LiteralPath $failedPath -Force }
        $reason = $_.Exception.Message
        $failures.Add("${name}: $reason")
        $manifest.Add([pscustomobject]@{
            file = $name
            codec = $specification.Codec
            width = $specification.Width
            height = $specification.Height
            fps = $specification.Fps
            duration_seconds = $DurationSeconds
            status = "unavailable"
            reason = $reason
        })
        Write-Warning $reason
    }
}

$manifestPath = Join-Path $resolvedOutput "manifest.json"
$manifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $manifestPath -Encoding UTF8
Write-Host "Verified fixtures and manifest: $resolvedOutput"
if ($RequireAllCodecs -and $failures.Count -gt 0) {
    throw "One or more required fixtures could not be generated: $($failures -join ' | ')"
}
$global:LASTEXITCODE = 0
