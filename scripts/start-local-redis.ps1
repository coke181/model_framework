[CmdletBinding()]
param(
    [string]$RedisServerPath = "D:\redis\redis-server.exe",
    [string]$RedisCliPath = "D:\redis\redis-cli.exe",
    [string]$RedisDataDir = "D:\redis\data",
    [string]$RedisLogPath = "D:\redis\redis.log",
    [int]$Port = 6379,
    [string]$Bind = "127.0.0.1",
    [string]$MaxMemory = "2gb",
    [ValidateSet("noeviction", "volatile-lru", "volatile-lfu", "volatile-random", "volatile-ttl", "allkeys-lru", "allkeys-lfu", "allkeys-random")]
    [string]$MaxMemoryPolicy = "volatile-ttl",
    [switch]$DisablePersistence
)

# 文件职责：
# 启动本地 Redis，给 hello_service 的 SessionRepository 提供真实 Redis 支撑。
# 调用流程：
# run-hello-mysql-redis.ps1 或开发者手动调用本脚本 ->
# 检查 redis-server/redis-cli 是否存在 ->
# 如未运行则后台启动 Redis ->
# 用 redis-cli ping 验证是否可用。

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Test-RedisRunning {
    param(
        [string]$CliPath,
        [int]$RedisPort
    )

    if (-not (Test-Path -LiteralPath $CliPath)) {
        return $false
    }

    try {
        $pong = & $CliPath -p $RedisPort PING 2>$null
        return $pong -contains "PONG"
    } catch {
        return $false
    }
}

if (-not (Test-Path -LiteralPath $RedisServerPath)) {
    throw "未找到 redis-server.exe：$RedisServerPath"
}

if (-not (Test-Path -LiteralPath $RedisCliPath)) {
    throw "未找到 redis-cli.exe：$RedisCliPath"
}

New-Item -ItemType Directory -Force -Path $RedisDataDir | Out-Null

if (Test-RedisRunning -CliPath $RedisCliPath -RedisPort $Port) {
    if ($MaxMemory) {
        & $RedisCliPath -p $Port CONFIG SET maxmemory $MaxMemory | Out-Null
        & $RedisCliPath -p $Port CONFIG SET maxmemory-policy $MaxMemoryPolicy | Out-Null
    }
    Write-Host "Redis 已经在运行，端口：$Port"
    exit 0
}

$arguments = @(
    "--port", $Port,
    "--bind", $Bind,
    "--dir", $RedisDataDir,
    "--dbfilename", "dump.rdb",
    "--logfile", $RedisLogPath,
    "--maxmemory", $MaxMemory,
    "--maxmemory-policy", $MaxMemoryPolicy
)

if ($DisablePersistence) {
    # 性能压测使用独立 Redis 时关闭 RDB/AOF，避免快照暂停污染吞吐和 P99。
    # Start-Process 会拒绝真正的空字符串参数，因此传入一对引号，让 Redis 收到空 save 规则。
    $arguments += @("--save", '""', "--appendonly", "no")
}

Start-Process -FilePath $RedisServerPath -ArgumentList $arguments -WindowStyle Hidden | Out-Null
Start-Sleep -Seconds 1

if (-not (Test-RedisRunning -CliPath $RedisCliPath -RedisPort $Port)) {
    throw "Redis 启动失败，请检查日志：$RedisLogPath"
}

Write-Host "Redis 已启动，端口：$Port"
