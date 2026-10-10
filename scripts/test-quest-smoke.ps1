param(
    [string]$ServiceExecutable = "D:\code\model\build-otel-off\Debug\hello_service.exe",
    [int]$Port = 18080
)

# 文件职责：启动一个临时内存版后端，验证任务配置、接受、推进、领奖和重复领奖幂等。
$env:HELLO_SERVICE_PORT = [string]$Port
$env:HELLO_SERVICE_HOST = "127.0.0.1"
$env:HELLO_SERVICE_STORAGE_MODE = "in_memory"
$env:HELLO_SERVICE_ENV = "development"
$env:HELLO_SERVICE_AUTO_BOOTSTRAP_DEV_ADMIN = "1"

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
        } catch {
            Start-Sleep -Milliseconds 250
        }
    }
    if (-not $ready) {
        throw "测试服务未能启动"
    }

    $adminPassword = if ($env:HELLO_SERVICE_DEV_ADMIN_PASSWORD) { $env:HELLO_SERVICE_DEV_ADMIN_PASSWORD } else { 'change_me_admin_password' }
    $admin = Invoke-RestMethod `
        -Method Post `
        -Uri "http://127.0.0.1:$Port/admin/login" `
        -ContentType "application/json" `
        -Body (@{ login_name = "admin_demo"; password = $adminPassword } | ConvertTo-Json)
    $headers = @{ Authorization = "Bearer $($admin.data.session.token)" }

    $questBody = @{
        quest_id = "quest_smoke_001"
        version = 1
        title = "任务闭环测试"
        description = "编译后自动验证"
        claim_mode = "manual"
        enabled = $true
        pre_quest_ids = @()
        required_flags = @()
        completion_flag = "smoke_done"
        objectives = @(@{
            objective_id = "kill_slime"
            event_type = "monster_killed"
            target_id = "slime"
            target_value = 2
        })
        rewards = @(@{
            kind = "currency"
            resource_id = "gold"
            amount = 10
        })
    } | ConvertTo-Json -Depth 8

    $configured = Invoke-RestMethod -Method Post -Uri "http://127.0.0.1:$Port/admin/quests/upsert" -Headers $headers -ContentType "application/json" -Body $questBody
    $accepted = Invoke-RestMethod -Method Post -Uri "http://127.0.0.1:$Port/quests/accept" -Headers $headers -ContentType "application/json" -Body (@{ quest_id = "quest_smoke_001"; request_id = "accept-smoke-001" } | ConvertTo-Json)
    $event = Invoke-RestMethod -Method Post -Uri "http://127.0.0.1:$Port/quests/events" -Headers $headers -ContentType "application/json" -Body (@{ event_id = "event-smoke-001"; event_type = "monster_killed"; target_id = "slime"; amount = 2; source_id = "local-test" } | ConvertTo-Json)
    $claimed = Invoke-RestMethod -Method Post -Uri "http://127.0.0.1:$Port/quests/claim" -Headers $headers -ContentType "application/json" -Body (@{ quest_id = "quest_smoke_001"; request_id = "claim-smoke-001" } | ConvertTo-Json)
    $repeated = Invoke-RestMethod -Method Post -Uri "http://127.0.0.1:$Port/quests/claim" -Headers $headers -ContentType "application/json" -Body (@{ quest_id = "quest_smoke_001"; request_id = "claim-smoke-002" } | ConvertTo-Json)

    [pscustomobject]@{
        configured = $configured.ok
        accepted_state = $accepted.data.state
        completed_state = $event.data.snapshot.quests[0].state
        claimed_state = $claimed.data.quest.state
        gold_after_claim = $claimed.data.player.gold
        gold_after_repeat = $repeated.data.player.gold
        repeat_detected = $repeated.data.already_claimed
        story_flag = $event.data.snapshot.story_flags[0]
    }
} finally {
    if ($process -and -not $process.HasExited) {
        Stop-Process -Id $process.Id -Force
    }
}
