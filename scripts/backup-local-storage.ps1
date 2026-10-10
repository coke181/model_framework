[CmdletBinding()]
param(
    [string]$BackupRoot = "D:\code\model\backups",
    [string]$MySqlDumpPath = "D:\mysql\bin\mysqldump.exe",
    [string]$MySqlHost = "127.0.0.1",
    [int]$MySqlPort = 3306,
    [string]$MySqlUser = "game_backend",
    [string]$MySqlPassword = $(if ($env:HELLO_SERVICE_MYSQL_PASSWORD) { $env:HELLO_SERVICE_MYSQL_PASSWORD } else { 'change_me_mysql_password' }),
    [string]$MySqlDatabase = "game_backend",
    [string]$RedisCliPath = "D:\redis\redis-cli.exe",
    [string]$RedisHost = "127.0.0.1",
    [int]$RedisPort = 6379
)

# 文件职责：生成同一时间点附近的 MySQL 逻辑备份和 Redis RDB 副本，并写 SHA256 清单。
# 调用流程：mysqldump -> Redis BGSAVE -> 复制 dump.rdb -> 生成 manifest.json。

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if (-not (Test-Path -LiteralPath $MySqlDumpPath)) { throw "未找到 mysqldump：$MySqlDumpPath" }
if (-not (Test-Path -LiteralPath $RedisCliPath)) { throw "未找到 redis-cli：$RedisCliPath" }

$timestamp = Get-Date -Format "yyyyMMdd_HHmmss"
$target = Join-Path $BackupRoot $timestamp
New-Item -ItemType Directory -Force -Path $target | Out-Null
$mysqlOutput = Join-Path $target "$MySqlDatabase.sql"

$oldMySqlPassword = $env:MYSQL_PWD
$env:MYSQL_PWD = $MySqlPassword
try {
    & $MySqlDumpPath `
        --host=$MySqlHost --port=$MySqlPort --user=$MySqlUser `
        --single-transaction --quick --routines --events --triggers `
        --set-gtid-purged=OFF --no-tablespaces --result-file=$mysqlOutput $MySqlDatabase
    if ($LASTEXITCODE -ne 0) { throw "mysqldump 失败，exit_code=$LASTEXITCODE" }
} finally {
    $env:MYSQL_PWD = $oldMySqlPassword
}

& $RedisCliPath -h $RedisHost -p $RedisPort BGSAVE | Out-Null
$deadline = (Get-Date).AddMinutes(5)
do {
    Start-Sleep -Seconds 1
    $persistence = & $RedisCliPath -h $RedisHost -p $RedisPort INFO persistence
    $runningLine = $persistence | Where-Object { $_ -match '^rdb_bgsave_in_progress:' } | Select-Object -First 1
    $statusLine = $persistence | Where-Object { $_ -match '^rdb_last_bgsave_status:' } | Select-Object -First 1
    $running = [int](($runningLine -split ':')[1].Trim())
    $status = (($statusLine -split ':')[1].Trim())
    if ((Get-Date) -ge $deadline) { throw "等待 Redis BGSAVE 超时" }
} while ($running -eq 1)
if ($status -ne 'ok') { throw "Redis BGSAVE 失败：$status" }

$config = @(& $RedisCliPath -h $RedisHost -p $RedisPort --raw CONFIG GET dir dbfilename)
$configValues = @{}
for ($index = 0; $index + 1 -lt $config.Count; $index += 2) {
    $configValues[$config[$index]] = $config[$index + 1]
}
$redisDir = $configValues['dir']
$redisFile = $configValues['dbfilename']
if (-not $redisDir -or -not $redisFile) { throw "读取 Redis dir/dbfilename 失败" }
$redisSource = Join-Path $redisDir $redisFile
$redisOutput = Join-Path $target $redisFile
Copy-Item -LiteralPath $redisSource -Destination $redisOutput -Force

$files = @($mysqlOutput, $redisOutput) | ForEach-Object {
    $item = Get-Item -LiteralPath $_
    [ordered]@{
        name = $item.Name
        length = $item.Length
        sha256 = (Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).Hash
    }
}
$manifest = [ordered]@{
    created_at = (Get-Date).ToString('o')
    mysql_database = $MySqlDatabase
    redis_host = $RedisHost
    redis_port = $RedisPort
    files = $files
}
$manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $target 'manifest.json') -Encoding UTF8
Write-Host "备份完成：$target"
$manifest | ConvertTo-Json -Depth 6
