[CmdletBinding()]
param(
    [string]$ProjectRoot = "D:\code\model",
    [string]$ServiceExePath = "D:\code\model\build-tracy\RelWithDebInfo\hello_service.exe",
    [int]$BasePort = 8080,
    [int]$Instances = 2,
    [string]$StorageMode = "mysql_redis",
    [string]$LogDirectory = "D:\code\model\build-tracy",
    [string]$MySqlHost = "127.0.0.1",
    [int]$MySqlPort = 3306,
    [string]$MySqlUser = "game_backend",
    [string]$MySqlPassword = $(if ($env:HELLO_SERVICE_MYSQL_PASSWORD) { $env:HELLO_SERVICE_MYSQL_PASSWORD } else { 'change_me_mysql_password' }),
    [string]$MySqlDatabase = "game_backend",
    [string]$RedisHost = "127.0.0.1",
    [int]$RedisPort = 6379,
    [string]$RedisPassword = "",
    [int]$RedisDb = 0,
    [int]$RedisPoolSize = 16,
    [int]$PasswordHashThreads = 0,
    [int]$PasswordHashMaxPending = 0,
    [int]$PasswordVerifyCacheTtlMs = -1,
    [int]$BusinessThreads = 0,
    [int]$AcceptConcurrency = 0,
    [int]$RegisterMaxPerIp = 0,
    [int]$RegisterMaxPerLoginIp = 0,
    [switch]$PerfMode,
    [switch]$InsecureFastPasswordHash,
    [switch]$InMemorySessions,
    [switch]$InMemoryAuthSecurity,
    [string]$RedisSentinels = "",
    [string]$RedisSentinelMasterName = "",
    [bool]$SerializeMutatingRequests = $true,
    [switch]$StopExisting
)

# 文件职责：
# 在同机快速拉起多个 hello_service 实例，供 Nginx/LB 分流做水平扩展压测。
# 每个实例只覆盖一个端口，账号/玩家/风控/会话仍共享同一套 MySQL + Redis，
# 因此密码校验这种 CPU 密集工作会随实例数近似线性扩展。

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if ($Instances -lt 1) { throw "-Instances 必须大于等于 1" }
if (-not (Test-Path -LiteralPath $ServiceExePath)) {
    throw "未找到服务程序：$ServiceExePath。请先编译 hello_service。"
}
if (-not (Test-Path -LiteralPath $ProjectRoot)) {
    throw "未找到项目目录：$ProjectRoot"
}

New-Item -ItemType Directory -Force -Path $LogDirectory | Out-Null

if ($StopExisting) {
    $normalizedExe = [System.IO.Path]::GetFullPath($ServiceExePath)
    Get-CimInstance Win32_Process -Filter "Name = 'hello_service.exe'" |
        Where-Object {
            $_.ExecutablePath -and
            [System.StringComparer]::OrdinalIgnoreCase.Equals(
                [System.IO.Path]::GetFullPath($_.ExecutablePath),
                $normalizedExe)
        } |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
    Start-Sleep -Milliseconds 800
}

$env:PATH = "D:\OpenSSL-Win64\bin;D:\vcpkg\installed\x64-windows\debug\bin;D:\vcpkg\installed\x64-windows\bin;$env:PATH"
$env:OTEL_EXPORTER_OTLP_ENDPOINT = "http://127.0.0.1:4318"
$env:OTEL_EXPORTER_OTLP_TRACES_ENDPOINT = "http://127.0.0.1:4318/v1/traces"
$env:OTEL_EXPORTER_OTLP_METRICS_ENDPOINT = "http://127.0.0.1:4318/v1/metrics"
$env:HELLO_SERVICE_STORAGE_MODE = $StorageMode
$env:HELLO_SERVICE_MYSQL_HOST = $MySqlHost
$env:HELLO_SERVICE_MYSQL_PORT = "$MySqlPort"
$env:HELLO_SERVICE_MYSQL_USER = $MySqlUser
$env:HELLO_SERVICE_MYSQL_PASSWORD = $MySqlPassword
$env:HELLO_SERVICE_MYSQL_DATABASE = $MySqlDatabase
$env:HELLO_SERVICE_REDIS_HOST = $RedisHost
$env:HELLO_SERVICE_REDIS_PORT = "$RedisPort"
$env:HELLO_SERVICE_REDIS_PASSWORD = $RedisPassword
$env:HELLO_SERVICE_REDIS_DB = "$RedisDb"
$env:HELLO_SERVICE_REDIS_POOL_SIZE = "$RedisPoolSize"
if ($RedisSentinelMasterName) {
    $env:HELLO_SERVICE_REDIS_SENTINEL_MASTER_NAME = $RedisSentinelMasterName
}
if ($RedisSentinels) {
    $env:HELLO_SERVICE_REDIS_SENTINELS = $RedisSentinels
}
$env:HELLO_SERVICE_SERIALIZE_MUTATING_REQUESTS = if ($SerializeMutatingRequests) { "true" } else { "false" }
if ($PasswordHashThreads -gt 0) {
    $env:HELLO_SERVICE_PASSWORD_HASH_THREADS = "$PasswordHashThreads"
}
if ($PasswordHashMaxPending -gt 0) {
    $env:HELLO_SERVICE_PASSWORD_HASH_MAX_PENDING = "$PasswordHashMaxPending"
}
if ($PasswordVerifyCacheTtlMs -ge 0) {
    $env:HELLO_SERVICE_PASSWORD_VERIFY_CACHE_TTL_MS = "$PasswordVerifyCacheTtlMs"
}
if ($BusinessThreads -gt 0) {
    $env:HELLO_SERVICE_BUSINESS_THREADS = "$BusinessThreads"
}
if ($AcceptConcurrency -gt 0) {
    $env:HELLO_SERVICE_ACCEPT_CONCURRENCY = "$AcceptConcurrency"
}
if ($RegisterMaxPerIp -gt 0) {
    $env:HELLO_SERVICE_REGISTER_MAX_PER_IP = "$RegisterMaxPerIp"
}
if ($RegisterMaxPerLoginIp -gt 0) {
    $env:HELLO_SERVICE_REGISTER_MAX_PER_LOGIN_IP = "$RegisterMaxPerLoginIp"
}
if ($PerfMode) {
    $env:HELLO_SERVICE_PERF_MODE = "1"
}
$env:HELLO_SERVICE_INSECURE_FAST_PASSWORD_HASH = if ($InsecureFastPasswordHash) { "1" } else { "0" }
$env:HELLO_SERVICE_IN_MEMORY_SESSIONS = if ($InMemorySessions) { "1" } else { "0" }
$env:HELLO_SERVICE_IN_MEMORY_AUTH_SECURITY = if ($InMemoryAuthSecurity) { "1" } else { "0" }

$started = @()
for ($index = 0; $index -lt $Instances; $index++) {
    $port = $BasePort + $index
    $stdout = Join-Path $LogDirectory "hello_service.cluster.$port.log"
    $stderr = Join-Path $LogDirectory "hello_service.cluster.$port.stderr.log"

    Remove-Item -LiteralPath $stdout -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $stderr -Force -ErrorAction SilentlyContinue

    $env:HELLO_SERVICE_PORT = "$port"
    $process = Start-Process -FilePath $ServiceExePath `
        -WorkingDirectory $ProjectRoot `
        -RedirectStandardOutput $stdout `
        -RedirectStandardError $stderr `
        -WindowStyle Hidden `
        -PassThru

    # schema migration 和 Redis 首次连接可能超过 900ms，不能只靠固定 sleep 判断启动成功。
    # 轮询健康接口，避免服务仍在初始化时就开始压测或被误判为启动失败。
    $ready = $false
    for ($attempt = 0; $attempt -lt 60; $attempt++) {
        if ($process.HasExited) {
            break
        }
        try {
            if ((Invoke-WebRequest -UseBasicParsing -Uri "http://127.0.0.1:$port/health" -TimeoutSec 1).StatusCode -eq 200) {
                $ready = $true
                break
            }
        } catch {
            # 服务仍在绑定端口、连接数据库或执行 migration，继续等待。
        }
        Start-Sleep -Milliseconds 250
    }
    if (-not $ready) {
        Write-Host "instance port=$port is not ready, see $stderr"
        Get-Content -LiteralPath $stderr -ErrorAction SilentlyContinue
        Get-Content -LiteralPath $stdout -Tail 40 -ErrorAction SilentlyContinue
        throw "hello_service 集群启动失败：port=$port"
    }

    $started += [pscustomobject]@{
        Instance = $index
        Port = $port
        Pid = $process.Id
        Log = $stdout
    }
}

Write-Host "hello_service 集群已启动：$Instances 个实例，基端口 $BasePort"
$started | Format-Table -AutoSize
Write-Host "在 Nginx/负载均衡器中将 / 按 least_conn 或 ip_hash 分流到以下地址："
$started | ForEach-Object { Write-Host "  http://127.0.0.1:$($_.Port)" }
