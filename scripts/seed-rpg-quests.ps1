param(
    [string]$BaseUrl = "http://127.0.0.1:8080",
    [string]$AdminLogin = "admin_demo",
    [string]$AdminPassword = $(if ($env:HELLO_SERVICE_DEV_ADMIN_PASSWORD) { $env:HELLO_SERVICE_DEV_ADMIN_PASSWORD } else { 'change_me_admin_password' })
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$admin = Invoke-RestMethod -Method Post -Uri "$BaseUrl/admin/login" -ContentType "application/json" -Body (@{
    login_name = $AdminLogin
    password = $AdminPassword
    single_session = $true
} | ConvertTo-Json)
$headers = @{ Authorization = "Bearer $($admin.data.session.token)" }

$quests = @(
    @{
        quest_id = "quest_arrive_camp"
        version = 1
        title = "初到营地"
        description = "前往营地主入口。"
        claim_mode = "manual"
        turn_in_npc_id = ""
        completion_flag = "arrive_camp_done"
        pre_quest_ids = @()
        required_flags = @()
        objectives = @(@{ objective_id = "reach_camp"; event_type = "target_reached"; target_id = "camp_main"; target_value = 1 })
        rewards = @(@{ kind = "currency"; resource_id = "gold"; amount = 150 })
        enabled = $true
    },
    @{
        quest_id = "quest_talk_katheryne"
        version = 1
        title = "营地事务"
        description = "与任务管理员交谈并在她那里提交。"
        claim_mode = "npc"
        turn_in_npc_id = "npc_katheryne"
        completion_flag = "talk_katheryne_done"
        pre_quest_ids = @("quest_arrive_camp")
        required_flags = @()
        objectives = @(@{ objective_id = "talk_katheryne"; event_type = "npc_talked"; target_id = "npc_katheryne"; target_value = 1 })
        rewards = @(@{ kind = "currency"; resource_id = "gold"; amount = 200 })
        enabled = $true
    },
    @{
        quest_id = "quest_machine_dog"
        version = 1
        title = "清理机器狗"
        description = "击杀 1 只机器狗。"
        claim_mode = "auto"
        turn_in_npc_id = ""
        completion_flag = "machine_dog_done"
        pre_quest_ids = @("quest_talk_katheryne")
        required_flags = @()
        objectives = @(@{ objective_id = "kill_machine_dog"; event_type = "monster_killed"; target_id = "machine_dog"; target_value = 1 })
        rewards = @(@{ kind = "currency"; resource_id = "gold"; amount = 500 })
        enabled = $true
    }
)

foreach ($quest in $quests) {
    $response = Invoke-RestMethod -Method Post -Uri "$BaseUrl/admin/quests/upsert" -Headers $headers -ContentType "application/json" -Body ($quest | ConvertTo-Json -Depth 8)
    [pscustomobject]@{
        quest_id = $response.data.quest_id
        version = $response.data.version
        claim_mode = $response.data.claim_mode
        enabled = $response.data.enabled
    }
}
