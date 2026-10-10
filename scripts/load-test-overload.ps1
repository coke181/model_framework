[CmdletBinding()]
param(
    [ValidateSet("auto", "wrk", "bombardier")]
    [string]$Tool = "auto",
    [string]$BaseUrl = "http://127.0.0.1:8080"
)

# 文件职责：
# 跑一组更激进的压测，故意逼近或打穿当前背压阈值，观察 429/503 与慢请求增长点。
# 调用流程：
# 调用脚本 ->
# 复用 load-test-http.ps1 ->
# 提高连接数和压测时长 ->
# 结束后直接查看 rejected / slow 指标。

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

& (Join-Path $PSScriptRoot "load-test-http.ps1") `
    -Tool $Tool `
    -BaseUrl $BaseUrl `
    -Path "/health" `
    -Connections 512 `
    -Threads 16 `
    -DurationSeconds 30 `
    -Method "GET"
