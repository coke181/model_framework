[CmdletBinding()]
param(
    [ValidateSet("auto", "wrk", "bombardier")]
    [string]$Tool = "auto",
    [string]$BaseUrl = "http://127.0.0.1:8080",
    [string]$Path = "/health",
    [int]$Connections = 128,
    [int]$Threads = 8,
    [int]$DurationSeconds = 20,
    [string]$Method = "GET",
    [int]$Rate = 0,
    [string]$MetricsBaseUrl = "",
    [switch]$SkipMetrics
)

# 文件职责：
# 用 wrk 或 bombardier 对指定 HTTP 接口做压测，并在结束后直接拉取 /metrics 看吞吐、慢请求和拒绝点。
# 调用流程：
# 调用脚本 ->
# 自动探测 wrk / bombardier ->
# 执行压测 ->
# 抓取 /metrics 中与 HTTP 吞吐、慢请求、429/503 拒绝相关的关键指标。

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Resolve-LoadTool {
    param([string]$Preferred)

    if ($Preferred -eq "wrk") {
        $wrk = Get-Command wrk -ErrorAction SilentlyContinue
        if (-not $wrk) { throw "未找到 wrk，可改用 -Tool bombardier 或先安装 wrk。" }
        return @{ Name = "wrk"; Path = $wrk.Source }
    }

    if ($Preferred -eq "bombardier") {
        $bombardier = Get-Command bombardier -ErrorAction SilentlyContinue
        if ($bombardier) {
            return @{ Name = "bombardier"; Path = $bombardier.Source }
        }
        $localBombardier = "D:\bombardier\bombardier.exe"
        if (Test-Path -LiteralPath $localBombardier) {
            return @{ Name = "bombardier"; Path = $localBombardier }
        }
        throw "未找到 bombardier，可改用 -Tool wrk 或先安装 bombardier。"
    }

    $bombardier = Get-Command bombardier -ErrorAction SilentlyContinue
    if ($bombardier) {
        return @{ Name = "bombardier"; Path = $bombardier.Source }
    }

    # 本项目约定第三方工具放在 D 盘；未加入 PATH 时也自动识别常用安装位置。
    $localBombardier = "D:\bombardier\bombardier.exe"
    if (Test-Path -LiteralPath $localBombardier) {
        return @{ Name = "bombardier"; Path = $localBombardier }
    }

    $wrk = Get-Command wrk -ErrorAction SilentlyContinue
    if ($wrk) {
        return @{ Name = "wrk"; Path = $wrk.Source }
    }

    throw "既没有找到 bombardier，也没有找到 wrk。请先安装其中一个压测工具。"
}

function Show-MetricSummary {
    param([string]$MetricsUrl)

    $content = (Invoke-WebRequest -UseBasicParsing $MetricsUrl).Content
    $lines = $content -split "`r?`n"
    $interesting = $lines | Where-Object {
        $_ -match '^mf_http_requests_total' -or
        $_ -match '^mf_http_request_duration_ms_(bucket|sum|count)' -or
        $_ -match '^mf_http_rejected_requests_total' -or
        $_ -match '^mf_http_slow_requests_total' -or
        $_ -match '^mf_alert_events_total'
    }

    Write-Host ""
    Write-Host "==== Metrics Summary ===="
    if ($interesting) {
        $interesting | ForEach-Object { Write-Host $_ }
    } else {
        Write-Host "未匹配到关键指标行。"
    }
}

$resolved = Resolve-LoadTool -Preferred $Tool
$url = "$BaseUrl$Path"

Write-Host "tool=$($resolved.Name)"
Write-Host "url=$url"
Write-Host "connections=$Connections threads=$Threads duration=${DurationSeconds}s method=$Method rate=$Rate"

if ($resolved.Name -eq "bombardier") {
    $arguments = @("-c", "$Connections", "-d", "${DurationSeconds}s", "-m", $Method, "-l")
    if ($Rate -gt 0) {
        $arguments += @("-r", "$Rate")
    }
    $arguments += $url
    & $resolved.Path @arguments
} else {
    $arguments = @("-t$Threads", "-c$Connections", "-d${DurationSeconds}s", "--latency")
    & $resolved.Path @arguments $url
}

if (-not $SkipMetrics) {
    $resolvedMetricsBaseUrl = if ($MetricsBaseUrl) { $MetricsBaseUrl.TrimEnd('/') } else { $BaseUrl.TrimEnd('/') }
    Show-MetricSummary -MetricsUrl "$resolvedMetricsBaseUrl/metrics"
}
