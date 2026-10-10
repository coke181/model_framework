param(
    [string]$ServiceExecutable = "D:\code\model\build-otel-off\Debug\hello_service.exe",
    [int]$Port = 18081,
    [string]$LiveKitUrl = "ws://127.0.0.1:7880",
    [string]$LiveKitApiKey = "devkey",
    [string]$LiveKitApiSecret = "devsecret"
)

# 文件职责：验证语音成员登记、LiveKit JWT Claim、服务端禁言和封禁。
$env:PATH = "D:\OpenSSL-Win64\bin;D:\vcpkg\installed\x64-windows\debug\bin;D:\vcpkg\installed\x64-windows\bin;$env:PATH"
$env:HELLO_SERVICE_PORT = [string]$Port
$env:HELLO_SERVICE_HOST = "127.0.0.1"
$env:HELLO_SERVICE_STORAGE_MODE = "in_memory"
$env:HELLO_SERVICE_ENV = "development"
$env:HELLO_SERVICE_AUTO_BOOTSTRAP_DEV_ADMIN = "1"
$env:HELLO_SERVICE_LIVEKIT_URL = $LiveKitUrl
$env:HELLO_SERVICE_LIVEKIT_API_KEY = $LiveKitApiKey
$env:HELLO_SERVICE_LIVEKIT_API_SECRET = $LiveKitApiSecret
$env:HELLO_SERVICE_VOICE_TOKEN_TTL_SECONDS = "900"
$env:HELLO_SERVICE_VOICE_ROOM_PREFIX = "blaster"

function ConvertFrom-Base64Url([string]$Value) {
    $normalized = $Value.Replace('-', '+').Replace('_', '/')
    while (($normalized.Length % 4) -ne 0) { $normalized += '=' }
    return [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($normalized)) | ConvertFrom-Json
}

function ConvertTo-Base64Url([byte[]]$Value) {
    return [Convert]::ToBase64String($Value).TrimEnd('=').Replace('+', '-').Replace('/', '_')
}

$process = Start-Process `
    -FilePath $ServiceExecutable `
    -WorkingDirectory (Split-Path -Parent $PSScriptRoot) `
    -WindowStyle Hidden `
    -PassThru

try {
    $ready = $false
    for ($index = 0; $index -lt 20; ++$index) {
        try {
            Invoke-RestMethod -Uri "http://127.0.0.1:$Port/health" -TimeoutSec 1 | Out-Null
            $ready = $true
            break
        } catch { Start-Sleep -Milliseconds 250 }
    }
    if (-not $ready) { throw "测试服务未能启动" }

    $adminPassword = if ($env:HELLO_SERVICE_DEV_ADMIN_PASSWORD) { $env:HELLO_SERVICE_DEV_ADMIN_PASSWORD } else { 'change_me_admin_password' }
    $admin = Invoke-RestMethod -Method Post -Uri "http://127.0.0.1:$Port/admin/login" `
        -ContentType "application/json" `
        -Body (@{ login_name = "admin_demo"; password = $adminPassword } | ConvertTo-Json)
    $headers = @{ Authorization = "Bearer $($admin.data.session.token)" }
    $playerId = $admin.data.player.player_id

    $memberBody = @{
        scope_type = "session"
        scope_id = "smoke_session_001"
        player_id = $playerId
        role = "leader"
    } | ConvertTo-Json
    $member = Invoke-RestMethod -Method Post -Uri "http://127.0.0.1:$Port/admin/voice/members/upsert" `
        -Headers $headers -ContentType "application/json" -Body $memberBody

    $scopeBody = @{ scopeType = "session"; scopeId = "smoke_session_001" } | ConvertTo-Json
    $firstToken = Invoke-RestMethod -Method Post -Uri "http://127.0.0.1:$Port/api/voice/token" `
        -Headers $headers -ContentType "application/json" -Body $scopeBody
    $tokenParts = $firstToken.data.access_token -split '\.'
    $firstPayload = ConvertFrom-Base64Url $tokenParts[1]
    $hmac = [Security.Cryptography.HMACSHA256]::new([Text.Encoding]::UTF8.GetBytes($LiveKitApiSecret))
    try {
        $expectedSignature = ConvertTo-Base64Url ($hmac.ComputeHash([Text.Encoding]::UTF8.GetBytes("$($tokenParts[0]).$($tokenParts[1])")))
    } finally {
        $hmac.Dispose()
    }

    $muteBody = @{
        scope_type = "session"
        scope_id = "smoke_session_001"
        target_player_id = $playerId
        action = "mute"
        reason = "smoke test"
    } | ConvertTo-Json
    $muted = Invoke-RestMethod -Method Post -Uri "http://127.0.0.1:$Port/admin/voice/moderation" `
        -Headers $headers -ContentType "application/json" -Body $muteBody
    $mutedToken = Invoke-RestMethod -Method Post -Uri "http://127.0.0.1:$Port/api/voice/token" `
        -Headers $headers -ContentType "application/json" -Body $scopeBody
    $mutedPayload = ConvertFrom-Base64Url (($mutedToken.data.access_token -split '\.')[1])

    $banBody = @{
        scope_type = "session"
        scope_id = "smoke_session_001"
        target_player_id = $playerId
        action = "ban"
        reason = "smoke test"
    } | ConvertTo-Json
    Invoke-RestMethod -Method Post -Uri "http://127.0.0.1:$Port/admin/voice/moderation" `
        -Headers $headers -ContentType "application/json" -Body $banBody | Out-Null
    $banRejected = $false
    try {
        Invoke-RestMethod -Method Post -Uri "http://127.0.0.1:$Port/api/voice/token" `
            -Headers $headers -ContentType "application/json" -Body $scopeBody | Out-Null
    } catch {
        $banRejected = $_.Exception.Response.StatusCode.value__ -eq 403
    }

    [pscustomobject]@{
        member_registered = $member.ok
        issuer = $firstPayload.iss
        participant = $firstPayload.sub
        room = $firstPayload.video.room
        signature_valid = $expectedSignature -eq $tokenParts[2]
        initial_can_publish = $firstPayload.video.canPublish
        muted_flag = $muted.data.server_muted
        muted_can_publish = $mutedPayload.video.canPublish
        banned_token_rejected = $banRejected
    }
} finally {
    if ($process -and -not $process.HasExited) { Stop-Process -Id $process.Id -Force }
}
