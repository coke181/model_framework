[CmdletBinding()]
param(
    [ValidateRange(1, 256)][int]$Concurrency = 24,
    [ValidateRange(12, 1000000)][int]$Requests = 1000000,
    [ValidateRange(1, 120)][int]$DurationSeconds = 10,
    [ValidateRange(1, 65535)][int]$Port = 18083,
    [ValidateSet('in_memory', 'mysql_redis')][string]$StorageMode = 'in_memory',
    [string]$MySqlHost = '127.0.0.1',
    [int]$MySqlPort = 3306,
    [string]$MySqlUser = 'game_backend',
    [string]$MySqlPassword = 'GameBackend_2026!',
    [string]$MySqlDatabase = 'game_backend',
    [string]$RedisHost = '127.0.0.1',
    [int]$RedisPort = 6379,
    [string]$RedisPassword = '',
    [int]$RedisDb = 0
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$exe = Join-Path $root 'build-tracy/RelWithDebInfo/hello_service.exe'
if (-not (Test-Path -LiteralPath $exe)) { throw 'Build hello_service RelWithDebInfo first.' }
if (Get-NetTCPConnection -LocalPort $Port -State Listen -ErrorAction SilentlyContinue) {
    throw "Port $Port is occupied."
}
$env:PATH = "D:\vcpkg\installed\x64-windows\bin;D:\OpenSSL-Win64\bin;$env:PATH"
$env:HELLO_SERVICE_HOST = '127.0.0.1'
$env:HELLO_SERVICE_PORT = [string]$Port
$env:HELLO_SERVICE_STORAGE_MODE = $StorageMode
$env:HELLO_SERVICE_ENV = 'development'
$env:HELLO_SERVICE_REQUEST_LOG_SAMPLE_RATE = '10000'
$env:HELLO_SERVICE_MYSQL_HOST = $MySqlHost
$env:HELLO_SERVICE_MYSQL_PORT = [string]$MySqlPort
$env:HELLO_SERVICE_MYSQL_USER = $MySqlUser
$env:HELLO_SERVICE_MYSQL_PASSWORD = $MySqlPassword
$env:HELLO_SERVICE_MYSQL_DATABASE = $MySqlDatabase
$env:HELLO_SERVICE_REDIS_HOST = $RedisHost
$env:HELLO_SERVICE_REDIS_PORT = [string]$RedisPort
$env:HELLO_SERVICE_REDIS_PASSWORD = $RedisPassword
$env:HELLO_SERVICE_REDIS_DB = [string]$RedisDb
$baseUrl = "http://127.0.0.1:$Port"
$service = $null
try {
    $env:NO_PROXY = '127.0.0.1,localhost'
    $service = Start-Process -FilePath $exe -WorkingDirectory $root -PassThru -WindowStyle Hidden `
        -RedirectStandardOutput (Join-Path $env:TEMP 'business-latency-stdout.log') `
        -RedirectStandardError (Join-Path $env:TEMP 'business-latency-stderr.log')
    $ready = $false
    for ($i = 0; $i -lt 40; $i++) {
        Start-Sleep -Milliseconds 250
        if ($service.HasExited) { break }
        try {
            if ((Invoke-WebRequest -Uri "$baseUrl/health" -TimeoutSec 1 -UseBasicParsing).StatusCode -eq 200) {
                $ready = $true
                break
            }
        } catch { $healthError = $_.Exception.Message }
    }
    if (-not $ready) { throw "Health check failed; service exited=$($service.HasExited); error=$healthError" }

    $account = "load$([guid]::NewGuid().ToString('N').Substring(0, 12))"
    $password = 'ProfileOnly_12345!'
    $alice = Invoke-RestMethod -Method Post -Uri "$baseUrl/auth/register" -ContentType 'application/json' `
        -Body (@{ login_name = $account; display_name = 'Alice'; password = $password } | ConvertTo-Json)
    $bob = Invoke-RestMethod -Method Post -Uri "$baseUrl/auth/register" -ContentType 'application/json' `
        -Body (@{ login_name = "${account}b"; display_name = 'Bob'; password = $password } | ConvertTo-Json)
    $aliceHeaders = @{ Authorization = "Bearer $($alice.data.session.token)" }
    $bobHeaders = @{ Authorization = "Bearer $($bob.data.session.token)" }
    $null = Invoke-RestMethod -Method Post -Uri "$baseUrl/friends/request" -Headers $aliceHeaders `
        -ContentType 'application/json' -Body (@{ target_player_id = $bob.data.player.player_id } | ConvertTo-Json)
    $null = Invoke-RestMethod -Method Post -Uri "$baseUrl/friends/respond" -Headers $bobHeaders `
        -ContentType 'application/json' -Body (@{ requester_player_id = $alice.data.player.player_id; accept = $true } | ConvertTo-Json)
    $null = Invoke-RestMethod -Method Post -Uri "$baseUrl/mail/send-player" -Headers $bobHeaders `
        -ContentType 'application/json' -Body (@{ to_player_id = $alice.data.player.player_id; title = 'Perf'; body = 'Local test' } | ConvertTo-Json)

    Add-Type -Path (Join-Path $PSScriptRoot 'business-load-client.cs')
    $service.Refresh()
    $serviceCpuBefore = $service.TotalProcessorTime
    $clientProcess = [System.Diagnostics.Process]::GetCurrentProcess()
    $clientCpuBefore = $clientProcess.TotalProcessorTime
    $loadTimer = [System.Diagnostics.Stopwatch]::StartNew()
    $result = [BusinessLoadClient]::Run($baseUrl, [string]$alice.data.session.token,
        $Concurrency, $Requests, $DurationSeconds)
    $loadTimer.Stop()
    $service.Refresh()
    $clientProcess.Refresh()
    $cpuResult = 'server_cpu_cores={0:F2} client_cpu_cores={1:F2} logical_processors={2}' -f `
        (($service.TotalProcessorTime - $serviceCpuBefore).TotalSeconds / $loadTimer.Elapsed.TotalSeconds), `
        (($clientProcess.TotalProcessorTime - $clientCpuBefore).TotalSeconds / $loadTimer.Elapsed.TotalSeconds), `
        [Environment]::ProcessorCount
    $outputDir = Join-Path $root 'build-tracy/cpu-profiles'
    New-Item -Path $outputDir -ItemType Directory -Force | Out-Null
    $reportPath = Join-Path $outputDir ("business_latency_{0}.txt" -f (Get-Date -Format 'yyyyMMdd_HHmmss'))
    @("Storage: $StorageMode; accounts: 2; friends: 1; mails: 1; no seeded quests or gacha pools.",
        "Workload: 12 authenticated read routes, $Concurrency concurrent clients, max ${DurationSeconds}s.",
        'ServerMs: X-Response-Time-Ms measured inside request middleware; excludes network/queue time.',
        'QueueMs: enqueue to business worker start; ProcessingMs: full read to write start; both exclude socket write and client wait.',
        $cpuResult,
        $result) | Set-Content -LiteralPath $reportPath -Encoding UTF8
    Write-Output $cpuResult
    Write-Output $result
    Write-Output "Report: $reportPath"
} finally {
    if ($service -and -not $service.HasExited) { Stop-Process -Id $service.Id -Force }
}
