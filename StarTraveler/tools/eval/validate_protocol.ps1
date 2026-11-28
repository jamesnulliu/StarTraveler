param(
    [Parameter(Mandatory=$true)][string]$Exe,
    [Parameter(Mandatory=$true)][string]$Scene,
    [Parameter(Mandatory=$true)][string]$Output,
    [string]$Config = (Join-Path $PSScriptRoot 'denoiser-protocol.json'),
    [ValidateRange(192,16384)][int]$Width = 384,
    [ValidateRange(108,16384)][int]$Height = 216
)
$ErrorActionPreference = 'Stop'
$Exe = (Resolve-Path -LiteralPath $Exe).Path
$Scene = (Resolve-Path -LiteralPath $Scene).Path
$protocol = Get-Content -LiteralPath $Config -Raw | ConvertFrom-Json
New-Item -ItemType Directory -Force -Path $Output | Out-Null
$Output = (Resolve-Path -LiteralPath $Output).Path
$results = @()

function Invoke-Case([string]$Name, [string[]]$Flags, [int]$Frames = 4,
                     [int]$Seed = $protocol.seed_base) {
    $png = Join-Path $Output ($Name + '.png')
    $csv = Join-Path $Output ($Name + '.csv')
    $logPath = Join-Path $Output ($Name + '.log')
    $renderArgs = @($Scene,'-W',"$Width",'-H',"$Height",'-f',"$Frames") +
        @($protocol.common_flags) + @($protocol.validation_camera_flags) +
        @('--seed',"$Seed",'-o',$png,'--frame-stats',$csv) + $Flags
    Write-Host "Running $Name"
    & $Exe @renderArgs 2>&1 | Tee-Object -FilePath $logPath | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "$Name exited with $LASTEXITCODE" }
    $log = Get-Content -LiteralPath $logPath -Raw
    if ($log -match '(?i)\[[^\]]*(error|critical)[^\]]*\]|evaluating failed|no feature for') {
        throw "$Name logged a rendering error"
    }
    $rows = @(Import-Csv -LiteralPath $csv)
    if ($rows.Count -ne $Frames) { throw "${Name}: wrong frame count" }
    return [pscustomobject]@{ Name=$Name; Rows=$rows; Png=$png; Log=$logPath; Args=$renderArgs }
}

foreach ($configCase in $protocol.configurations) {
    $flags = @($configCase.flags)
    if ($configCase.name -eq 'raw-path') { $flags += @('--dump-every','1') }
    $case = Invoke-Case $configCase.name $flags
    $expectedWidth = [int][Math]::Floor($Width * $configCase.scale + 0.5)
    $expectedHeight = [int][Math]::Floor($Height * $configCase.scale + 0.5)
    for ($index=0; $index -lt $case.Rows.Count; ++$index) {
        $row = $case.Rows[$index]
        $expectedAccumulation = if ($configCase.accumulates) { $index+1 } else { 1 }
        $expectedReset = if ($configCase.upscaler -eq 'Off') { 0 }
            elseif (!$configCase.accumulates -or $index -eq 0) { 1 } else { 0 }
        $expectedSeed = [int]$protocol.seed_base
        if ($configCase.name -eq 'raw-path') { $expectedSeed += $index }
        if ([int]$row.frame -ne $index+1 -or [int]$row.seed -ne $expectedSeed -or
            [int]$row.spp -ne 1 -or [int]$row.accumulated_frames -ne $expectedAccumulation -or
            [int]$row.render_width -ne $expectedWidth -or
            [int]$row.render_height -ne $expectedHeight -or
            [int]$row.output_width -ne $Width -or [int]$row.output_height -ne $Height -or
            $row.active_upscaler -ne $configCase.upscaler -or
            [int]$row.upscaler_history_reset -ne $expectedReset) {
            throw "$($case.Name): frame $($index+1) state differs from protocol"
        }
    }
    $results += [pscustomobject]@{
        Name=$case.Name; Group=$configCase.group; Passed=$true
        RenderWidth=$expectedWidth; RenderHeight=$expectedHeight
        ActiveUpscaler=$configCase.upscaler
        AccumulatedFrames=@($case.Rows.accumulated_frames)
        HistoryResets=@($case.Rows.upscaler_history_reset)
        Sha256=(Get-FileHash -LiteralPath $case.Png -Algorithm SHA256).Hash
        Arguments=$case.Args
    }
    $results | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $Output 'results.json') -Encoding UTF8
}

# Independent known-answer render: the fourth raw frame must equal a fresh
# one-sample render at seed 20 (17 + zero-based frame index 3), byte for byte.
$single = Invoke-Case 'raw-final-independent' @('--gpu') 1 ([int]$protocol.seed_base+3)
$raw = $results | Where-Object Name -eq 'raw-path'
if ((Get-FileHash -LiteralPath $single.Png -Algorithm SHA256).Hash -ne $raw.Sha256) {
    throw 'Raw frame contains history or does not use the documented seed stream'
}
$default = $results | Where-Object Name -eq 'gpu-ignores-dlss'
if ($default.Sha256 -eq $raw.Sha256) { throw 'Raw and accumulated controls unexpectedly match' }
$rawFrameHashes = @(2..4 | ForEach-Object {
    (Get-FileHash -LiteralPath (Join-Path $Output ('raw-path_{0:0000}.png' -f $_)) -Algorithm SHA256).Hash
})
if (@($rawFrameHashes | Select-Object -Unique).Count -ne 3) { throw 'Raw noise is frozen' }
$checks = [ordered]@{
    RawFinalEqualsIndependentSingle=$true; RawDiffersFromAccumulated=$true
    RawFramesHaveDistinctHashes=$true
}
$checks | ConvertTo-Json | Set-Content (Join-Path $Output 'checks.json') -Encoding UTF8
Write-Output 'Protocol validation passed. CSV states are not performance measurements.'
