[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)]
    [string]$EnvFile
)

# 文件职责：部署前静态检查 deploy.env，阻止性能模式、弱默认密码和开发管理员进入生产。
# 调用流程：读取 KEY=VALUE -> 校验生产安全基线 -> 有错误时返回非零退出码。

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if (-not (Test-Path -LiteralPath $EnvFile)) { throw "环境文件不存在：$EnvFile" }
$values = @{}
foreach ($rawLine in Get-Content -LiteralPath $EnvFile) {
    $line = $rawLine.Trim()
    if (-not $line -or $line.StartsWith('#') -or -not $line.Contains('=')) { continue }
    $parts = $line.Split('=', 2)
    $values[$parts[0].Trim()] = $parts[1].Trim()
}

function Value-OrEmpty([string]$Name) {
    if ($values.ContainsKey($Name)) { return [string]$values[$Name] }
    return ""
}

function Is-Enabled([string]$Name) {
    return (Value-OrEmpty $Name).ToLowerInvariant() -in @('1', 'true', 'on', 'yes')
}

$errors = [System.Collections.Generic.List[string]]::new()
$warnings = [System.Collections.Generic.List[string]]::new()

if ((Value-OrEmpty 'HELLO_SERVICE_ENV') -ne 'production') { $errors.Add('HELLO_SERVICE_ENV 必须为 production') }
if ((Value-OrEmpty 'HELLO_SERVICE_STORAGE_MODE') -ne 'mysql_redis') { $errors.Add('生产必须使用 mysql_redis') }
if (Is-Enabled 'HELLO_SERVICE_PERF_MODE') { $errors.Add('生产禁止 HELLO_SERVICE_PERF_MODE') }
if (Is-Enabled 'HELLO_SERVICE_INSECURE_FAST_PASSWORD_HASH') { $errors.Add('生产禁止快速密码哈希') }
if (Is-Enabled 'HELLO_SERVICE_AUTO_BOOTSTRAP_DEV_ADMIN') { $errors.Add('生产禁止自动创建开发管理员') }

$mysqlPassword = Value-OrEmpty 'HELLO_SERVICE_MYSQL_PASSWORD'
if ($mysqlPassword.Length -lt 12 -or $mysqlPassword -match '^(change_me|GameBackend_123456|GameBackend_2026!)$') {
    $errors.Add('MySQL 密码为空、过短或仍是示例默认值')
}
$gmToken = Value-OrEmpty 'HELLO_SERVICE_GM_TOKEN'
if ($gmToken.Length -lt 24 -or $gmToken -match '^change_me') {
    $errors.Add('GM token 为空、过短或仍是示例默认值')
}

$argonMemory = Value-OrEmpty 'HELLO_SERVICE_PASSWORD_ARGON2_MEMORY_KIB'
$argonTime = Value-OrEmpty 'HELLO_SERVICE_PASSWORD_ARGON2_TIME_COST'
if ($argonMemory -and [int]$argonMemory -lt 19456) { $errors.Add('Argon2 memory 必须至少 19456 KiB') }
if ($argonTime -and [int]$argonTime -lt 2) { $errors.Add('Argon2 time_cost 必须至少为 2') }

if (-not (Value-OrEmpty 'HELLO_SERVICE_REDIS_PASSWORD')) {
    $warnings.Add('Redis 没有密码；仅当它严格绑定 127.0.0.1/内网并由防火墙隔离时可接受')
}
if ((Value-OrEmpty 'HELLO_SERVICE_REQUEST_LOG_SAMPLE_RATE') -eq '1') {
    $warnings.Add('生产请求日志未采样，高吞吐时日志 I/O 可能较高')
}

Write-Host "errors=$($errors.Count) warnings=$($warnings.Count)"
$errors | ForEach-Object { Write-Host "ERROR: $_" -ForegroundColor Red }
$warnings | ForEach-Object { Write-Host "WARN: $_" -ForegroundColor Yellow }
if ($errors.Count -gt 0) { exit 1 }
Write-Host "生产配置检查通过：$EnvFile"
