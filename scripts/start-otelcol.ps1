[CmdletBinding()]
param(
    [string]$CollectorRoot = "D:\otelcol",
    [string]$CollectorExePath = "",
    [string]$ConfigPath = "",
    [string]$LogPath = "",
    [switch]$ForceRewriteConfig,
    [switch]$StopExisting
)

# 文件职责：
# 生成一个最小可用的本地 OpenTelemetry Collector 配置，并以前台/后台方式启动 collector。
# 调用流程：
# 调用脚本 ->
# 定位 collector 可执行文件 ->
# 生成 OTLP receiver + debug exporter 配置 ->
# 后台启动 collector，并把日志落到 D:\otelcol 方便联调检查。

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Resolve-CollectorExePath {
    param([string]$ExplicitPath, [string]$Root)

    if ($ExplicitPath -and (Test-Path -LiteralPath $ExplicitPath)) {
        return $ExplicitPath
    }

    $candidates = @(
        (Join-Path $Root "otelcol-contrib.exe"),
        (Join-Path $Root "otelcol.exe"),
        (Join-Path $Root "install\otelcol-contrib.exe"),
        (Join-Path $Root "install\otelcol.exe"),
        (Join-Path $Root "extract\OpenTelemetry Collector\otelcol-contrib.exe"),
        (Join-Path $Root "extract\OpenTelemetry Collector\otelcol.exe")
    )

    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate) {
            return $candidate
        }
    }

    return $null
}

function Stop-ExistingCollectorProcess {
    param([string]$ResolvedExePath)

    $normalizedExe = [System.IO.Path]::GetFullPath($ResolvedExePath)
    $running = Get-CimInstance Win32_Process -Filter "Name = 'otelcol-contrib.exe' OR Name = 'otelcol.exe'" |
        Where-Object {
            $_.ExecutablePath -and
            [System.StringComparer]::OrdinalIgnoreCase.Equals(
                [System.IO.Path]::GetFullPath($_.ExecutablePath),
                $normalizedExe)
        }

    foreach ($process in $running) {
        Stop-Process -Id $process.ProcessId -Force
    }
}

New-Item -ItemType Directory -Force -Path $CollectorRoot | Out-Null

if (-not $ConfigPath) {
    $ConfigPath = Join-Path $CollectorRoot "config.yaml"
}
if (-not $LogPath) {
    $LogPath = Join-Path $CollectorRoot "collector.log"
}
$errorLogPath = [System.IO.Path]::ChangeExtension($LogPath, ".stderr.log")

$resolvedExe = Resolve-CollectorExePath -ExplicitPath $CollectorExePath -Root $CollectorRoot
if (-not $resolvedExe) {
    throw "未找到 collector 可执行文件。请先把 GitHub 下载的 otelcol 解压到 $CollectorRoot，或者通过 -CollectorExePath 显式指定。"
}

if ($StopExisting) {
    # 后台常驻和手动调试可能都会调用本脚本，先清理同一 exe 的旧实例，避免端口占用。
    Stop-ExistingCollectorProcess -ResolvedExePath $resolvedExe
}

if ($ForceRewriteConfig -or -not (Test-Path -LiteralPath $ConfigPath)) {
    @'
receivers:
  otlp:
    protocols:
      grpc:
        endpoint: 127.0.0.1:4317
      http:
        endpoint: 127.0.0.1:4318

processors:
  batch:

exporters:
  debug:
    verbosity: detailed

service:
  pipelines:
    traces:
      receivers: [otlp]
      processors: [batch]
      exporters: [debug]
    metrics:
      receivers: [otlp]
      processors: [batch]
      exporters: [debug]
'@ | Set-Content -LiteralPath $ConfigPath -Encoding UTF8
}

if (Test-Path -LiteralPath $LogPath) {
    try {
        Remove-Item -LiteralPath $LogPath -Force
    } catch {
        Start-Sleep -Seconds 1
        Remove-Item -LiteralPath $LogPath -Force -ErrorAction SilentlyContinue
    }
}
if (Test-Path -LiteralPath $errorLogPath) {
    try {
        Remove-Item -LiteralPath $errorLogPath -Force
    } catch {
        Start-Sleep -Seconds 1
        Remove-Item -LiteralPath $errorLogPath -Force -ErrorAction SilentlyContinue
    }
}

for ($attempt = 1; $attempt -le 4; ++$attempt) {
    $process = Start-Process -FilePath $resolvedExe `
        -ArgumentList @("--config", $ConfigPath) `
        -RedirectStandardOutput $LogPath `
        -RedirectStandardError $errorLogPath `
        -WindowStyle Hidden `
        -PassThru

    Start-Sleep -Seconds 3
    if (-not $process.HasExited) {
        Write-Host "collector 已启动"
        Write-Host "PID: $($process.Id)"
        Write-Host "EXE: $resolvedExe"
        Write-Host "CONFIG: $ConfigPath"
        Write-Host "LOG: $LogPath"
        Write-Host "ERR: $errorLogPath"

        exit 0
    }

    # 计划任务重启旧实例时，端口可能还在 TIME_WAIT，给一次短暂缓冲后自动重试。
    Start-Sleep -Seconds 3
}

Write-Host "collector 已退出，日志如下："
if (Test-Path -LiteralPath $LogPath) {
    Get-Content -LiteralPath $LogPath
}
if (Test-Path -LiteralPath $errorLogPath) {
    Get-Content -LiteralPath $errorLogPath
}
throw "collector 启动失败。"
