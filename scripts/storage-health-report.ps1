[CmdletBinding()]
param(
    [string]$BaseUrl = "http://127.0.0.1:8080",
    [string]$RedisCliPath = "D:\redis\redis-cli.exe",
    [string]$RedisHost = "127.0.0.1",
    [int]$RedisPort = 6379,
    [string]$MySqlCliPath = "D:\mysql\bin\mysql.exe",
    [string]$MySqlHost = "127.0.0.1",
    [int]$MySqlPort = 3306,
    [string]$MySqlUser = "game_backend",
    [string]$MySqlPassword = "GameBackend_2026!",
    [string]$MySqlDatabase = "game_backend",
    [string]$OutputPath = ""
)

# 文件职责：
# 汇总应用 metrics、Redis 内存/持久化和 MySQL 慢查询状态，给出可直接处理的告警。
# 调用流程：读取 /metrics -> 查询 Redis INFO -> 查询 MySQL 状态 -> 输出 JSON 报告。

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Get-MapValueOrDefault {
    param($Map, [string]$Key, $Default)
    if ($Map.ContainsKey($Key)) { return $Map[$Key] }
    return $Default
}

if (-not $OutputPath) {
    $timestamp = Get-Date -Format "yyyyMMdd_HHmmss"
    $OutputPath = "D:\code\model\build\storage-health-$timestamp.json"
}

$warnings = [System.Collections.Generic.List[string]]::new()
$metricsText = (Invoke-WebRequest -UseBasicParsing "$($BaseUrl.TrimEnd('/'))/metrics" -TimeoutSec 10).Content
$metricLines = @($metricsText -split "`r?`n" | Where-Object {
    $_ -match '^mf_(mysql|redis|http_slow|http_rejected|alert)_'
})

if (-not (Test-Path -LiteralPath $RedisCliPath)) {
    throw "未找到 redis-cli：$RedisCliPath"
}
$redisInfo = & $RedisCliPath -h $RedisHost -p $RedisPort INFO
$redisValues = @{}
foreach ($line in $redisInfo) {
    if ($line -match '^([^#][^:]+):(.*)$') {
        $redisValues[$matches[1]] = $matches[2].Trim()
    }
}
$redisKeys = 0L
if ($redisValues.ContainsKey('db0') -and $redisValues['db0'] -match 'keys=(\d+)') {
    $redisKeys = [int64]$matches[1]
}
$redisMemory = [int64](Get-MapValueOrDefault $redisValues 'used_memory' '0')
$redisMaxMemory = [int64](Get-MapValueOrDefault $redisValues 'maxmemory' '0')
if ($redisKeys -gt 1000000) { $warnings.Add("Redis key 数超过 100 万：$redisKeys") }
if ($redisMaxMemory -eq 0) { $warnings.Add("Redis 未设置 maxmemory，异常写入可能占满主机内存") }
if ((Get-MapValueOrDefault $redisValues 'maxmemory_policy' '') -eq 'noeviction') {
    $warnings.Add("Redis 淘汰策略为 noeviction，达到内存上限后写命令会失败")
}

if (-not (Test-Path -LiteralPath $MySqlCliPath)) {
    throw "未找到 mysql.exe：$MySqlCliPath"
}
$oldMySqlPassword = $env:MYSQL_PWD
$env:MYSQL_PWD = $MySqlPassword
try {
    $mysqlRows = & $MySqlCliPath `
        --host=$MySqlHost --port=$MySqlPort --user=$MySqlUser `
        --database=$MySqlDatabase --batch --raw --skip-column-names `
        -e "SELECT 'slow_query_log', @@GLOBAL.slow_query_log UNION ALL SELECT 'long_query_time', @@GLOBAL.long_query_time UNION ALL SELECT 'max_connections', @@GLOBAL.max_connections UNION ALL SELECT VARIABLE_NAME, VARIABLE_VALUE FROM performance_schema.global_status WHERE VARIABLE_NAME IN ('Threads_connected','Threads_running','Slow_queries');"
} finally {
    $env:MYSQL_PWD = $oldMySqlPassword
}
$mysqlValues = @{}
foreach ($row in $mysqlRows) {
    $columns = $row -split "`t", 2
    if ($columns.Count -eq 2) { $mysqlValues[$columns[0]] = $columns[1] }
}
if ((Get-MapValueOrDefault $mysqlValues 'slow_query_log' '0') -ne '1') {
    $warnings.Add("MySQL slow_query_log 未开启；应用指标仍可观测仓储耗时，但无法保留原生慢 SQL 文件")
}
if ([double](Get-MapValueOrDefault $mysqlValues 'long_query_time' '10') -gt 1.0) {
    $warnings.Add("MySQL long_query_time 大于 1 秒，短时慢查询不容易被原生日志捕获")
}

$report = [ordered]@{
    generated_at = (Get-Date).ToString('o')
    application = [ordered]@{
        base_url = $BaseUrl
        storage_metrics = $metricLines
    }
    redis = [ordered]@{
        host = $RedisHost
        port = $RedisPort
        keys = $redisKeys
        used_memory_bytes = $redisMemory
        used_memory_human = $redisValues['used_memory_human']
        maxmemory_bytes = $redisMaxMemory
        maxmemory_policy = $redisValues['maxmemory_policy']
        rdb_last_bgsave_status = $redisValues['rdb_last_bgsave_status']
        aof_enabled = $redisValues['aof_enabled']
    }
    mysql = $mysqlValues
    warnings = @($warnings)
}

$directory = Split-Path -Parent $OutputPath
if ($directory) { New-Item -ItemType Directory -Force -Path $directory | Out-Null }
$report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $OutputPath -Encoding UTF8
$report | ConvertTo-Json -Depth 8
Write-Host "报告已写入：$OutputPath"
