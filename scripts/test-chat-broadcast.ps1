param(
    [string]$BaseUrl = "http://120.26.173.47",
    [string]$WebSocketUrl = "ws://120.26.173.47:8090",
    [int]$Connections = 10,
    [int]$Messages = 100,
    [int]$ReceiveTimeoutSeconds = 30
)

$ErrorActionPreference = "Stop"

function Send-JsonText {
    param(
        [System.Net.WebSockets.ClientWebSocket]$Socket,
        [hashtable]$Payload
    )

    $json = $Payload | ConvertTo-Json -Compress -Depth 8
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($json)
    $segment = [ArraySegment[byte]]::new($bytes)
    $sendTask = $Socket.SendAsync(
        $segment,
        [System.Net.WebSockets.WebSocketMessageType]::Text,
        $true,
        [Threading.CancellationToken]::None)
    $sendTask.GetAwaiter().GetResult() | Out-Null
}

function Receive-JsonText {
    param(
        [System.Net.WebSockets.ClientWebSocket]$Socket,
        [int]$TimeoutSeconds
    )

    $buffer = New-Object byte[] 65536
    $builder = [System.Text.StringBuilder]::new()
    $cancel = [Threading.CancellationTokenSource]::new([TimeSpan]::FromSeconds($TimeoutSeconds))
    try {
        do {
            $segment = [ArraySegment[byte]]::new($buffer)
            $result = $Socket.ReceiveAsync($segment, $cancel.Token).GetAwaiter().GetResult()
            if ($result.MessageType -eq [System.Net.WebSockets.WebSocketMessageType]::Close) {
                return $null
            }
            [void]$builder.Append([System.Text.Encoding]::UTF8.GetString($buffer, 0, $result.Count))
        } while (-not $result.EndOfMessage)
        return ($builder.ToString() | ConvertFrom-Json)
    }
    finally {
        $cancel.Dispose()
    }
}

$sockets = [System.Collections.Generic.List[System.Net.WebSockets.ClientWebSocket]]::new()
$totalExpected = $Connections * $Messages
$received = 0
$authName = "chat_{0}" -f ([DateTimeOffset]::UtcNow.ToUnixTimeSeconds() % 100000000)

try {
    for ($index = 0; $index -lt $Connections; $index++) {
        $loginName = "{0}_{1:D2}" -f $authName, $index
        $body = @{ login_name = $loginName; display_name = $loginName } | ConvertTo-Json -Compress
        $login = Invoke-RestMethod -Method Post -Uri "$BaseUrl/auth/quick-login" -ContentType "application/json" -Body $body
        $token = [string]$login.data.session.token
        if ([string]::IsNullOrWhiteSpace($token)) {
            throw "quick-login 没有返回 token"
        }

        $socket = [System.Net.WebSockets.ClientWebSocket]::new()
        $connectTask = $socket.ConnectAsync([Uri]$WebSocketUrl, [Threading.CancellationToken]::None)
        $connectTask.GetAwaiter().GetResult() | Out-Null
        Send-JsonText $socket @{ type = "auth"; token = $token }
        $authResult = Receive-JsonText $socket 10
        if (-not $authResult.ok) { throw "第 $index 个连接鉴权失败" }
        Send-JsonText $socket @{ type = "join"; channel_kind = "lobby"; scope_id = "global" }
        $joinResult = Receive-JsonText $socket 10
        if (-not $joinResult.ok) { throw "第 $index 个连接入大厅失败" }
        [void]$sockets.Add($socket)
    }

    $started = [Diagnostics.Stopwatch]::StartNew()
    for ($index = 0; $index -lt $Messages; $index++) {
        Send-JsonText $sockets[0] @{ type = "say"; message_kind = "text"; text = "broadcast_test_$index" }
    }

    while ($received -lt $totalExpected -and $started.Elapsed.TotalSeconds -lt $ReceiveTimeoutSeconds) {
        foreach ($socket in $sockets) {
            if ($socket.State -ne [System.Net.WebSockets.WebSocketState]::Open) { continue }
            try {
                $message = Receive-JsonText $socket 1
                if ($null -ne $message -and $message.type -eq "message") { $received++ }
            }
            catch [System.OperationCanceledException] {
                continue
            }
        }
    }
    $started.Stop()

    [pscustomobject]@{
        connections = $Connections
        sent_messages = $Messages
        expected_deliveries = $totalExpected
        received_deliveries = $received
        missing_deliveries = $totalExpected - $received
        elapsed_seconds = [math]::Round($started.Elapsed.TotalSeconds, 3)
        receive_rate = if ($started.Elapsed.TotalSeconds -gt 0) {
            [math]::Round($received / $started.Elapsed.TotalSeconds, 1)
        } else { 0 }
        result = if ($received -eq $totalExpected) { "PASS" } else { "FAIL" }
    } | ConvertTo-Json
}
finally {
    foreach ($socket in $sockets) {
        try { $socket.Dispose() } catch {}
    }
}
