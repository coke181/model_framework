#pragma once

// 文件职责：
// 1. 定义后台任务调度器的公共接口。
// 2. 让 API 进程只负责“写入任务/重试任务”，让独立 worker 进程负责“claim 并执行任务”。
// 3. 让后台任务逻辑脱离 HTTP 路由文件，便于单独 review 和后续继续拆服务。
//
// 文件调用流程：
// 1. `hello_service` 调用 `make_admin_task_scheduler(...)`，只用它的 schedule/retry 接口。
// 2. `hello_task_worker` 调用同一个工厂创建实例，再执行 `run_until_stopped(...)`。
// 3. 调度器从 `admin_task` 仓储 claim 到期任务，执行业务，再回写状态与审计。

#include "mf/auth/account_service.hpp"
#include "mf/chat/chat_service.hpp"
#include "mf/core/logger.hpp"
#include "mf/core/metrics.hpp"
#include "mf/game/repositories.hpp"

#include <boost/json/object.hpp>

#include <chrono>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>

class AdminTaskScheduler {
public:
    AdminTaskScheduler(
        std::shared_ptr<mf::chat::ChatService> chat_service,
        std::shared_ptr<mf::auth::AccountService> account_service,
        mf::game::RepositoryBundle repositories,
        std::shared_ptr<mf::game::AuditLogRepository> audit_logs,
        std::shared_ptr<mf::game::AdminTaskRepository> task_repository,
        std::shared_ptr<mf::core::MetricsRegistry> metrics,
        std::shared_ptr<mf::core::Logger> logger);
    ~AdminTaskScheduler();

    AdminTaskScheduler(const AdminTaskScheduler&) = delete;
    AdminTaskScheduler& operator=(const AdminTaskScheduler&) = delete;

    mf::game::AdminTask schedule(
        mf::game::ChatChannel channel,
        std::string text,
        std::string operator_id,
        std::string request_ip,
        std::string user_agent,
        std::string trace_id,
        std::chrono::system_clock::time_point execute_at);

    mf::game::AdminTask schedule_batch_mute(
        boost::json::object request_object,
        std::string operator_id,
        std::string request_ip,
        std::string user_agent,
        std::string trace_id);

    mf::game::AdminTask schedule_batch_account_status(
        boost::json::object request_object,
        std::string operator_id,
        std::string request_ip,
        std::string user_agent,
        std::string trace_id);

    mf::game::AdminTask schedule_batch_reward(
        boost::json::object request_object,
        std::string operator_id,
        std::string request_ip,
        std::string user_agent,
        std::string trace_id);

    mf::game::AdminTask schedule_batch_mail(
        boost::json::object request_object,
        std::string operator_id,
        std::string request_ip,
        std::string user_agent,
        std::string trace_id);

    std::optional<mf::game::AdminTask> retry_task(
        const mf::game::AdminTask& original_task,
        bool failed_only,
        std::string operator_id,
        std::string request_ip,
        std::string user_agent,
        std::string trace_id);

    // worker 进程在当前线程里阻塞执行轮询，
    // API 进程不调用这个方法，因此不会再和在线请求抢同一个进程内资源。
    void run_until_stopped(std::stop_token stop_token);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

std::shared_ptr<AdminTaskScheduler> make_admin_task_scheduler(
    const std::shared_ptr<mf::chat::ChatService>& chat_service,
    const std::shared_ptr<mf::auth::AccountService>& account_service,
    const mf::game::RepositoryBundle& repositories,
    const std::shared_ptr<mf::game::AuditLogRepository>& audit_logs,
    const std::shared_ptr<mf::game::AdminTaskRepository>& task_repository,
    const std::shared_ptr<mf::core::MetricsRegistry>& metrics,
    const std::shared_ptr<mf::core::Logger>& logger);
