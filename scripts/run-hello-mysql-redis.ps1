[CmdletBinding()]
param(
    [string]$ServiceExePath = "D:\code\model\build\Debug\hello_service.exe",
    [string]$SchemaPath = "D:\code\model\db\mysql\001_game_backend.sql",
    [string]$MySqlStarterScript = "D:\code\model\scripts\start-local-mysql.ps1",
    [string]$RedisStarterScript = "D:\code\model\scripts\start-local-redis.ps1",
    [string]$MySqlHost = "127.0.0.1",
    [int]$MySqlPort = 3306,
    [string]$MySqlUser = "game_backend",
    [string]$MySqlPassword = "GameBackend_2026!",
    [string]$MySqlDatabase = "game_backend",
    [string]$MySqlCliPath = "",
    [string]$RedisHost = "127.0.0.1",
    [int]$RedisPort = 6379,
    [string]$RedisPassword = "",
    [int]$RedisDb = 0,
    [int]$RegisterMaxPerIp = 0,
    [int]$RegisterMaxPerLoginIp = 0,
    [int]$RequestLogSampleRate = 0,
    [switch]$PerfMode,
    [switch]$StartMySql,
    [switch]$StartRedis,
    [switch]$ApplySchema
)

# 文件职责：
# 用 mysql_redis 模式启动 hello_service，并把环境变量一次性配置好。
# 调用流程：
# 开发者传入 MySQL/Redis 参数 ->
# 可选启动本地 Redis、可选执行 schema ->
# 设置 HELLO_SERVICE_* 环境变量 ->
# 启动真实存储模式的示例服务。

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Resolve-MySqlCliPath {
    param([string]$ExplicitPath)

    if ($ExplicitPath -and (Test-Path -LiteralPath $ExplicitPath)) {
        return $ExplicitPath
    }

    $candidates = @(
        "D:\mysql\bin\mysql.exe",
        "D:\Program Files\MySQL\MySQL Server 8.0\bin\mysql.exe",
        "D:\Program Files\MySQL\MySQL Server 8.4\bin\mysql.exe"
    )

    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate) {
            return $candidate
        }
    }

    return $null
}

if (-not (Test-Path -LiteralPath $ServiceExePath)) {
    throw "未找到服务程序：$ServiceExePath。请先执行 cmake --build build --config Debug"
}

if ($StartMySql) {
    & $MySqlStarterScript -MySqlHost $MySqlHost -Port $MySqlPort
}

if ($StartRedis) {
    & $RedisStarterScript -Port $RedisPort
}

$resolvedMySqlCli = Resolve-MySqlCliPath -ExplicitPath $MySqlCliPath
if ($ApplySchema) {
    if (-not $resolvedMySqlCli) {
        throw "请求执行 schema，但没有找到 mysql.exe。请把 MySQL 装到 D: 或通过 -MySqlCliPath 指定。"
    }
    if (-not (Test-Path -LiteralPath $SchemaPath)) {
        throw "未找到 schema 文件：$SchemaPath"
    }

    # 先保证目标数据库存在，再把 schema 导进去。
    & $resolvedMySqlCli --host=$MySqlHost --port=$MySqlPort --user=$MySqlUser --password=$MySqlPassword -e "CREATE DATABASE IF NOT EXISTS $MySqlDatabase CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;"
    Get-Content -LiteralPath $SchemaPath | & $resolvedMySqlCli --host=$MySqlHost --port=$MySqlPort --user=$MySqlUser --password=$MySqlPassword $MySqlDatabase
}

$env:HELLO_SERVICE_STORAGE_MODE = "mysql_redis"
$env:HELLO_SERVICE_MYSQL_HOST = $MySqlHost
$env:HELLO_SERVICE_MYSQL_PORT = "$MySqlPort"
$env:HELLO_SERVICE_MYSQL_USER = $MySqlUser
$env:HELLO_SERVICE_MYSQL_PASSWORD = $MySqlPassword
$env:HELLO_SERVICE_MYSQL_DATABASE = $MySqlDatabase
$env:HELLO_SERVICE_REDIS_HOST = $RedisHost
$env:HELLO_SERVICE_REDIS_PORT = "$RedisPort"
$env:HELLO_SERVICE_REDIS_PASSWORD = $RedisPassword
$env:HELLO_SERVICE_REDIS_DB = "$RedisDb"
if ($RegisterMaxPerIp -gt 0) {
    $env:HELLO_SERVICE_REGISTER_MAX_PER_IP = "$RegisterMaxPerIp"
}
if ($RegisterMaxPerLoginIp -gt 0) {
    $env:HELLO_SERVICE_REGISTER_MAX_PER_LOGIN_IP = "$RegisterMaxPerLoginIp"
}
if ($RequestLogSampleRate -gt 0) {
    $env:HELLO_SERVICE_REQUEST_LOG_SAMPLE_RATE = "$RequestLogSampleRate"
}
if ($PerfMode) {
    $env:HELLO_SERVICE_PERF_MODE = "1"
}

Write-Host "HELLO_SERVICE_STORAGE_MODE=mysql_redis"
Write-Host "MySQL: $MySqlHost`:$MySqlPort / db=$MySqlDatabase / user=$MySqlUser"
Write-Host "Redis: $RedisHost`:$RedisPort / db=$RedisDb"
if (-not $resolvedMySqlCli) {
    Write-Warning "当前未检测到 mysql.exe。服务仍可启动，但前提是你的 MySQL 库和表已经建好。"
}

& $ServiceExePath
