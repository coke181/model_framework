[CmdletBinding()]
param(
    [string]$BaseUrl = "http://127.0.0.1:8080",
    [string]$LoginName = "alice",
    [string]$DisplayName = "Alice",
    [string]$Password = $(if ($env:HELLO_SERVICE_SMOKE_PASSWORD) { $env:HELLO_SERVICE_SMOKE_PASSWORD } else { 'change_me' })
)

# 文件职责：
# 对 hello_service 做最小 HTTP 冒烟验证，确认真实存储模式至少能完成登录和读资料。
# 调用流程：
# 服务启动后运行本脚本 ->
# 读取 /config ->
# 调用 /auth/register 与 /auth/login 验证正式账号链路 ->
# 再验证 /auth/change-password、/auth/refresh、/auth/sessions ->
# 再调用 /auth/quick-login 验证开发快捷入口 ->
# 最后请求 /players/me 和 /players 验证主链路。

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$config = Invoke-RestMethod -Method Get -Uri "$BaseUrl/config"
Write-Host "storage_mode = $($config.data.storage_mode)"
Write-Host "mysql = $($config.data.mysql_host):$($config.data.mysql_port)"
Write-Host "redis = $($config.data.redis_host):$($config.data.redis_port)"

$registerBody = @{
    login_name = $LoginName
    display_name = $DisplayName
    password = $Password
} | ConvertTo-Json

$register = $null
try {
    $register = Invoke-RestMethod -Method Post -Uri "$BaseUrl/auth/register" -ContentType "application/json" -Body $registerBody
    Write-Host "register created account_id = $($register.data.account.account_id)"
} catch {
    Write-Host "register skipped, account may already exist"
}

$loginBody = @{
    login_name = $LoginName
    password = $Password
} | ConvertTo-Json

$login = Invoke-RestMethod -Method Post -Uri "$BaseUrl/auth/login" -ContentType "application/json" -Body $loginBody
$token = $login.data.session.token
$refreshToken = $login.data.session.refresh_token

if (-not $token) {
    throw "登录结果里没有拿到 token。"
}

$headers = @{
    Authorization = "Bearer $token"
}

$changeBody = @{
    current_password = $Password
    new_password = "${Password}_next"
} | ConvertTo-Json

$changed = Invoke-RestMethod -Method Post -Uri "$BaseUrl/auth/change-password" -Headers $headers -ContentType "application/json" -Body $changeBody
$refreshed = Invoke-RestMethod -Method Post -Uri "$BaseUrl/auth/refresh" -ContentType "application/json" -Body (@{
    refresh_token = $refreshToken
} | ConvertTo-Json)
$refreshedHeaders = @{
    Authorization = "Bearer $($refreshed.data.token)"
}
$sessions = Invoke-RestMethod -Method Get -Uri "$BaseUrl/auth/sessions" -Headers $refreshedHeaders
$restore = Invoke-RestMethod -Method Post -Uri "$BaseUrl/auth/change-password" -Headers $refreshedHeaders -ContentType "application/json" -Body (@{
    current_password = "${Password}_next"
    new_password = $Password
} | ConvertTo-Json)

$quickBody = @{
    login_name = "${LoginName}_quick"
    display_name = "${DisplayName}Quick"
} | ConvertTo-Json

$quickLogin = Invoke-RestMethod -Method Post -Uri "$BaseUrl/auth/quick-login" -ContentType "application/json" -Body $quickBody
$me = Invoke-RestMethod -Method Get -Uri "$BaseUrl/players/me" -Headers $refreshedHeaders
$players = Invoke-RestMethod -Method Get -Uri "$BaseUrl/players"

Write-Host "login account_id = $($login.data.account.account_id)"
Write-Host "login player_id = $($login.data.player.player_id)"
Write-Host "changed account status = $($changed.data.status)"
Write-Host "refreshed session_id = $($refreshed.data.session_id)"
Write-Host "session count = $($sessions.data.Count)"
Write-Host "restored account status = $($restore.data.status)"
Write-Host "quick login account_id = $($quickLogin.data.account.account_id)"
Write-Host "players/me nickname = $($me.data.nickname)"
Write-Host "players total = $($players.data.Count)"
