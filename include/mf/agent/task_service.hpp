#pragma once

#include "mf/game/mail_service.hpp"

#include <boost/json/value.hpp>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace mf::agent {

struct AgentTaskStepState {
    std::string step_id;
    std::string tool_name;
    std::string title;
    std::string status = "pending";
    boost::json::value result = nullptr;
    std::string error;
};

struct AgentTaskState {
    std::string task_id;
    std::string player_id;
    std::string goal;
    std::string status = "created";
    std::vector<AgentTaskStepState> steps;
    std::optional<std::chrono::system_clock::time_point> created_at;
    std::optional<std::chrono::system_clock::time_point> updated_at;
};

class AgentTaskService {
public:
    mf::game::ServiceResult<AgentTaskState> create(
        const std::string& player_id,
        std::string goal,
        std::vector<AgentTaskStepState> steps);
    mf::game::ServiceResult<AgentTaskState> get(
        const std::string& player_id,
        const std::string& task_id) const;
    mf::game::ServiceResult<AgentTaskState> record_step_result(
        const std::string& player_id,
        const std::string& task_id,
        const std::string& step_id,
        bool success,
        boost::json::value result,
        std::string error);
    mf::game::ServiceResult<AgentTaskState> cancel(
        const std::string& player_id,
        const std::string& task_id);

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, AgentTaskState> tasks_;
};

boost::json::object to_json(const AgentTaskState& task);

}  // namespace mf::agent
