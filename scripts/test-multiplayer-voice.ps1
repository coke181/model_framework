param(
    [string]$ServiceExecutable = "D:\code\model\build\Debug\hello_service.exe",
    [int]$Port = 18083
)

# 文件职责：验证权威多人房间与语音成员的自动同步和越权拒绝。
$ErrorActionPreference = "Stop"
$env:PATH = "D:\OpenSSL-Win64\bin;D:\vcpkg\installed\x64-windows\debug\bin;D:\vcpkg\installed\x64-windows\bin;$env:PATH"
$env:HELLO_SERVICE_PORT = [string]$Port
$env:HELLO_SERVICE_HOST = "127.0.0.1"
$env:HELLO_SERVICE_STORAGE_MODE = "in_memory"
$env:HELLO_SERVICE_ENV = "development"
$env:HELLO_SERVICE_LIVEKIT_URL = "ws://127.0.0.1:7880"
$env:HELLO_SERVICE_LIVEKIT_API_KEY = "multiplayer-smoke-key"
$env:HELLO_SERVICE_LIVEKIT_API_SECRET = "multiplayer-smoke-secret"
$env:HELLO_SERVICE_VOICE_ROOM_PREFIX = "blaster"

function Invoke-JsonPost([string]$Path, [object]$Body, [string]$Token = "") {
    $headers = @{}
    if ($Token) { $headers.Authorization = "Bearer $Token" }
    return Invoke-RestMethod -Method Post -Uri "http://127.0.0.1:$Port$Path" `
        -Headers $headers -ContentType "application/json" -Body ($Body | ConvertTo-Json -Depth 8)
}

function Invoke-ExpectedError([string]$Path, [object]$Body, [string]$Token) {
    try {
        Invoke-JsonPost $Path $Body $Token | Out-Null
        return [pscustomobject]@{ status = 200; code = "unexpected_success" }
    } catch {
        $status = $_.Exception.Response.StatusCode.value__
        $payload = $_.ErrorDetails.Message | ConvertFrom-Json
        return [pscustomobject]@{ status = $status; code = $payload.error.code }
    }
}

function QuickLogin([string]$LoginName) {
    return Invoke-JsonPost "/auth/quick-login" @{
        login_name = $LoginName
        display_name = $LoginName
    }
}

$stdoutLog = Join-Path $PSScriptRoot "multiplayer-smoke.stdout.log"
$stderrLog = Join-Path $PSScriptRoot "multiplayer-smoke.stderr.log"
$process = Start-Process -FilePath $ServiceExecutable `
    -WorkingDirectory (Split-Path -Parent $PSScriptRoot) `
    -WindowStyle Hidden -RedirectStandardOutput $stdoutLog -RedirectStandardError $stderrLog -PassThru

try {
    $ready = $false
    for ($index = 0; $index -lt 40; ++$index) {
        try {
            Invoke-RestMethod -Uri "http://127.0.0.1:$Port/health" -TimeoutSec 1 | Out-Null
            $ready = $true
            break
        } catch { Start-Sleep -Milliseconds 250 }
    }
    if (-not $ready) { throw "测试服务未能启动，请查看 $stderrLog" }

    $owner = QuickLogin "mp_owner"
    $member = QuickLogin "mp_member"
    $outsider = QuickLogin "mp_outsider"
    $ownerToken = $owner.data.session.token
    $memberToken = $member.data.session.token
    $outsiderToken = $outsider.data.session.token

    $created = Invoke-JsonPost "/api/multiplayer/rooms/create" @{
        room_name = "联动测试房间"
        max_players = 2
    } $ownerToken
    $roomId = $created.data.room_id
    $voiceBody = @{ scopeType = "session"; scopeId = $roomId }

    $ownerVoice = Invoke-JsonPost "/api/voice/token" $voiceBody $ownerToken
    $memberBeforeJoin = Invoke-ExpectedError "/api/voice/token" $voiceBody $memberToken
    $joined = Invoke-JsonPost "/api/multiplayer/rooms/join" @{ room_id = $roomId } $memberToken
    $memberVoice = Invoke-JsonPost "/api/voice/token" $voiceBody $memberToken
    $roomFull = Invoke-ExpectedError "/api/multiplayer/rooms/join" @{ room_id = $roomId } $outsiderToken
    $outsiderVoice = Invoke-ExpectedError "/api/voice/token" $voiceBody $outsiderToken
    $outsiderGet = Invoke-ExpectedError "/api/multiplayer/rooms/get" @{ room_id = $roomId } $outsiderToken
    $ownerLeave = Invoke-ExpectedError "/api/multiplayer/rooms/leave" @{ room_id = $roomId } $ownerToken
    $memberClose = Invoke-ExpectedError "/api/multiplayer/rooms/close" @{ room_id = $roomId } $memberToken

    $left = Invoke-JsonPost "/api/multiplayer/rooms/leave" @{ room_id = $roomId } $memberToken
    $memberAfterLeave = Invoke-ExpectedError "/api/voice/token" $voiceBody $memberToken
    Invoke-JsonPost "/api/multiplayer/rooms/join" @{ room_id = $roomId } $memberToken | Out-Null
    $closed = Invoke-JsonPost "/api/multiplayer/rooms/close" @{ room_id = $roomId } $ownerToken
    $ownerAfterClose = Invoke-ExpectedError "/api/voice/token" $voiceBody $ownerToken
    $memberAfterClose = Invoke-ExpectedError "/api/voice/token" $voiceBody $memberToken

    [pscustomobject]@{
        room_created = $created.ok
        room_id = $roomId
        owner_auto_voice_member = $ownerVoice.ok
        member_before_join_code = $memberBeforeJoin.code
        joined_member_count = $joined.data.member_count
        member_voice_after_join = $memberVoice.ok
        room_full_code = $roomFull.code
        outsider_voice_code = $outsiderVoice.code
        outsider_get_code = $outsiderGet.code
        owner_leave_code = $ownerLeave.code
        member_close_code = $memberClose.code
        member_count_after_leave = $left.data.member_count
        member_after_leave_code = $memberAfterLeave.code
        closed_status = $closed.data.status
        closed_member_count = $closed.data.member_count
        owner_after_close_code = $ownerAfterClose.code
        member_after_close_code = $memberAfterClose.code
    }
} finally {
    if ($process -and -not $process.HasExited) { Stop-Process -Id $process.Id -Force }
}
