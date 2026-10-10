#include "mf/agent/task_service.hpp"

#include "mf/core/api.hpp"

#include <boost/beast/http/status.hpp>
#include <boost/json/array.hpp>
#include <random>

namespace mf::agent {
namespace {

std::string random_task_id() {
    static thread_local std::mt19937_64 generator{std::random_device{}()};
    return "agent_task_" + std::to_string(generator());
}

mf::core::ApiError error(boost::beast::http::status status, std::string code, std::string message) {
    return {.status = status, .code = std::move(code), .message = std::move(message)};
}

}  // namespace

mf::game::ServiceResult<AgentTaskState> AgentTaskService::create(
    const std::string& player_id, std::string goal, std::vector<AgentTaskStepState> steps) {
    if (goal.empty() || goal.size() > 1000 || steps.empty() || steps.size() > 32) {
        return {.error = error(boost::beast::http::status::bad_request, "agent_task_invalid", "任务目标或步骤无效")};
    }
    AgentTaskState task{
        .task_id = random_task_id(), .player_id = player_id, .goal = std::move(goal),
        .status = "executing", .steps = std::move(steps),
        .created_at = std::chrono::system_clock::now(), .updated_at = std::chrono::system_clock::now(),
    };
    std::scoped_lock lock{mutex_};
    tasks_[task.task_id] = task;
    return {.value = task};
}

mf::game::ServiceResult<AgentTaskState> AgentTaskService::get(
    const std::string& player_id, const std::string& task_id) const {
    std::scoped_lock lock{mutex_};
    const auto it = tasks_.find(task_id);
    if (it == tasks_.end() || it->second.player_id != player_id) {
        return {.error = error(boost::beast::http::status::not_found, "agent_task_not_found", "Agent 任务不存在")};
    }
    return {.value = it->second};
}

mf::game::ServiceResult<AgentTaskState> AgentTaskService::record_step_result(
    const std::string& player_id, const std::string& task_id, const std::string& step_id,
    bool success, boost::json::value result, std::string step_error) {
    std::scoped_lock lock{mutex_};
    const auto it = tasks_.find(task_id);
    if (it == tasks_.end() || it->second.player_id != player_id) {
        return {.error = error(boost::beast::http::status::not_found, "agent_task_not_found", "Agent 任务不存在")};
    }
    if (it->second.status == "cancelled" || it->second.status == "completed") {
        return {.error = error(boost::beast::http::status::conflict, "agent_task_finished", "Agent 任务已经结束")};
    }
    auto step = std::find_if(it->second.steps.begin(), it->second.steps.end(), [&](const auto& value) {
        return value.step_id == step_id;
    });
    if (step == it->second.steps.end()) {
        return {.error = error(boost::beast::http::status::not_found, "agent_step_not_found", "Agent 步骤不存在")};
    }
    step->status = success ? "completed" : "failed";
    step->result = std::move(result);
    step->error = std::move(step_error);
    it->second.updated_at = std::chrono::system_clock::now();
    if (!success) {
        it->second.status = "failed";
    } else if (std::all_of(it->second.steps.begin(), it->second.steps.end(), [](const auto& value) {
        return value.status == "completed";
    })) {
        it->second.status = "completed";
    }
    return {.value = it->second};
}

mf::game::ServiceResult<AgentTaskState> AgentTaskService::cancel(
    const std::string& player_id, const std::string& task_id) {
    std::scoped_lock lock{mutex_};
    const auto it = tasks_.find(task_id);
    if (it == tasks_.end() || it->second.player_id != player_id) {
        return {.error = error(boost::beast::http::status::not_found, "agent_task_not_found", "Agent 任务不存在")};
    }
    it->second.status = "cancelled";
    it->second.updated_at = std::chrono::system_clock::now();
    return {.value = it->second};
}

boost::json::object to_json(const AgentTaskState& task) {
    boost::json::array steps;
    for (const auto& step : task.steps) {
        steps.push_back(boost::json::object{
            {"step_id", step.step_id}, {"tool", step.tool_name}, {"title", step.title},
            {"status", step.status}, {"result", step.result}, {"error", step.error},
        });
    }
    return {{"task_id", task.task_id}, {"goal", task.goal}, {"status", task.status}, {"steps", std::move(steps)}};
}

}  // namespace mf::agent
