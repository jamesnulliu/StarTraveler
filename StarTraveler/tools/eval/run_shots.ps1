param(
    [Parameter(Mandatory=$true)][string]$Exe,
    [Parameter(Mandatory=$true)][string]$PrepareExe,
    [Parameter(Mandatory=$true)][string]$Assets,
    [Parameter(Mandatory=$true)][string]$Output,
    [string]$Config = (Join-Path $PSScriptRoot 'shots.json'),
    [string[]]$Configurations = @('raw-path','asvgf-full','rr-quality'),
    [string[]]$OnlyShot = @(),
    [int[]]$Seeds = @(17),
    [ValidateRange(192,16384)][int]$Width = 384,
    [ValidateRange(108,16384)][int]$Height = 216,
    [switch]$PlanOnly
)
$ErrorActionPreference = 'Stop'
$invariant = [Globalization.CultureInfo]::InvariantCulture
function Number($value) { ([double]$value).ToString('R', $invariant) }
$Exe = (Resolve-Path -LiteralPath $Exe).Path
$PrepareExe = (Resolve-Path -LiteralPath $PrepareExe).Path
$Assets = (Resolve-Path -LiteralPath $Assets).Path
$Config = (Resolve-Path -LiteralPath $Config).Path
$spec = Get-Content -LiteralPath $Config -Raw | ConvertFrom-Json
$protocolPath = Join-Path (Split-Path $Config) $spec.protocol
$protocol = Get-Content -LiteralPath $protocolPath -Raw | ConvertFrom-Json
$selected = @($Configurations | ForEach-Object {
    $requested = $_
    $matches = @($protocol.configurations | Where-Object name -eq $requested)
    if ($matches.Count -ne 1 -or $matches[0].group -eq 'diagnostic-only') {
        throw "Unknown or diagnostic-only configuration: $requested"
    }
    $matches[0]
})
$shots = @($spec.shots | Where-Object { !$OnlyShot.Count -or $_.name -in $OnlyShot })
if (!$shots.Count -or !$selected.Count -or !$Seeds.Count) { throw 'Empty selection' }
if ($OnlyShot.Count -and @($OnlyShot | Where-Object { $_ -notin $shots.name }).Count) {
    throw 'Unknown shot selection'
}
New-Item -ItemType Directory -Force -Path $Output | Out-Null
$Output = (Resolve-Path -LiteralPath $Output).Path
Add-Type -AssemblyName System.Drawing
$rows = New-Object 'System.Collections.Generic.List[object]'
$jobs = New-Object 'System.Collections.Generic.List[object]'
$manifest = [ordered]@{
    version=1; purpose='shot-smoke'; complete=$false
    upstream_checked=$spec.upstream_checked
    renderer=$Exe; renderer_sha256=(Get-FileHash $Exe -Algorithm SHA256).Hash
    preparer=$PrepareExe; preparer_sha256=(Get-FileHash $PrepareExe -Algorithm SHA256).Hash
    shots_sha256=(Get-FileHash $Config -Algorithm SHA256).Hash
    protocol_sha256=(Get-FileHash $protocolPath -Algorithm SHA256).Hash
    width=$Width; height=$Height; seeds=$Seeds; shots=@(); jobs=$jobs
}
function Save-Manifest {
    $manifest | ConvertTo-Json -Depth 12 | Set-Content (Join-Path $Output 'manifest.json') -Encoding UTF8
    if ($rows.Count) { $rows | Export-Csv (Join-Path $Output 'runs.csv') -NoTypeInformation -Encoding UTF8 }
}
Save-Manifest

function Camera-Flags($camera) {
    return @('--camera') + @($camera.position | ForEach-Object { Number $_ }) +
        @('--camera-angles',(Number $camera.yaw),(Number $camera.pitch),
          '--fov',(Number $camera.fov))
}

function Render-Job($shot, $configCase, [int]$seed, [string]$scene,
                    $camera, [bool]$endCheck = $false) {
    $role = if ($endCheck) { 'end-check' } else { 'motion' }
    $name = "$($shot.name)--$($configCase.name)--$seed--$role"
    $png = Join-Path $Output ($name + '.png')
    $statsPath = Join-Path $Output ($name + '.csv')
    $logPath = Join-Path $Output ($name + '.log')
    $frames = if ($endCheck) { 1 } else { [int]$shot.frames }
    $flags = if ($endCheck) { @('--gpu') } else { @($configCase.flags) }
    $renderArgs = @($scene,'-W',"$Width",'-H',"$Height",'-f',"$frames",'--seed',"$seed") +
        @($protocol.common_flags) + (Camera-Flags $camera) +
        @('-o',$png,'--frame-stats',$statsPath) + $flags
    if (!$endCheck) {
        $delta = @($shot.move | ForEach-Object { Number $_ })
        $renderArgs += @('--move') + $delta + @('--move-entity','Ship') + $delta
        if ([double]$shot.turn -ne 0) {
            $renderArgs += @('--turn-entity','Ship',(Number $shot.turn))
        }
        $renderArgs += @('--dump-every','1')
    }
    $job = [pscustomobject]@{
        shot=$shot.name; config=$configCase.name; seed=$seed; role=$role
        arguments=$renderArgs; log=$logPath; image=$png; frame_stats=$statsPath
        status='planned'; image_sha256=$null
    }
    $jobs.Add($job)
    Save-Manifest
    if (!$PlanOnly) {
        Write-Host "Rendering $name ($frames frames)"
        & $Exe @renderArgs 2>&1 | Tee-Object -FilePath $logPath | Out-Host
        if ($LASTEXITCODE -ne 0) { throw "$name exited with $LASTEXITCODE" }
        $log = Get-Content -LiteralPath $logPath -Raw
        if ($log -match '(?i)\[[^\]]*(error|critical)[^\]]*\]|evaluating failed|no feature for') {
            throw "$name logged a rendering error"
        }
        $states = @(Import-Csv $statsPath)
        if ($states.Count -ne $frames) { throw "$name has incorrect frame accounting" }
        $scale = if ($endCheck) { 1.0 } else { [double]$configCase.scale }
        $active = if ($endCheck) { 'Off' } else { $configCase.upscaler }
        $rw = [int][Math]::Floor($Width * $scale + 0.5)
        $rh = [int][Math]::Floor($Height * $scale + 0.5)
        for ($index=0; $index -lt $states.Count; ++$index) {
            $state = $states[$index]
            $expectedSeed = $seed
            if (!$endCheck -and $configCase.name -eq 'raw-path') { $expectedSeed += $index }
            $expectedAccumulation = if ($endCheck -or !$configCase.accumulates) { 1 }
                else { $index+1 }
            $expectedReset = if ($active -ne 'Off' -and $index -eq 0) { 1 } else { 0 }
            if ([int]$state.frame -ne $index+1 -or [int]$state.seed -ne $expectedSeed -or
                [int]$state.spp -ne 1 -or [int]$state.render_width -ne $rw -or
                [int]$state.render_height -ne $rh -or [int]$state.output_width -ne $Width -or
                [int]$state.output_height -ne $Height -or $state.active_upscaler -ne $active -or
                [int]$state.accumulated_frames -ne $expectedAccumulation -or
                [int]$state.upscaler_history_reset -ne $expectedReset) {
                throw "$name frame $($index+1): state/dimensions/upscaler mismatch"
            }
        }
        $image = [Drawing.Bitmap]::FromFile($png)
        try {
            if ($image.Width -ne $Width -or $image.Height -ne $Height) { throw "${name}: PNG size" }
            $colors = New-Object 'System.Collections.Generic.HashSet[int]'
            for ($y=0; $y -lt $Height; $y+=7) {
                for ($x=0; $x -lt $Width; $x+=7) { [void]$colors.Add($image.GetPixel($x,$y).ToArgb()) }
            }
            if ($colors.Count -lt 16) { throw "$name appears blank" }
        } finally { $image.Dispose() }
        $job.image_sha256 = (Get-FileHash $png -Algorithm SHA256).Hash
        $job.status = 'passed'
    }
    $rows.Add([pscustomobject][ordered]@{
        shot=$shot.name; config=$configCase.name; seed=$seed
        gpu_ms=''; trace_ms=''; flip=''
        group=$configCase.group; run_role=$role; status=$job.status
        measurement_status='unmeasured_smoke'; frames=$frames
        output_width=$Width; output_height=$Height; image=$png
    })
    Save-Manifest
    return $job
}

foreach ($shot in $shots) {
    if ($shot.name -notmatch '^[a-z0-9-]+$') { throw 'Invalid shot name' }
    $sceneDir = Join-Path (Join-Path $Output 'scenes') $shot.name
    $prepareArgs = @('--scene',(Join-Path $Assets $spec.source_scene),
        '--model',(Join-Path $Assets $spec.ship_model),'--output',$sceneDir,
        '--position') + @($shot.position | ForEach-Object { Number $_ }) +
        @('--yaw',(Number $shot.yaw),'--pitch',(Number $shot.pitch),'--move') +
        @($shot.move | ForEach-Object { Number $_ }) +
        @('--turn',(Number $shot.turn),'--frames',"$($shot.frames)")
    if ($shot.camera) {
        $prepareArgs += @('--free-camera') +
            @($shot.camera.position | ForEach-Object { Number $_ }) +
            @('--view-yaw',(Number $shot.camera.yaw),'--view-pitch',(Number $shot.camera.pitch),
              '--fov',(Number $shot.camera.fov))
    }
    & $PrepareExe @prepareArgs | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "Could not prepare $($shot.name)" }
    $initial = Join-Path $sceneDir 'initial.yaml'
    $end = Join-Path $sceneDir 'end.yaml'
    $poses = Get-Content (Join-Path $sceneDir 'poses.json') -Raw | ConvertFrom-Json
    $manifest.shots += [pscustomobject]@{
        name=$shot.name; initial=$initial; end=$end; poses=$poses
        initial_sha256=(Get-FileHash $initial -Algorithm SHA256).Hash
        end_sha256=(Get-FileHash $end -Algorithm SHA256).Hash
    }
    for ($seedIndex=0; $seedIndex -lt $Seeds.Count; ++$seedIndex) {
        for ($offset=0; $offset -lt $selected.Count; ++$offset) {
            $configCase = $selected[($offset + $seedIndex) % $selected.Count]
            $job = Render-Job $shot $configCase $Seeds[$seedIndex] $initial $poses.initial
            if ($configCase.name -eq 'raw-path') {
                $endSeed = $Seeds[$seedIndex] + [int]$shot.frames - 1
                $reference = Render-Job $shot $configCase $endSeed $end $poses.end $true
                if (!$PlanOnly -and $job.image_sha256 -ne $reference.image_sha256) {
                    throw "$($shot.name): moving final sample and explicit end pose differ"
                }
            }
        }
    }
}
$manifest.complete = !$PlanOnly
Save-Manifest
Write-Host 'Shot run complete; gpu_ms/trace_ms/flip remain empty for this smoke-only runner.'
