#include "mf/game/quest_service.hpp"
#include "mf/core/profiling.hpp"

// 文件职责：
// 实现任务服务的参数校验和统一错误映射。
// 调用流程：
// 路由传入已鉴权 player_id 和请求字段 -> QuestService 校验 ->
// QuestRepository 执行持久化/事务 -> ServiceResult 返回给 HTTP 层。

namespace mf::game {
namespace {

mf::core::ApiError quest_error(std::string code) {
    using boost::beast::http::status;
    if (code == "quest_not_found") {
        return {.status = status::not_found, .code = code, .message = "任务不存在或未启用"};
    }
    if (code == "quest_condition_not_met") {
        return {.status = status::forbidden, .code = code, .message = "任务前置条件尚未满足"};
    }
    if (code == "quest_not_accepted") {
        return {.status = status::conflict, .code = code, .message = "玩家尚未接受该任务"};
    }
    if (code == "quest_not_completed") {
        return {.status = status::conflict, .code = code, .message = "任务尚未完成，暂时不能领取奖励"};
    }
    if (code == "quest_npc_mismatch") {
        return {.status = status::forbidden, .code = code, .message = "必须在任务指定的 NPC 处提交"};
    }
    if (code == "quest_event_rejected") {
        return {.status = status::unprocessable_entity, .code = code, .message = "该任务事件没有匹配到可推进的目标"};
    }
    if (code == "quest_request_conflict") {
        return {.status = status::conflict, .code = code, .message = "同一个 request_id 已被其他任务请求使用"};
    }
    if (code == "player_not_found") {
        return {.status = status::not_found, .code = code, .message = "玩家不存在"};
    }
    return {.status = status::conflict, .code = code.empty() ? "quest_operation_failed" : code, .message = "任务操作失败"};
}

}  // namespace

QuestService::QuestService(std::shared_ptr<QuestRepository> quests)
    : quests_(std::move(quests)) {}

std::vector<QuestDefinition> QuestService::list_definitions(bool include_disabled) const {
    MF_PROFILE_SCOPE("Quest list definitions");
    return quests_->list_definitions(include_disabled);
}

QuestSnapshot QuestService::get_snapshot(const std::string& player_id) const {
    MF_PROFILE_SCOPE("Quest get snapshot");
    return quests_->get_snapshot(player_id);
}

ServiceResult<PlayerQuest> QuestService::accept(
    const std::string& player_id,
    const std::string& quest_id,
    const std::string& request_id) const {
    if (quest_id.empty() || request_id.empty() || quest_id.size() > 96 || request_id.size() > 96) {
        return {.error = mf::core::ApiError{
                    .status = boost::beast::http::status::bad_request,
                    .code = "invalid_quest_request",
                    .message = "quest_id 和 request_id 不能为空，且长度不能超过 96 个字符",
                }};
    }
    std::string failure_code;
    auto quest = quests_->accept(player_id, quest_id, request_id, failure_code);
    if (!quest) {
        return {.error = quest_error(std::move(failure_code))};
    }
    return {.value = std::move(quest)};
}

ServiceResult<QuestEventResult> QuestService::apply_event(
    const std::string& player_id,
    const std::string& event_id,
    const std::string& event_type,
    const std::string& target_id,
    std::int64_t amount,
    const std::string& source_id) const {
    if (event_id.empty() || event_type.empty() || event_id.size() > 96 || event_type.size() > 64 ||
        target_id.size() > 96 || source_id.size() > 128 || amount <= 0 || amount > 1000000) {
        return {.error = mf::core::ApiError{
                    .status = boost::beast::http::status::bad_request,
                    .code = "invalid_quest_event",
                    .message = "任务事件字段为空、长度超限，或 amount 不在 1 到 1000000 之间",
                }};
    }
    std::string failure_code;
    auto result = quests_->apply_event(
        player_id,
        event_id,
        event_type,
        target_id,
        amount,
        source_id,
        failure_code);
    if (!failure_code.empty()) {
        return {.error = quest_error(std::move(failure_code))};
    }
    return {.value = std::move(result)};
}

ServiceResult<QuestClaimResult> QuestService::claim(
    const std::string& player_id,
    const std::string& quest_id,
    const std::string& request_id,
    const std::string& npc_id) const {
    if (quest_id.empty() || request_id.empty() || quest_id.size() > 96 || request_id.size() > 96) {
        return {.error = mf::core::ApiError{
                    .status = boost::beast::http::status::bad_request,
                    .code = "invalid_quest_claim",
                    .message = "quest_id 和 request_id 不能为空，且长度不能超过 96 个字符",
                }};
    }
    std::string failure_code;
    auto result = quests_->claim(player_id, quest_id, request_id, npc_id, failure_code);
    if (!result) {
        return {.error = quest_error(std::move(failure_code))};
    }
    return {.value = std::move(result)};
}

ServiceResult<QuestDefinition> QuestService::upsert_definition(QuestDefinition definition) const {
    if (definition.quest_id.empty() || definition.title.empty() || definition.objectives.empty()) {
        return {.error = mf::core::ApiError{
                    .status = boost::beast::http::status::bad_request,
                    .code = "invalid_quest_definition",
                    .message = "quest_id、title 和 objectives 不能为空",
                }};
    }
    for (const auto& objective : definition.objectives) {
        if (objective.objective_id.empty() || objective.event_type.empty() || objective.target_value <= 0) {
            return {.error = mf::core::ApiError{
                        .status = boost::beast::http::status::bad_request,
                        .code = "invalid_quest_objective",
                        .message = "任务目标必须包含 objective_id、event_type，且 target_value 大于 0",
                    }};
        }
    }
    return {.value = quests_->upsert_definition(std::move(definition))};
}

}  // namespace mf::game
