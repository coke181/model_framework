# 文件职责：停止由本地开发脚本启动的 LiveKit，不影响其他服务。
$connections = Get-NetTCPConnection -LocalPort 7880 -State Listen -ErrorAction SilentlyContinue
if (-not $connections) {
    Write-Output "LiveKit 当前未运行"
    exit 0
}

$processIds = $connections | Select-Object -ExpandProperty OwningProcess -Unique
foreach ($processId in $processIds) {
    $process = Get-Process -Id $processId -ErrorAction SilentlyContinue
    if ($process -and $process.Path -eq "D:\tools\livekit\server\livekit-server.exe") {
        Stop-Process -Id $processId -Force
        Write-Output "已停止 LiveKit，PID=$processId"
    } else {
        throw "7880 端口不是 D 盘 LiveKit 占用，拒绝停止 PID=$processId"
    }
}
