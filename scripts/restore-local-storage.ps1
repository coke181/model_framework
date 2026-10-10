[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)]
    [string]$BackupDirectory,
    [string]$MySqlCliPath = "D:\mysql\bin\mysql.exe",
    [string]$MySqlHost = "127.0.0.1",
    [int]$MySqlPort = 3306,
    [string]$MySqlUser = "game_backend",
    [string]$MySqlPassword = $(if ($env:HELLO_SERVICE_MYSQL_PASSWORD) { $env:HELLO_SERVICE_MYSQL_PASSWORD } else { 'change_me_mysql_password' }),
    [string]$MySqlDatabase = "game_backend",
    [string]$RedisDataFile = "D:\redis\data\dump.rdb",
    [int]$RedisPort = 6379,
    [switch]$RecreateDatabase,
    [switch]$RestoreRedis,
    [switch]$ConfirmRestore
)

# 文件职责：从 backup-local-storage.ps1 生成的目录恢复 MySQL，并可选恢复 Redis RDB。
# 安全要求：必须显式传 -ConfirmRestore；恢复 Redis 前必须停止 Redis 和 hello_service。

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if (-not $ConfirmRestore) { throw "恢复会覆盖现有数据，必须显式传入 -ConfirmRestore" }
$manifestPath = Join-Path $BackupDirectory 'manifest.json'
if (-not (Test-Path -LiteralPath $manifestPath)) { throw "缺少 manifest.json：$manifestPath" }
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
foreach ($file in $manifest.files) {
    $path = Join-Path $BackupDirectory $file.name
    if (-not (Test-Path -LiteralPath $path)) { throw "备份文件缺失：$path" }
    $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
    if ($hash -ne $file.sha256) { throw "备份文件校验失败：$path" }
}

$mysqlBackup = Join-Path $BackupDirectory "$MySqlDatabase.sql"
if (-not (Test-Path -LiteralPath $MySqlCliPath)) { throw "未找到 mysql.exe：$MySqlCliPath" }
$oldMySqlPassword = $env:MYSQL_PWD
$env:MYSQL_PWD = $MySqlPassword
try {
    if ($RecreateDatabase) {
        & $MySqlCliPath --host=$MySqlHost --port=$MySqlPort --user=$MySqlUser `
            -e "DROP DATABASE IF EXISTS $MySqlDatabase; CREATE DATABASE $MySqlDatabase CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;"
        if ($LASTEXITCODE -ne 0) { throw "重建 MySQL 数据库失败" }
    }
    Get-Content -LiteralPath $mysqlBackup -ReadCount 1000 | & $MySqlCliPath `
        --host=$MySqlHost --port=$MySqlPort --user=$MySqlUser $MySqlDatabase
    if ($LASTEXITCODE -ne 0) { throw "恢复 MySQL 失败，exit_code=$LASTEXITCODE" }
} finally {
    $env:MYSQL_PWD = $oldMySqlPassword
}

if ($RestoreRedis) {
    if (Get-Process hello_service -ErrorAction SilentlyContinue) {
        throw "恢复 Redis 前必须停止 hello_service"
    }
    if (Get-NetTCPConnection -State Listen -LocalPort $RedisPort -ErrorAction SilentlyContinue) {
        throw "恢复 Redis 前必须停止 Redis 端口 $RedisPort"
    }
    $redisBackup = Join-Path $BackupDirectory 'dump.rdb'
    if (-not (Test-Path -LiteralPath $redisBackup)) { throw "缺少 Redis 备份：$redisBackup" }
    Copy-Item -LiteralPath $redisBackup -Destination $RedisDataFile -Force
}

Write-Host "恢复完成。恢复 Redis 后请重新启动 Redis，再启动 hello_service。"
