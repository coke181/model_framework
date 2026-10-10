param(
    [string]$ServiceExecutable = "D:\code\model\build\Debug\hello_service.exe",
    [int]$Port = 18084
)

# 文件职责：验证 Agent 登录鉴权、降级回复、工具确认幂等、历史和玩家隔离。
$ErrorActionPreference = "Stop"
$env:PATH = "D:\OpenSSL-Win64\bin;D:\vcpkg\installed\x64-windows\debug\bin;D:\vcpkg\installed\x64-windows\bin;$env:PATH"
$env:HELLO_SERVICE_PORT = [string]$Port
$env:HELLO_SERVICE_HOST = "127.0.0.1"
$env:HELLO_SERVICE_STORAGE_MODE = "in_memory"
$env:HELLO_SERVICE_ENV = "development"
$env:HELLO_SERVICE_AGENT_ENABLED = "0"
$env:HELLO_SERVICE_AGENT_DAILY_REQUEST_LIMIT = "20"

function Invoke-JsonPost([string]$Path, [object]$Body, [string]$Token = "") {
    $headers = @{}
    if ($Token) { $headers.Authorization = "Bearer $Token" }
    # Windows PowerShell 5.1 可能把字符串请求体按系统代码页发送；显式转成 UTF-8，避免中文意图被破坏。
    $jsonBytes = [System.Text.Encoding]::UTF8.GetBytes(($Body | ConvertTo-Json -Depth 10))
    Invoke-RestMethod -Method Post -Uri "http://127.0.0.1:$Port$Path" `
        -Headers $headers -ContentType "application/json; charset=utf-8" -Body $jsonBytes
}

function Invoke-ExpectedError([string]$Path, [object]$Body, [string]$Token = "") {
    try {
        Invoke-JsonPost $Path $Body $Token | Out-Null
        return [pscustomobject]@{ status = 200; code = "unexpected_success" }
    } catch {
        $payload = $_.ErrorDetails.Message | ConvertFrom-Json
        return [pscustomobject]@{ status = $_.Exception.Response.StatusCode.value__; code = $payload.error.code }
    }
}

function QuickLogin([string]$Name) {
    Invoke-JsonPost "/auth/quick-login" @{ login_name = $Name; display_name = $Name }
}

$stdoutLog = Join-Path $PSScriptRoot "agent-smoke.stdout.log"
$stderrLog = Join-Path $PSScriptRoot "agent-smoke.stderr.log"
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

    $unauthorized = Invoke-ExpectedError "/api/agent/chat" @{
        client_request_id = "agent_no_auth_1"
        message = "你好"
        scene = "rpg"
    }

    $player = QuickLogin "agent_player"
    $other = QuickLogin "agent_other"
    $token = $player.data.session.token
    $otherToken = $other.data.session.token
    $requestId = "agent_request_open_inventory"
    $chatBody = @{
        client_request_id = $requestId
        message = "帮我打开背包"
        scene = "rpg"
        context = @{ map_name = "RPGMap"; combat_state = "idle" }
    }
    $chat = Invoke-JsonPost "/api/agent/chat" $chatBody $token
    $duplicate = Invoke-JsonPost "/api/agent/chat" $chatBody $token
    $calls = @($chat.data.tool_calls)
    if ($calls.Count -eq 0 -or -not $calls[0].call_id) {
        $chatJson = $chat | ConvertTo-Json -Depth 10
        throw "Agent 聊天响应没有返回可确认的工具调用。完整响应：`n$chatJson"
    }
    $call = $calls[0]

    $confirmBody = @{
        conversation_id = $chat.data.conversation_id
        call_id = $call.call_id
        approved = $true
        client_request_id = "agent_confirm_inventory"
    }
    $confirmed = Invoke-JsonPost "/api/agent/confirm" $confirmBody $token
    $confirmedAgain = Invoke-JsonPost "/api/agent/confirm" $confirmBody $token
    $history = Invoke-JsonPost "/api/agent/history" @{
        conversation_id = $chat.data.conversation_id
        limit = 30
    } $token
    $otherHistory = Invoke-ExpectedError "/api/agent/history" @{
        conversation_id = $chat.data.conversation_id
        limit = 30
    } $otherToken
    $deleted = Invoke-JsonPost "/api/agent/conversations/delete" @{
        conversation_id = $chat.data.conversation_id
    } $token
    $historyAfterDelete = Invoke-ExpectedError "/api/agent/history" @{
        conversation_id = $chat.data.conversation_id
        limit = 30
    } $token

    [pscustomobject]@{
        unauthorized_status = $unauthorized.status
        chat_ok = $chat.ok
        used_fallback = $chat.data.used_fallback
        tool_name = $call.tool_name
        tool_status = $call.status
        duplicate_message_same = $duplicate.data.message_id -eq $chat.data.message_id
        duplicate_fallback_same = $duplicate.data.used_fallback -eq $chat.data.used_fallback
        confirm_status = $confirmed.data.status
        client_action_type = $confirmed.data.client_action.type
        repeated_confirm_status = $confirmedAgain.data.status
        history_count = $history.data.messages.Count
        other_player_history_code = $otherHistory.code
        deleted = $deleted.data.deleted
        deleted_history_code = $historyAfterDelete.code
    }
} finally {
    if ($process -and -not $process.HasExited) { Stop-Process -Id $process.Id -Force }
}
