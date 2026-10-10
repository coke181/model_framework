[CmdletBinding()]
param(
    [string]$RedisServerPath = "D:\redis\redis-server.exe",
    [string]$RedisSentinelPath = "D:\redis\redis-sentinel.exe",
    [string]$RedisCliPath = "D:\redis\redis-cli.exe",
    [string]$HaRoot = "D:\code\model\.redis-ha",
    [int]$MasterPort = 6380,
    [int]$ReplicaPort = 6381,
    [int]$SentinelStartPort = 26379,
    [int]$SentinelCount = 3,
    [string]$MasterName = "mymaster",
    [int]$Quorum = 2,
    [switch]$StopExisting,
    [switch]$Stop
)

# 文件职责：
# 在本地拉起 Redis 一主一从 + N 个哨兵，用于验证 hello_service 的主从 + Sentinel 高可用模式。
# 调用流程：
# 开发者调用本脚本 -> 可选清理上次 HA 进程 ->
# 在 $HaRoot 下生成 master/replica/sentinel 配置 ->
# 依次后台拉起 master、replica、sentinel -> 用 redis-cli 验证拓扑。

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if (-not (Test-Path -LiteralPath $RedisServerPath)) {
    throw "未找到 redis-server.exe：$RedisServerPath"
}
if (-not (Test-Path -LiteralPath $RedisSentinelPath)) {
    throw "未找到 redis-sentinel.exe：$RedisSentinelPath"
}
if (-not (Test-Path -LiteralPath $RedisCliPath)) {
    throw "未找到 redis-cli.exe：$RedisCliPath"
}

$runDirectory = Join-Path $HaRoot "run"
$dataDirectory = Join-Path $HaRoot "data"
$logDirectory = Join-Path $HaRoot "logs"

function Stop-HaProcesses {
    if (-not (Test-Path -LiteralPath $runDirectory)) {
        return
    }
    Get-ChildItem -LiteralPath $runDirectory -Filter "*.pid" | ForEach-Object {
        $pidText = (Get-Content -LiteralPath $_.FullName -Raw).Trim()
        $processId = 0
        if ([int]::TryParse($pidText, [ref]$processId) -and $processId -gt 0) {
            if (Get-Process -Id $processId -ErrorAction SilentlyContinue) {
                Stop-Process -Id $processId -Force
                Write-Host "已停止 HA 进程 PID：$processId"
            }
        }
    }
}

function Write-RedisConfig {
    param(
        [string]$Path,
        [string]$Content
    )
    Set-Content -LiteralPath $Path -Value $Content -Encoding ascii
}

function Start-RedisNode {
    param(
        [string]$BinaryPath,
        [string]$ConfigName,
        [string]$PidFileName,
        [string]$Label
    )

    $process = Start-Process -FilePath $BinaryPath `
        -ArgumentList $ConfigName `
        -WorkingDirectory $HaRoot `
        -WindowStyle Hidden `
        -PassThru
    Set-Content -LiteralPath (Join-Path $runDirectory $PidFileName) -Value $process.Id -Encoding ascii
    Write-Host "$Label 已启动，PID：$($process.Id)"
}

if ($Stop) {
    Stop-HaProcesses
    Write-Host "Redis HA 已停止。"
    exit 0
}

if ($StopExisting) {
    Stop-HaProcesses
    Start-Sleep -Seconds 1
}

foreach ($directory in @($runDirectory, $dataDirectory, $logDirectory)) {
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
}

New-Item -ItemType Directory -Force -Path (Join-Path $dataDirectory "master") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $dataDirectory "replica") | Out-Null
1..$SentinelCount | ForEach-Object {
    New-Item -ItemType Directory -Force -Path (Join-Path $dataDirectory "sentinel$_") | Out-Null
}

# Redis 的 logfile 相对路径是按 dir 指令解析的，而不是 CWD；这里统一写绝对正斜杠路径避免歧义。
$haRootForward = $HaRoot.Replace("\", "/")

# 注意：本机 MSYS 版 Redis 会把传入的绝对 Windows 配置文件路径错误转换，
# 所以配置文件本身以相对文件名传入；dir/logfile 则写绝对正斜杠路径。
$masterConfig = @"
port $MasterPort
bind 127.0.0.1
protected-mode no
dir $haRootForward/data/master
appendonly yes
appendfilename appendonly.aof
save ""
logfile $haRootForward/logs/master.log
"@
Write-RedisConfig -Path (Join-Path $HaRoot "master.conf") -Content $masterConfig

$replicaConfig = @"
port $ReplicaPort
bind 127.0.0.1
protected-mode no
dir $haRootForward/data/replica
appendonly yes
appendfilename appendonly.aof
save ""
replicaof 127.0.0.1 $MasterPort
logfile $haRootForward/logs/replica.log
"@
Write-RedisConfig -Path (Join-Path $HaRoot "replica.conf") -Content $replicaConfig

1..$SentinelCount | ForEach-Object {
    $index = $_
    $sentinelPort = $SentinelStartPort + $index - 1
    $sentinelConfig = @"
port $sentinelPort
bind 127.0.0.1
protected-mode no
dir $haRootForward/data/sentinel$index
logfile $haRootForward/logs/sentinel$index.log
sentinel monitor $MasterName 127.0.0.1 $MasterPort $Quorum
sentinel down-after-milliseconds $MasterName 1000
sentinel failover-timeout $MasterName 10000
sentinel parallel-syncs $MasterName 1
"@
    Write-RedisConfig -Path (Join-Path $HaRoot "sentinel$index.conf") -Content $sentinelConfig
}

Start-RedisNode -BinaryPath $RedisServerPath -ConfigName "master.conf" -PidFileName "master.pid" -Label "master"
Start-Sleep -Seconds 1
Start-RedisNode -BinaryPath $RedisServerPath -ConfigName "replica.conf" -PidFileName "replica.pid" -Label "replica"
Start-Sleep -Seconds 1
1..$SentinelCount | ForEach-Object {
    Start-RedisNode -BinaryPath $RedisSentinelPath -ConfigName "sentinel$_.conf" -PidFileName "sentinel$_.pid" -Label "sentinel$_"
}

Start-Sleep -Seconds 3

$masterPong = (& $RedisCliPath -p $MasterPort PING 2>$null) -contains "PONG"
$replicaRole = (& $RedisCliPath -p $ReplicaPort ROLE 2>$null) -contains "slave"
$sentinelMaster = & $RedisCliPath -p $SentinelStartPort SENTINEL MASTER $MasterName 2>$null

$sentinelMasterIp = ""
$sentinelMasterPort = ""
if ($sentinelMaster) {
    for ($index = 0; $index -lt ($sentinelMaster.Count - 1); $index++) {
        if ($sentinelMaster[$index] -eq "ip") {
            $sentinelMasterIp = $sentinelMaster[$index + 1]
        } elseif ($sentinelMaster[$index] -eq "port") {
            $sentinelMasterPort = $sentinelMaster[$index + 1]
        }
    }
}

if (-not $masterPong) {
    throw "master 未就绪，请检查日志：$logDirectory\master.log"
}

Write-Host ""
Write-Host "Redis HA 已启动：master=$MasterPort replica=$ReplicaPort sentinel=$SentinelStartPort..$($SentinelStartPort + $SentinelCount - 1)"
Write-Host "master 状态：$(if ($masterPong) { 'PONG' } else { 'FAILED' })"
Write-Host "replica 角色：$(if ($replicaRole) { 'slave' } else { 'CHECK' })"
if ($sentinelMasterIp -and $sentinelMasterPort) {
    Write-Host "哨兵上报 master：$sentinelMasterIp`:$sentinelMasterPort"
} else {
    Write-Host "哨兵尚未上报 master，请稍后重试 SENTINEL MASTER $MasterName"
}
Write-Host ""
Write-Host "服务侧使用以下环境变量接入："
$sentinels = (0..($SentinelCount - 1) | ForEach-Object { "127.0.0.1:$($SentinelStartPort + $_)" }) -join ","
Write-Host "  HELLO_SERVICE_REDIS_SENTINELS=$sentinels"
Write-Host "  HELLO_SERVICE_REDIS_SENTINEL_MASTER_NAME=$MasterName"
Write-Host ""
Write-Host "如需停止：.\scripts\start-local-redis-ha.ps1 -Stop"
Write-Host "如需重启：.\scripts\start-local-redis-ha.ps1 -StopExisting"
