[CmdletBinding()]
param(
    [int]$Port = 18081,
    [ValidateRange(1, 100000)]
    [int]$Requests = 500,
    [switch]$Business,
    [switch]$Build,
    [switch]$NoViewer,
    [switch]$OpenViewer,
    [switch]$Elevated
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$buildDir = Join-Path $root 'build-tracy'
$exe = Join-Path $buildDir 'RelWithDebInfo/hello_service.exe'
$pdb = Join-Path $buildDir 'RelWithDebInfo/hello_service.pdb'
$outputDir = Join-Path $root 'build-tracy/cpu-profiles'
$baseUrl = "http://127.0.0.1:$Port"

function Find-CompatibleWpa {
    $candidates = @(
        (Get-Command wpa.exe -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source -ErrorAction SilentlyContinue)
        (Get-ChildItem -Path 'C:\Program Files (x86)\Windows Kits\10\Windows Performance Toolkit\wpa.exe',
            'C:\Program Files (x86)\Windows Kits\11\Windows Performance Toolkit\wpa.exe',
            'C:\Program Files\Windows Kits\10\Windows Performance Toolkit\wpa.exe' -ErrorAction SilentlyContinue |
            Select-Object -ExpandProperty FullName)
    ) | Where-Object { $_ -and (Test-Path -LiteralPath $_ -PathType Leaf) } | Select-Object -Unique
    foreach ($candidate in $candidates) {
        $version = [version](Get-Item -LiteralPath $candidate).VersionInfo.FileVersion.Split(' ')[0]
        if ($version.Major -ge 10) { return $candidate }
    }
    return $null
}

if ($Port -lt 1 -or $Port -gt 65535) { throw 'Port must be between 1 and 65535.' }
if (-not (Get-Command wpr.exe -ErrorAction SilentlyContinue)) { throw 'Windows Performance Recorder (wpr.exe) is required.' }
if (-not (Test-Path -LiteralPath 'C:\Program Files (x86)\Windows Kits\10\Windows Performance Toolkit\xperf.exe' -PathType Leaf)) {
    throw 'xperf.exe (Windows 10/11 Performance Toolkit) is required for the function report.'
}
$wpa = if ($OpenViewer -and -not $NoViewer) { Find-CompatibleWpa } else { $null }
if ($OpenViewer -and -not $NoViewer -and -not $wpa) {
    throw 'No compatible WPA found. The installed Windows Kits 8.1 WPA (6.3) cannot open traces from Windows 11 WPR (10.0). Install Windows 10/11 ADK Windows Performance Toolkit, or use -NoViewer to capture an ETL for later analysis.'
}
$isAdmin = ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    if ($Elevated) { throw 'WPR requires an Administrator terminal.' }
    $arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"", '-Elevated',
        '-Port', $Port, '-Requests', $Requests)
    if ($Build) { $arguments += '-Build' }
    if ($Business) { $arguments += '-Business' }
    if ($NoViewer) { $arguments += '-NoViewer' }
    if ($OpenViewer) { $arguments += '-OpenViewer' }
    Write-Host 'Requesting Administrator permission for Windows CPU sampling...'
    $elevatedProcess = Start-Process -FilePath 'powershell.exe' -Verb RunAs -ArgumentList $arguments `
        -PassThru -Wait
    if ($elevatedProcess.ExitCode -ne 0) { throw "CPU profiling failed (exit code $($elevatedProcess.ExitCode))." }
    $report = Get-ChildItem -LiteralPath $outputDir -Filter 'hello_service_*.report.md' -File |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $report) { throw "CPU trace completed but no report found in $outputDir" }
    Write-Host "Report: $($report.FullName)"
    Get-Content -LiteralPath $report.FullName -TotalCount 28 | Out-Host
    exit 0
}
if ($Build -or -not (Test-Path -LiteralPath $exe -PathType Leaf) -or -not (Test-Path -LiteralPath $pdb -PathType Leaf)) {
    & cmake -S $root -B $buildDir -A x64 -DMF_ENABLE_OTEL=OFF -DMF_ENABLE_TRACY=ON `
        -DCMAKE_PREFIX_PATH=D:/vcpkg/installed/x64-windows
    if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed.' }
    & cmake --build $buildDir --config RelWithDebInfo --target hello_service
    if ($LASTEXITCODE -ne 0) { throw 'RelWithDebInfo build failed.' }
}
if (-not (Test-Path -LiteralPath $exe -PathType Leaf) -or -not (Test-Path -LiteralPath $pdb -PathType Leaf)) {
    throw "Missing executable or PDB: $exe / $pdb"
}
if (Get-NetTCPConnection -LocalPort $Port -State Listen -ErrorAction SilentlyContinue) {
    throw "Port $Port is already occupied. Choose another -Port."
}
$status = (& wpr.exe -status 2>&1 | Out-String)
if ($LASTEXITCODE -ne 0 -or $status -notmatch 'WPR is not recording') {
    throw "WPR is already recording or its status is unavailable. Do not stop somebody else's recording: $status"
}

New-Item -ItemType Directory -Path $outputDir -Force | Out-Null
$trace = Join-Path $outputDir ("hello_service_{0}.etl" -f (Get-Date -Format 'yyyyMMdd_HHmmss'))
$requestPath = [System.IO.Path]::ChangeExtension($trace, '.requests.csv')
$summaryPath = [System.IO.Path]::ChangeExtension($trace, '.workload.json')
$requestRows = [System.Collections.Generic.List[object]]::new()
$workloadTimer = $null
$stdout = Join-Path $outputDir 'hello_service.stdout.log'
$stderr = Join-Path $outputDir 'hello_service.stderr.log'
$service = $null
$recording = $false
$completed = $false
$traceSaved = $false
try {
    $env:PATH = "D:\OpenSSL-Win64\bin;D:\vcpkg\installed\x64-windows\bin;$env:PATH"
    $env:HELLO_SERVICE_HOST = '127.0.0.1'
    $env:HELLO_SERVICE_PORT = [string]$Port
    $env:HELLO_SERVICE_STORAGE_MODE = 'in_memory'
    $env:HELLO_SERVICE_ENV = 'development'
    $service = Start-Process -FilePath $exe -WorkingDirectory $root `
        -RedirectStandardOutput $stdout -RedirectStandardError $stderr -WindowStyle Hidden -PassThru

    $ready = $false
    for ($attempt = 0; $attempt -lt 40; ++$attempt) {
        Start-Sleep -Milliseconds 250
        if ($service.HasExited) { break }
        try {
            if ((Invoke-WebRequest -Uri "$baseUrl/health" -UseBasicParsing -TimeoutSec 1).StatusCode -eq 200) {
                $ready = $true
                break
            }
        } catch { }
    }
    if (-not $ready) { throw "Service did not become ready. Check $stdout and $stderr" }

    & wpr.exe -start CPU -filemode
    if ($LASTEXITCODE -ne 0) { throw 'WPR CPU recording failed. Try running the terminal as Administrator.' }
    $recording = $true

    $account = "profile_$([guid]::NewGuid().ToString('N').Substring(0, 12))"
    $password = 'ProfileOnly_12345!'
    $body = @{ login_name = $account; display_name = 'Profiler'; password = $password } | ConvertTo-Json
    $null = Invoke-RestMethod -Method Post -Uri "$baseUrl/auth/register" -ContentType 'application/json' -Body $body
    if ($Business) {
        $peer = "${account}_peer"
        $peerBody = @{ login_name = $peer; display_name = 'Profiler Peer'; password = $password } | ConvertTo-Json
        $peerRegistration = Invoke-RestMethod -Method Post -Uri "$baseUrl/auth/register" -ContentType 'application/json' -Body $peerBody
    }
    $login = Invoke-RestMethod -Method Post -Uri "$baseUrl/auth/login" -ContentType 'application/json' `
        -Body (@{ login_name = $account; password = $password } | ConvertTo-Json)
    $headers = @{ Authorization = "Bearer $($login.data.session.token)" }
    if (-not $login.data.session.token) { throw 'Login returned no token.' }

    if ($Business) {
        $peerId = [string]$peerRegistration.data.player.player_id
        if (-not $peerId) { throw 'Peer registration returned no player ID.' }
        $null = Invoke-RestMethod -Method Post -Uri "$baseUrl/friends/request" -Headers $headers `
            -ContentType 'application/json' -Body (@{ target_player_id = $peerId } | ConvertTo-Json)
        $peerHeaders = @{ Authorization = "Bearer $($peerRegistration.data.session.token)" }
        if (-not $peerRegistration.data.session.token) { throw 'Peer registration returned no token.' }
        $null = Invoke-RestMethod -Method Post -Uri "$baseUrl/friends/respond" -Headers $peerHeaders `
            -ContentType 'application/json' -Body (@{ requester_player_id = [string]$login.data.player.player_id; accept = $true } | ConvertTo-Json)
        $null = Invoke-RestMethod -Method Post -Uri "$baseUrl/mail/send-player" -Headers $peerHeaders `
            -ContentType 'application/json' -Body (@{ to_player_id = [string]$login.data.player.player_id; title = 'Profiling mail'; body = 'Local in-memory workload' } | ConvertTo-Json)
        $paths = @('/players/me', '/friends', '/mail', '/chat/unread', '/players/me/quests',
            '/shop/products', '/gacha/pools', '/friends/requests', '/chat/history/lobby',
            '/quests', '/shop/history', '/gacha/history')
    } else {
        $paths = @('/players/me', '/players', '/config')
    }
    $workloadTimer = [System.Diagnostics.Stopwatch]::StartNew()
    for ($i = 0; $i -lt $Requests; ++$i) {
        $path = $paths[$i % $paths.Count]
        $method = if ($path -in @('/shop/history', '/gacha/history')) { 'Post' } else { 'Get' }
        $timer = [System.Diagnostics.Stopwatch]::StartNew()
        $response = $null
        $statusCode = 0
        $serverMs = $null
        try {
            $requestArgs = @{ Method = $method; Uri = "$baseUrl$path"; Headers = $headers; TimeoutSec = 10; UseBasicParsing = $true }
            if ($method -eq 'Post') {
                $requestArgs.ContentType = 'application/json'
                $requestArgs.Body = '{"limit":20,"offset":0}'
            }
            $response = Invoke-WebRequest @requestArgs
            $statusCode = [int]$response.StatusCode
            $parsedMs = 0.0
            if ([double]::TryParse([string]$response.Headers['X-Response-Time-Ms'],
                [System.Globalization.NumberStyles]::Float, [System.Globalization.CultureInfo]::InvariantCulture,
                [ref]$parsedMs)) { $serverMs = $parsedMs }
        } catch {
            if ($_.Exception.Response) { $statusCode = [int]$_.Exception.Response.StatusCode }
            if (-not $statusCode) { $statusCode = -1 }
        } finally {
            $timer.Stop()
            $requestRows.Add([pscustomobject]@{
                Path = "$($method.ToUpperInvariant()) $path"
                StatusCode = $statusCode
                ClientMs = [math]::Round($timer.Elapsed.TotalMilliseconds, 3)
                ServerMs = $serverMs
            })
        }
    }
    if ($Business -and ($requestRows | Where-Object { $_.StatusCode -lt 200 -or $_.StatusCode -ge 400 } | Select-Object -First 1)) {
        Write-Warning 'Some business requests failed; inspect the report for route-specific error counts.'
    }
    $workloadTimer.Stop()
    $completed = $true
} finally {
    if ($workloadTimer -and $workloadTimer.IsRunning) { $workloadTimer.Stop() }
    if ($recording) {
        & wpr.exe -stop $trace
        $traceSaved = $LASTEXITCODE -eq 0 -and (Test-Path -LiteralPath $trace -PathType Leaf) -and
            (Get-Item -LiteralPath $trace).Length -gt 0
        if (-not $traceSaved) { Write-Warning "WPR could not save trace to $trace" }
    }
    if ($service -and -not $service.HasExited) { Stop-Process -Id $service.Id -Force }
    if ($requestRows.Count -gt 0) {
        $requestRows | Export-Csv -LiteralPath $requestPath -NoTypeInformation -Encoding UTF8
        @{ Requests = $requestRows.Count; ElapsedMs = [math]::Round($workloadTimer.Elapsed.TotalMilliseconds, 3) } |
            ConvertTo-Json | Set-Content -LiteralPath $summaryPath -Encoding UTF8
    }
}

if (-not $completed) { throw 'Workload did not finish; see service logs and trace for partial results.' }
if (-not $traceSaved) { throw "CPU recording failed; no valid trace at $trace" }
Write-Host "CPU trace: $trace"
Write-Host "PDB: $pdb"
& (Join-Path $PSScriptRoot 'export-cpu-hotspots.ps1') -Trace $trace
if ($OpenViewer -and -not $NoViewer) {
    Start-Process -FilePath $wpa -ArgumentList @("`"$trace`"") | Out-Null
}
