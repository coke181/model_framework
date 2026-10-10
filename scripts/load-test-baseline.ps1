[CmdletBinding()]
param(
    [ValidateSet("auto", "wrk", "bombardier")]
    [string]$Tool = "auto",
    [string]$BaseUrl = "http://127.0.0.1:8080"
)

# 文件职责：
# 跑一组偏保守的基线压测，先看当前服务在正常并发下的吞吐与时延。
# 调用流程：
# 调用脚本 ->
# 复用 load-test-http.ps1 ->
# 以中等连接数打 /health ->
# 压测完成后直接查看关键 metrics。

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

& (Join-Path $PSScriptRoot "load-test-http.ps1") `
    -Tool $Tool `
    -BaseUrl $BaseUrl `
    -Path "/health" `
    -Connections 64 `
    -Threads 8 `
    -DurationSeconds 20 `
    -Method "GET"
