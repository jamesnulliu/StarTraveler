param([Parameter(Mandatory=$true)][string]$OutputPath)
$ErrorActionPreference = 'Stop'
# Keep one counter query alive; process names distinguish the renderer from desktop apps.
Get-Counter '\GPU Engine(*)\Utilization Percentage' -SampleInterval 1 -Continuous |
    ForEach-Object {
        $stamp = $_.Timestamp.ToUniversalTime().ToString('o')
        $rows = foreach ($sample in $_.CounterSamples) {
            if ($sample.CookedValue -gt 0.1 -and $sample.InstanceName -match '^pid_(\d+)_') {
                $taskPid = [int]$Matches[1]
                $task = Get-Process -Id $taskPid -ErrorAction SilentlyContinue
                [pscustomobject]@{
                    utc = $stamp
                    pid = $taskPid
                    process = if ($task) { $task.ProcessName } else { 'exited' }
                    engine = $sample.InstanceName
                    percent = $sample.CookedValue
                }
            }
        }
        # A heartbeat makes missing/broken counter collection detectable even when idle.
        [pscustomobject]@{utc=$stamp; pid=0; process='heartbeat'; engine=''; percent=0} |
            Export-Csv -LiteralPath $OutputPath -Append -NoTypeInformation -Encoding UTF8
        if ($rows) {
            $rows | Export-Csv -LiteralPath $OutputPath -Append -NoTypeInformation -Encoding UTF8
        }
    }
