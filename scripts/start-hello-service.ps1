[CmdletBinding()]
param(
    [string]$ProjectRoot = "D:\code\model",
    [string]$ServiceExePath = "D:\code\model\build\Debug\hello_service.exe",
    [string]$StorageMode = "mysql_redis",
    [string]$LogPath = "D:\code\model\build\hello_service.log",
    [string]$MySqlHost = "127.0.0.1",
    [int]$MySqlPort = 3306,
    [string]$MySqlUser = "game_backend",
    [string]$MySqlPassword = "GameBackend_2026!",
    [string]$MySqlDatabase = "game_backend",
    [string]$RedisHost = "127.0.0.1",
    [int]$RedisPort = 6379,
    [string]$RedisPassword = "",
    [int]$RedisDb = 0,
    [int]$RedisPoolSize = 16,
    [int]$AcceptConcurrency = 0,
    [string]$RedisSentinels = "",
    [string]$RedisSentinelMasterName = "",
    [bool]$SerializeMutatingRequests = $true,
    [int]$MaxPendingTasksPerKey = 256,
    [int]$ChatStateShardCount = 64,
    [switch]$StopExisting
)

# 文件职责：
# 以后台常驻方式启动 hello_service，并统一补齐运行时 DLL PATH 与 OTel 导出环境变量。
# 调用流程：
# 调用脚本 ->
# 可选清理旧 hello_service 进程 ->
# 设置 PATH / OTEL / 存储模式环境变量 ->
# 后台拉起服务并把 stdout/stderr 落盘，便于排查。

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Stop-ExistingHelloService {
    param([string]$ResolvedExePath)

    $normalizedExe = [System.IO.Path]::GetFullPath($ResolvedExePath)
    $running = Get-CimInstance Win32_Process -Filter "Name = 'hello_service.exe'" |
        Where-Object {
            $_.ExecutablePath -and
            [System.StringComparer]::OrdinalIgnoreCase.Equals(
                [System.IO.Path]::GetFullPath($_.ExecutablePath),
                $normalizedExe)
        }

    foreach ($process in $running) {
        Stop-Process -Id $process.ProcessId -Force
    }
}

if (-not (Test-Path -LiteralPath $ServiceExePath)) {
    throw "未找到服务程序：$ServiceExePath。请先执行 cmake --build build --config Debug"
}

if (-not (Test-Path -LiteralPath $ProjectRoot)) {
    throw "未找到项目目录：$ProjectRoot"
}

$logDirectory = Split-Path -Parent $LogPath
if ($logDirectory) {
    New-Item -ItemType Directory -Force -Path $logDirectory | Out-Null
}

$errorLogPath = [System.IO.Path]::ChangeExtension($LogPath, ".stderr.log")

if ($StopExisting) {
    # 登录自启和手动调试都可能重复调用，这里先清旧实例，避免 8080 端口冲突。
    Stop-ExistingHelloService -ResolvedExePath $ServiceExePath
}

if (Test-Path -LiteralPath $LogPath) {
    Remove-Item -LiteralPath $LogPath -Force
}
if (Test-Path -LiteralPath $errorLogPath) {
    Remove-Item -LiteralPath $errorLogPath -Force
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
if ($AcceptConcurrency -gt 0) {
    $env:HELLO_SERVICE_ACCEPT_CONCURRENCY = "$AcceptConcurrency"
}
if ($RedisSentinelMasterName) {
    $env:HELLO_SERVICE_REDIS_SENTINEL_MASTER_NAME = $RedisSentinelMasterName
}
if ($RedisSentinels) {
    $env:HELLO_SERVICE_REDIS_SENTINELS = $RedisSentinels
}
$env:HELLO_SERVICE_SERIALIZE_MUTATING_REQUESTS = if ($SerializeMutatingRequests) { "true" } else { "false" }
$env:HELLO_SERVICE_MAX_PENDING_TASKS_PER_KEY = "$MaxPendingTasksPerKey"
$env:HELLO_CHAT_STATE_SHARD_COUNT = "$ChatStateShardCount"

$process = Start-Process -FilePath $ServiceExePath `
    -WorkingDirectory $ProjectRoot `
    -RedirectStandardOutput $LogPath `
    -RedirectStandardError $errorLogPath `
    -WindowStyle Hidden `
    -PassThru

Start-Sleep -Seconds 4
if ($process.HasExited) {
    Write-Host "hello_service 已退出，日志如下："
    if (Test-Path -LiteralPath $LogPath) {
        Get-Content -LiteralPath $LogPath
    }
    if (Test-Path -LiteralPath $errorLogPath) {
        Get-Content -LiteralPath $errorLogPath
    }
    throw "hello_service 启动失败。"
}

Write-Host "hello_service 已启动"
Write-Host "PID: $($process.Id)"
Write-Host "EXE: $ServiceExePath"
Write-Host "STORAGE: $StorageMode"
Write-Host "LOG: $LogPath"
Write-Host "ERR: $errorLogPath"

exit 0
