$ErrorActionPreference = "Stop"

# 文件职责：确保 D 盘上的本地 LiveKit 已启动；重复执行不会创建第二个进程。
$executable = "D:\tools\livekit\server\livekit-server.exe"
$config = "D:\tools\livekit\config\livekit.yaml"
$stdoutLog = "D:\tools\livekit\logs\livekit.stdout.log"
$stderrLog = "D:\tools\livekit\logs\livekit.stderr.log"

if (-not (Test-Path -LiteralPath $executable)) {
    throw "未找到 LiveKit：$executable"
}
if (-not (Test-Path -LiteralPath $config)) {
    throw "未找到 LiveKit 配置：$config"
}

$listening = Get-NetTCPConnection -LocalPort 7880 -State Listen -ErrorAction SilentlyContinue
if ($listening) {
    Write-Output "LiveKit 已在 127.0.0.1:7880 运行"
    exit 0
}

$process = Start-Process `
    -FilePath $executable `
    -ArgumentList @("--config", $config) `
    -WorkingDirectory "D:\tools\livekit" `
    -WindowStyle Hidden `
    -RedirectStandardOutput $stdoutLog `
    -RedirectStandardError $stderrLog `
    -PassThru

for ($attempt = 0; $attempt -lt 20; ++$attempt) {
    Start-Sleep -Milliseconds 250
    if ($process.HasExited) {
        throw "LiveKit 启动失败，请查看 $stderrLog"
    }
    if (Get-NetTCPConnection -LocalPort 7880 -State Listen -ErrorAction SilentlyContinue) {
        Write-Output "LiveKit 已启动，PID=$($process.Id)，地址=ws://127.0.0.1:7880"
        exit 0
    }
}

Stop-Process -Id $process.Id -Force
throw "LiveKit 在等待时间内没有监听 7880，请查看 $stderrLog"
