[CmdletBinding()]
param(
    [string]$MySqlServerPath = "D:\mysql\bin\mysqld.exe",
    [string]$MySqlAdminPath = "D:\mysql\bin\mysqladmin.exe",
    [string]$DefaultsFile = "D:\mysql\my.ini",
    [string]$MySqlHost = "127.0.0.1",
    [int]$Port = 3306
)

# 文件职责：
# 启动本地 MySQL 开发实例，确保 hello_service 依赖的关系库可用。
# 调用流程：
# run-hello-mysql-redis.ps1 或开发者手动调用本脚本 ->
# 检查 mysqld/mysqladmin 和 my.ini ->
# 如未运行则后台启动 MySQL ->
# 用 mysqladmin ping 验证实例已经 ready。

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Test-MySqlRunning {
    param(
        [int]$MyPort
    )

    try {
        $process = Get-Process mysqld -ErrorAction Stop
        $listener = Get-NetTCPConnection -LocalPort $MyPort -State Listen -ErrorAction Stop
        return ($null -ne $process -and $null -ne $listener)
    } catch {
        return $false
    }
}

if (-not (Test-Path -LiteralPath $MySqlServerPath)) {
    throw "未找到 mysqld.exe：$MySqlServerPath"
}

if (-not (Test-Path -LiteralPath $MySqlAdminPath)) {
    throw "未找到 mysqladmin.exe：$MySqlAdminPath"
}

if (-not (Test-Path -LiteralPath $DefaultsFile)) {
    throw "未找到 MySQL 配置文件：$DefaultsFile"
}

if (Test-MySqlRunning -MyPort $Port) {
    Write-Host "MySQL 已经在运行，地址：$MySqlHost`:$Port"
    exit 0
}

Start-Process -FilePath $MySqlServerPath -ArgumentList "--defaults-file=$DefaultsFile" -WindowStyle Hidden | Out-Null
Start-Sleep -Seconds 5

if (-not (Test-MySqlRunning -MyPort $Port)) {
    throw "MySQL 启动失败，请检查 D:\\mysql\\logs\\error.log"
}

Write-Host "MySQL 已启动，地址：$MySqlHost`:$Port"
