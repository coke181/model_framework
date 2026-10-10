param(
    [string]$ServiceExecutable = "D:\code\model\build-otel-off\Debug\hello_service.exe",
    [int]$Port = 18081
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$env:HELLO_SERVICE_PORT = [string]$Port
$env:HELLO_SERVICE_HOST = "127.0.0.1"
$env:HELLO_SERVICE_STORAGE_MODE = "in_memory"
$env:HELLO_SERVICE_ENV = "development"
$env:HELLO_SERVICE_AUTO_BOOTSTRAP_DEV_ADMIN = "1"

$process = Start-Process -FilePath $ServiceExecutable -WorkingDirectory (Split-Path -Parent $PSScriptRoot) -WindowStyle Hidden -PassThru
try {
    for ($index = 0; $index -lt 30; ++$index) {
        try { Invoke-RestMethod -Uri "http://127.0.0.1:$Port/health" -TimeoutSec 1 | Out-Null; break }
        catch { if ($index -eq 29) { throw "测试服务未能启动" }; Start-Sleep -Milliseconds 250 }
    }

    $admin = Invoke-RestMethod -Method Post -Uri "http://127.0.0.1:$Port/admin/login" -ContentType "application/json" -Body (@{ login_name = "admin_demo"; password = "AdminDemo_123456" } | ConvertTo-Json)
    $headers = @{ Authorization = "Bearer $($admin.data.session.token)" }

    $definitions = @(
        @{ quest_id="test_manual"; claim_mode="manual"; turn_in_npc_id=""; event_type="target_reached"; target_id="camp"; flag="manual_done" },
        @{ quest_id="test_npc"; claim_mode="npc"; turn_in_npc_id="npc_test"; event_type="npc_talked"; target_id="npc_test"; flag="npc_done" },
        @{ quest_id="test_auto"; claim_mode="auto"; turn_in_npc_id=""; event_type="monster_killed"; target_id="machine_dog"; flag="auto_done" }
    )

    foreach ($definition in $definitions) {
        Invoke-RestMethod -Method Post -Uri "http://127.0.0.1:$Port/admin/quests/upsert" -Headers $headers -ContentType "application/json" -Body (@{
            quest_id=$definition.quest_id; version=1; title=$definition.quest_id; description="mode smoke"
            claim_mode=$definition.claim_mode; turn_in_npc_id=$definition.turn_in_npc_id; completion_flag=$definition.flag
            pre_quest_ids=@(); required_flags=@(); enabled=$true
            objectives=@(@{ objective_id="objective"; event_type=$definition.event_type; target_id=$definition.target_id; target_value=1 })
            rewards=@(@{ kind="currency"; resource_id="gold"; amount=10 })
        } | ConvertTo-Json -Depth 8) | Out-Null
        Invoke-RestMethod -Method Post -Uri "http://127.0.0.1:$Port/quests/accept" -Headers $headers -ContentType "application/json" -Body (@{ quest_id=$definition.quest_id; request_id="accept-$($definition.quest_id)" } | ConvertTo-Json) | Out-Null
    }

    $manualEvent = Invoke-RestMethod -Method Post -Uri "http://127.0.0.1:$Port/quests/events" -Headers $headers -ContentType "application/json" -Body (@{ event_id="event-manual"; event_type="target_reached"; target_id="camp"; amount=1; source_id="test" } | ConvertTo-Json)
    $manualDuplicate = Invoke-RestMethod -Method Post -Uri "http://127.0.0.1:$Port/quests/events" -Headers $headers -ContentType "application/json" -Body (@{ event_id="event-manual"; event_type="target_reached"; target_id="camp"; amount=1; source_id="test" } | ConvertTo-Json)
    $manualClaim = Invoke-RestMethod -Method Post -Uri "http://127.0.0.1:$Port/quests/claim" -Headers $headers -ContentType "application/json" -Body (@{ quest_id="test_manual"; request_id="claim-manual" } | ConvertTo-Json)

    Invoke-RestMethod -Method Post -Uri "http://127.0.0.1:$Port/quests/events" -Headers $headers -ContentType "application/json" -Body (@{ event_id="event-npc"; event_type="npc_talked"; target_id="npc_test"; amount=1; source_id="test" } | ConvertTo-Json) | Out-Null
    $npcClaim = Invoke-RestMethod -Method Post -Uri "http://127.0.0.1:$Port/quests/claim" -Headers $headers -ContentType "application/json" -Body (@{ quest_id="test_npc"; request_id="claim-npc"; npc_id="npc_test" } | ConvertTo-Json)

    $autoEvent = Invoke-RestMethod -Method Post -Uri "http://127.0.0.1:$Port/quests/events" -Headers $headers -ContentType "application/json" -Body (@{ event_id="event-auto"; event_type="monster_killed"; target_id="machine_dog"; amount=1; source_id="test" } | ConvertTo-Json)
    $snapshot = Invoke-RestMethod -Method Get -Uri "http://127.0.0.1:$Port/players/me/quests" -Headers $headers

    [pscustomobject]@{
        manual_completed = ($manualEvent.data.snapshot.quests | Where-Object quest_id -eq "test_manual").state
        duplicate_event = $manualDuplicate.data.duplicate_event
        manual_claimed = $manualClaim.data.quest.state
        npc_claimed = $npcClaim.data.quest.state
        auto_grant_count = $autoEvent.data.automatic_grants.Count
        auto_state = ($snapshot.data.quests | Where-Object quest_id -eq "test_auto").state
        story_flags = ($snapshot.data.story_flags -join ",")
    }
} finally {
    if ($process -and -not $process.HasExited) { Stop-Process -Id $process.Id -Force }
}
