[CmdletBinding()]
param(
    [string]$Trace,
    [ValidateRange(1, 200)]
    [int]$Top = 25
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$profileDir = Join-Path $root 'build-tracy/cpu-profiles'
if (-not $Trace) {
    $latest = Get-ChildItem -LiteralPath $profileDir -Filter 'hello_service_*.etl' -File |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $latest) { throw "No ETL found in $profileDir" }
    $Trace = $latest.FullName
}
$Trace = (Resolve-Path -LiteralPath $Trace).Path
$xperf = 'C:\Program Files (x86)\Windows Kits\10\Windows Performance Toolkit\xperf.exe'
$pdbDir = Join-Path $root 'build-tracy/RelWithDebInfo'
if (-not (Test-Path -LiteralPath $xperf -PathType Leaf)) { throw "Install the Windows 10/11 Performance Toolkit: $xperf" }
if (-not (Test-Path -LiteralPath (Join-Path $pdbDir 'hello_service.pdb') -PathType Leaf)) {
    throw "PDB missing from $pdbDir; build RelWithDebInfo first."
}
$env:_NT_SYMBOL_PATH = $pdbDir
$env:_NT_SYMCACHE_PATH = Join-Path $profileDir 'SymCache'
$raw = [System.IO.Path]::ChangeExtension($Trace, '.functions.txt')
& $xperf -i $Trace -symbols -a profile -detail -ao $raw
if ($LASTEXITCODE -ne 0) { throw "xperf failed ($LASTEXITCODE); check the ETL and PDB." }

$rows = [System.Collections.Generic.List[object]]::new()
$unresolved = 0
$processWeight = [long]0
$moduleWeight = [long]0
$moduleWeights = @{}
$pattern = '^hello_service\.exe\s*\(\s*\d+\),\s*(?<weight>\d+),\s*[\d.]+,\s*(?<module>.+?)!(?<function>.+)$'
foreach ($line in [System.IO.File]::ReadLines($raw)) {
    if ($line -notmatch $pattern) { continue }
    $weight = [long]$Matches['weight']
    $processWeight += $weight
    $module = $Matches['module'].Trim().Trim('"')
    if (-not $moduleWeights.ContainsKey($module)) { $moduleWeights[$module] = [long]0 }
    $moduleWeights[$module] += $weight
    if ($module -ne 'hello_service.exe') { continue }
    $moduleWeight += $weight
    $name = $Matches['function'].Trim().Trim('"')
    if ($name -eq 'Unknown') { $unresolved++; continue }
    $rows.Add([pscustomobject]@{
        Weight = $weight
        Function = $name
    })
}
if ($rows.Count -eq 0) { throw "No resolved hello_service.exe functions found in $raw. Check matching PDB/symbols." }

$stem = [System.IO.Path]::Combine([System.IO.Path]::GetDirectoryName($Trace),
    [System.IO.Path]::GetFileNameWithoutExtension($Trace))
$all = $rows | Sort-Object Weight -Descending
$project = $all | Where-Object { $_.Function -match '^(`)?mf::|^(`)?register_.*_routes|^main$' }
$all = @($all | ForEach-Object {
    [pscustomobject]@{ Weight = $_.Weight; ProcessPct = [math]::Round(100.0 * $_.Weight / $processWeight, 3);
        ModulePct = [math]::Round(100.0 * $_.Weight / $moduleWeight, 3); Function = $_.Function }
})
$project = @($all | Where-Object { $_.Function -match '^(`)?mf::|^(`)?register_.*_routes|^main$' })
$allPath = "$stem.top-functions.csv"
$projectPath = "$stem.project-functions.csv"
$all | Export-Csv -LiteralPath $allPath -NoTypeInformation -Encoding UTF8
$project | Export-Csv -LiteralPath $projectPath -NoTypeInformation -Encoding UTF8

function Percentile([double[]]$sorted, [double]$fraction) {
    if ($sorted.Count -eq 0) { return 0 }
    return $sorted[[math]::Max(0, [math]::Ceiling($fraction * $sorted.Count) - 1)]
}

$report = [System.Collections.Generic.List[string]]::new()
$report.Add('# hello_service CPU and request latency')
$report.Add('')
$report.Add("Trace: ``$Trace``")
$report.Add('')
$requestPath = "$stem.requests.csv"
$requestStats = @()
if (Test-Path -LiteralPath $requestPath -PathType Leaf) {
    $requestRows = @(Import-Csv -LiteralPath $requestPath)
    $requestStats = @($requestRows | Group-Object Path | ForEach-Object {
        $group = $_.Group
        $sorted = @($group | ForEach-Object {
            [double]::Parse($_.ClientMs, [System.Globalization.CultureInfo]::InvariantCulture)
        } | Sort-Object)
        $server = @($group | Where-Object { $_.ServerMs -ne '' } | ForEach-Object {
            [double]::Parse($_.ServerMs, [System.Globalization.CultureInfo]::InvariantCulture)
        } | Sort-Object)
        $failures = @($group | Where-Object { [int]$_.StatusCode -lt 200 -or [int]$_.StatusCode -ge 400 }).Count
        [pscustomobject]@{
            Path = $_.Name; Count = $group.Count; Failures = $failures
            AverageMs = [math]::Round(($sorted | Measure-Object -Average).Average, 2)
            P50Ms = [math]::Round((Percentile $sorted 0.50), 2)
            P95Ms = [math]::Round((Percentile $sorted 0.95), 2)
            P99Ms = [math]::Round((Percentile $sorted 0.99), 2)
            MaxMs = [math]::Round($sorted[-1], 2)
            ServerP95Ms = if ($server.Count -gt 0) { [math]::Round((Percentile $server 0.95), 2) } else { 'n/a' }
        }
    } | Sort-Object P95Ms -Descending)
}
$report.Add('## What to inspect first')
$report.Add('')
if ($requestStats.Count -gt 0) {
    $withFailures = @($requestStats | Where-Object { $_.Failures -gt 0 })
    foreach ($entry in $withFailures) {
        $report.Add("- Fix request failures first: ``$($entry.Path)`` failed $($entry.Failures)/$($entry.Count) times.")
    }
    $slowest = $requestStats[0]
    if (@($requestStats | Where-Object { $_.ServerP95Ms -ne 'n/a' -and [double]$_.ServerP95Ms -gt 0 }).Count -eq 0) {
        $report.Add('- Server P95 is zero or unavailable for every route. Client latency includes PowerShell overhead; do not optimize routes based on its ranking alone. Use server-side sub-millisecond timers or Tracy zones for function duration.')
    } else {
        $report.Add("- Highest client P95: ``$($slowest.Path)`` $($slowest.P95Ms) ms (P99 $($slowest.P99Ms) ms). Compare its server P95 and repeat under representative load before optimizing.")
    }
} else {
    $report.Add('- No matching request measurements (older trace); rerun profile-cpu.ps1 for route latency.')
}
if ($project.Count -gt 0 -and $project[0].ProcessPct -ge 1) {
    $leader = $project[0]
    $report.Add("- Inspect CPU candidate $([System.Net.WebUtility]::HtmlEncode($leader.Function)): $($leader.ProcessPct)% of sampled process weight. This is a sample share, not execution time or proof of a bottleneck.")
} else { $report.Add('- No dominant project-function CPU hotspot in this capture. Review module distribution and collect more representative load before editing code.') }
$report.Add('')
$report.Add('## Request latency (milliseconds, client wall clock)')
$report.Add('')
if ($requestStats.Count -eq 0) { $report.Add('No request measurements available.') }
else {
    $summaryPath = "$stem.workload.json"
    if (Test-Path -LiteralPath $summaryPath -PathType Leaf) {
        $workload = Get-Content -LiteralPath $summaryPath -Raw | ConvertFrom-Json
        if ($workload.ElapsedMs -gt 0) {
            $report.Add("Workload: $($workload.Requests) serial requests in $($workload.ElapsedMs) ms ($([math]::Round(1000.0 * $workload.Requests / $workload.ElapsedMs, 1)) requests/s, serial only).")
            $report.Add('')
        }
    }
    $report.Add('| Route | Requests | Failures | Avg | P50 | P95 | P99 | Max | Server P95 |')
    $report.Add('| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |')
    foreach ($entry in $requestStats) {
        $report.Add("| $($entry.Path) | $($entry.Count) | $($entry.Failures) | $($entry.AverageMs) | $($entry.P50Ms) | $($entry.P95Ms) | $($entry.P99Ms) | $($entry.MaxMs) | $($entry.ServerP95Ms) |")
    }
}
$report.Add('')
$report.Add('Client time includes HTTP and PowerShell overhead. Server P95 uses X-Response-Time-Ms when present; this header may round fast requests to zero. Requests run sequentially against in-memory storage; these numbers do not measure concurrent capacity, MySQL or Redis.')
$report.Add('')
$report.Add('## Process CPU module distribution')
$report.Add('')
$report.Add('| Module | Weight | Process % |')
$report.Add('| --- | ---: | ---: |')
foreach ($module in ($moduleWeights.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 10)) {
    $report.Add("| $([System.Net.WebUtility]::HtmlEncode($module.Key)) | $($module.Value) | $([math]::Round(100.0 * $module.Value / $processWeight, 2)) |")
}
$report.Add('')
$report.Add('## Project CPU samples (exclusive instruction pointer)')
$report.Add('')
$report.Add("Process weight: $processWeight; hello_service.exe module weight: $moduleWeight; resolved function rows: $($rows.Count); unresolved module rows: $unresolved.")
$report.Add('')
$report.Add('| Function | Weight | Process % | Module % |')
$report.Add('| --- | ---: | ---: | ---: |')
foreach ($entry in ($project | Select-Object -First $Top)) {
    $report.Add("| $([System.Net.WebUtility]::HtmlEncode($entry.Function)) | $($entry.Weight) | $($entry.ProcessPct) | $($entry.ModulePct) |")
}
$report.Add('')
$report.Add('Weight is the xperf profile sampling weight, NOT milliseconds, calls or wall-clock time. Process % divides by all sampled weights attributed to hello_service.exe (including libraries and unresolved symbols); Module % divides by hello_service.exe module weight. Exclusive samples do not include callees. Small percentages, inlining, short captures and background activity make rankings uncertain. Use WPA call stacks and repeat runs before changing code.')
$reportPath = "$stem.report.md"
$report | Set-Content -LiteralPath $reportPath -Encoding UTF8
Write-Host "Report: $reportPath"
$report | Select-Object -First 28 | Out-Host
Write-Host "CSV: $projectPath (project); $allPath (all resolved service functions)"
