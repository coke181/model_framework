#pragma once

// 文件职责：
// 封装任务定义、接受任务、事件推进和领奖流程，并把仓储失败转换为稳定 API 错误。
// 调用流程：
// UE 请求 /quests* -> HTTP 路由完成鉴权和字段解析 -> QuestService ->
// QuestRepository 在内存或 MySQL 中完成状态校验、幂等和事务 -> 返回任务快照。

#include "mf/game/mail_service.hpp"
#include "mf/game/repositories.hpp"

#include <memory>

namespace mf::game {

class QuestService {
public:
    explicit QuestService(std::shared_ptr<QuestRepository> quests);

    std::vector<QuestDefinition> list_definitions(bool include_disabled = false) const;
    QuestSnapshot get_snapshot(const std::string& player_id) const;
    ServiceResult<PlayerQuest> accept(
        const std::string& player_id,
        const std::string& quest_id,
        const std::string& request_id) const;
    ServiceResult<QuestEventResult> apply_event(
        const std::string& player_id,
        const std::string& event_id,
        const std::string& event_type,
        const std::string& target_id,
        std::int64_t amount,
        const std::string& source_id) const;
    ServiceResult<QuestClaimResult> claim(
        const std::string& player_id,
        const std::string& quest_id,
        const std::string& request_id,
        const std::string& npc_id) const;
    ServiceResult<QuestDefinition> upsert_definition(QuestDefinition definition) const;

private:
    std::shared_ptr<QuestRepository> quests_;
};

}  // namespace mf::game
