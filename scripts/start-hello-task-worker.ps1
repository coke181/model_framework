[CmdletBinding()]
param(
    [string]$ProjectRoot = "D:\code\model",
    [string]$WorkerExePath = "D:\code\model\build\Debug\hello_task_worker.exe",
    [string]$StorageMode = "mysql_redis",
    [string]$LogPath = "D:\code\model\build\hello_task_worker.log",
    [string]$MySqlHost = "127.0.0.1",
    [int]$MySqlPort = 3306,
    [string]$MySqlUser = "game_backend",
    [string]$MySqlPassword = "GameBackend_2026!",
    [string]$MySqlDatabase = "game_backend",
    [string]$RedisHost = "127.0.0.1",
    [int]$RedisPort = 6379,
    [string]$RedisPassword = "",
    [int]$RedisDb = 0,
    [int]$ChatStateShardCount = 64,
    [switch]$StopExisting
)

# 文件职责：
# 以后台常驻方式启动 hello_task_worker，并统一补齐存储与 OTel 运行环境。
# 调用流程：
# 调用脚本 ->
# 可选清理旧 worker 进程 ->
# 设置 PATH / OTEL / 存储环境变量 ->
# 后台拉起任务 worker，并把 stdout/stderr 落盘。

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Stop-ExistingTaskWorker {
    param([string]$ResolvedExePath)

    $normalizedExe = [System.IO.Path]::GetFullPath($ResolvedExePath)
    $running = Get-CimInstance Win32_Process -Filter "Name = 'hello_task_worker.exe'" |
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

if (-not (Test-Path -LiteralPath $WorkerExePath)) {
    throw "未找到任务 worker：$WorkerExePath。请先执行 cmake --build build --config Debug --target hello_task_worker"
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
    Stop-ExistingTaskWorker -ResolvedExePath $WorkerExePath
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
$env:HELLO_CHAT_STATE_SHARD_COUNT = "$ChatStateShardCount"

$process = Start-Process -FilePath $WorkerExePath `
    -WorkingDirectory $ProjectRoot `
    -RedirectStandardOutput $LogPath `
    -RedirectStandardError $errorLogPath `
    -WindowStyle Hidden `
    -PassThru

Start-Sleep -Seconds 4
if ($process.HasExited) {
    Write-Host "hello_task_worker 已退出，日志如下："
    if (Test-Path -LiteralPath $LogPath) {
        Get-Content -LiteralPath $LogPath
    }
    if (Test-Path -LiteralPath $errorLogPath) {
        Get-Content -LiteralPath $errorLogPath
    }
    throw "hello_task_worker 启动失败。"
}

Write-Host "hello_task_worker 已启动"
Write-Host "PID: $($process.Id)"
Write-Host "EXE: $WorkerExePath"
Write-Host "STORAGE: $StorageMode"
Write-Host "LOG: $LogPath"
Write-Host "ERR: $errorLogPath"

exit 0
