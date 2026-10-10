[CmdletBinding()]
param(
    [string]$ServiceExePath = "D:\code\model\build\Debug\hello_service.exe",
    [string]$CollectorLogPath = "D:\otelcol\collector.log",
    [string]$BaseUrl = "http://127.0.0.1:8080"
)

# 文件职责：
# 启动 hello_service，触发几条典型 HTTP 请求，再检查 collector 日志里是否出现 trace 和 metrics。
# 调用流程：
# 调用脚本 ->
# 设置 OTLP 导出环境变量和 DLL PATH ->
# 启动示例服务 ->
# 请求 /health、/metrics、/debug/traces ->
# 回看 collector.log，确认 collector 已收到导出的 traces/metrics。

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if (-not (Test-Path -LiteralPath $ServiceExePath)) {
    throw "未找到服务程序：$ServiceExePath。请先执行 cmake --build build --config Debug"
}

$env:PATH = "D:\OpenSSL-Win64\bin;D:\vcpkg\installed\x64-windows\debug\bin;D:\vcpkg\installed\x64-windows\bin;$env:PATH"
$env:OTEL_EXPORTER_OTLP_ENDPOINT = "http://127.0.0.1:4318"
$env:OTEL_EXPORTER_OTLP_TRACES_ENDPOINT = "http://127.0.0.1:4318/v1/traces"
$env:OTEL_EXPORTER_OTLP_METRICS_ENDPOINT = "http://127.0.0.1:4318/v1/metrics"

$service = Start-Process -FilePath $ServiceExePath -WindowStyle Hidden -PassThru
try {
    Start-Sleep -Seconds 5

    $health = Invoke-WebRequest -UseBasicParsing "$BaseUrl/health"
    $metrics = Invoke-WebRequest -UseBasicParsing "$BaseUrl/metrics"
    $traces = Invoke-WebRequest -UseBasicParsing "$BaseUrl/debug/traces"

    Write-Host "/health -> $($health.StatusCode)"
    Write-Host "/metrics -> $($metrics.StatusCode)"
    Write-Host "/debug/traces -> $($traces.StatusCode)"

    # trace 和 metrics 都要等 collector 的批处理/周期导出真正落日志，等待太短容易误判失败。
    Start-Sleep -Seconds 12

    if (-not (Test-Path -LiteralPath $CollectorLogPath)) {
        throw "未找到 collector 日志：$CollectorLogPath。请先启动 collector。"
    }

    $logText = Get-Content -LiteralPath $CollectorLogPath -Raw
    $hasTrace = $logText -match "ScopeSpans|ResourceSpans|trace_id"
    $hasMetric = $logText -match "ResourceMetrics|ScopeMetrics|mf_http_requests_total|mf_http_request_duration_ms"

    Write-Host "collector traces: $hasTrace"
    Write-Host "collector metrics: $hasMetric"

    if (-not $hasTrace -or -not $hasMetric) {
        throw "collector 日志中尚未看到完整 trace/metrics 导出，请检查 $CollectorLogPath"
    }
}
finally {
    if ($service -and -not $service.HasExited) {
        Stop-Process -Id $service.Id -Force
    }
}
