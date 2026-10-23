# Idle resource statistics for the dock: CPU, RSS, handles, threads.
#
# Sampled over a window with the pointer parked away from the dock, which is the
# state the app spends most of its life in. Hover cost is measured separately by
# tools/hover_bench.ps1.
param(
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$ProcessName = 'DockForge',
    [int]$Seconds = 60,
    [int]$IntervalMs = 5000
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$proc = Get-Process -Name $ProcessName -ErrorAction SilentlyContinue
if ($proc -eq $null) { throw "process $ProcessName is not running" }

$lines = New-Object System.Collections.Generic.List[string]
$lines.Add("process=$ProcessName pid=$($proc.Id)")
$lines.Add("duration_s=$Seconds interval_ms=$IntervalMs")

$cpuStart = $proc.TotalProcessorTime.TotalSeconds
$start = Get-Date
$samples = @()

$elapsed = 0
while ($elapsed -lt $Seconds * 1000) {
    Start-Sleep -Milliseconds $IntervalMs
    $elapsed += $IntervalMs
    $proc.Refresh()
    $samples += [pscustomobject]@{
        t         = $elapsed / 1000.0
        rss_mb    = [Math]::Round($proc.WorkingSet64 / 1MB, 2)
        handles   = $proc.HandleCount
        threads   = $proc.Threads.Count
        cpu_s     = [Math]::Round($proc.TotalProcessorTime.TotalSeconds, 3)
    }
}

$proc.Refresh()
$cpuEnd = $proc.TotalProcessorTime.TotalSeconds
$wall = ((Get-Date) - $start).TotalSeconds
$cores = [Environment]::ProcessorCount
$cpuPct = 100.0 * ($cpuEnd - $cpuStart) / ($wall * $cores)

$lines.Add("cpu_total_s=$([Math]::Round($cpuEnd - $cpuStart, 3)) wall_s=$([Math]::Round($wall, 1)) cores=$cores")
$lines.Add("cpu_percent=$([Math]::Round($cpuPct, 4))")
$lines.Add("rss_first_mb=$($samples[0].rss_mb) rss_last_mb=$($samples[$samples.Count - 1].rss_mb)")
$lines.Add("rss_growth_mb=$([Math]::Round($samples[$samples.Count - 1].rss_mb - $samples[0].rss_mb, 2))")
$lines.Add("handles_first=$($samples[0].handles) handles_last=$($samples[$samples.Count - 1].handles)")
$lines.Add("handles_min=$(($samples | Measure-Object handles -Minimum).Minimum) handles_max=$(($samples | Measure-Object handles -Maximum).Maximum)")
$lines.Add("threads_min=$(($samples | Measure-Object threads -Minimum).Minimum) threads_max=$(($samples | Measure-Object threads -Maximum).Maximum)")
$lines.Add("--- samples ---")
foreach ($s in $samples) {
    $lines.Add(("t={0,5:N1}s rss={1,7:N2}MB handles={2,4} threads={3} cpu={4,7:N3}s" -f $s.t, $s.rss_mb, $s.handles, $s.threads, $s.cpu_s))
}

# Budgets from PERFORMANCE.md.
$lines.Add("--- budgets ---")
$lines.Add("cpu_budget=0.5% rss_budget=120MB")
$lines.Add("verdict_cpu=$(if ($cpuPct -lt 0.5) { 'PASS' } else { 'FAIL' })")
$lines.Add("verdict_rss=$(if ($samples[$samples.Count - 1].rss_mb -lt 120) { 'PASS' } else { 'FAIL' })")
$lines.Add("verdict_handles=$(if ($samples[$samples.Count - 1].handles -le $samples[0].handles + 5) { 'PASS' } else { 'FAIL' })")

$lines | ForEach-Object { $_ }
[System.IO.File]::WriteAllText($Out, ($lines -join "`r`n"), (New-Object System.Text.UTF8Encoding($false)))
