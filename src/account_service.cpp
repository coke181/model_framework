#include "mf/auth/account_service.hpp"
#include "mf/core/profiling.hpp"

// 文件职责：
// 实现正式账号体系的核心业务流程，包括登录风控、会话策略、改密和找回密码。
// 调用流程：
// handler 把请求字段交给 AccountService ->
// AccountService 统一完成字段校验、仓储访问、风控判断、密码哈希和 Session 签发 ->
// handler 只负责把结果转成 HTTP 响应。

#include <algorithm>
#include <cctype>
#include <chrono>
#include <mutex>
#include <random>
#include <string_view>

namespace mf::auth {
namespace {

// 认证记录缓存时间很短，主要覆盖同一登录高峰中的重复请求，
// 同时把封号/改密状态的最长滞后控制在几秒内。
constexpr auto kPasswordAccountCacheTtl = std::chrono::seconds{2};
constexpr auto kLoginPlayerCacheTtl = std::chrono::seconds{30};

// 密码校验会大量占用 CPU 和内存带宽。默认保留约 20% 的逻辑处理器给 HTTP、
// MySQL、Redis 和操作系统，避免把所有业务线程都挤在 Argon2 任务上。
std::size_t default_password_hash_thread_count() {
    const auto cpu_count = static_cast<std::size_t>(std::thread::hardware_concurrency());
    if (cpu_count <= 4) {
        return std::max<std::size_t>(1, cpu_count / 2);
    }
    return std::max<std::size_t>(1, cpu_count - std::max<std::size_t>(2, cpu_count / 5));
}

struct ValidationIssue {
    std::string code;
    std::string message;
    boost::json::object details;
};

mf::core::ApiError make_error(
    boost::beast::http::status status,
    std::string code,
    std::string message,
    boost::json::object details = {}) {
    return mf::core::ApiError{
        .status = status,
        .code = std::move(code),
        .message = std::move(message),
        .details = std::move(details),
    };
}

bool is_locked(const mf::game::Account& account) {
    return account.locked_until && *account.locked_until > std::chrono::system_clock::now();
}

bool is_ascii_alnum(unsigned char ch) {
    return std::isalnum(ch) != 0;
}

bool is_special_password_char(unsigned char ch) {
    return std::ispunct(ch) != 0;
}

std::optional<ValidationIssue> validate_login_name_issue(std::string_view login_name) {
    if (login_name.empty()) {
        return ValidationIssue{
            .code = "login_name_required",
            .message = "必须填写登录名",
            .details = {{"field", "login_name"}, {"field_label", "登录名"}},
        };
    }
    if (login_name.size() < 3 || login_name.size() > 32) {
        return ValidationIssue{
            .code = "invalid_login_name_length",
            .message = "登录名长度必须在 3 到 32 个字符之间",
            .details =
                {
                    {"field", "login_name"},
                    {"field_label", "登录名"},
                    {"min_length", 3},
                    {"max_length", 32},
                    {"actual_length", static_cast<std::int64_t>(login_name.size())},
                },
        };
    }
    for (const unsigned char ch : login_name) {
        if (!(is_ascii_alnum(ch) || ch == '_' || ch == '-' || ch == '.')) {
            return ValidationIssue{
                .code = "invalid_login_name_character",
                .message = "登录名只能包含字母、数字、下划线、连字符和点号",
                .details =
                    {
                        {"field", "login_name"},
                        {"field_label", "登录名"},
                        {"invalid_character", std::string(1, static_cast<char>(ch))},
                        {"allowed_pattern", "[A-Za-z0-9_.-]"},
                        {"allowed_pattern_text", "字母、数字、下划线、连字符、点号"},
                    },
            };
        }
    }
    return std::nullopt;
}

std::optional<ValidationIssue> validate_display_name_issue(std::string_view display_name) {
    if (display_name.empty()) {
        return ValidationIssue{
            .code = "display_name_required",
            .message = "必须填写显示名",
            .details = {{"field", "display_name"}, {"field_label", "显示名"}},
        };
    }
    if (display_name.size() > 32) {
        return ValidationIssue{
            .code = "invalid_display_name_length",
            .message = "显示名长度必须在 1 到 32 个字符之间",
            .details =
                {
                    {"field", "display_name"},
                    {"field_label", "显示名"},
                    {"max_length", 32},
                    {"actual_length", static_cast<std::int64_t>(display_name.size())},
                },
        };
    }
    return std::nullopt;
}

std::optional<ValidationIssue> validate_password_issue(std::string_view password) {
    if (password.empty()) {
        return ValidationIssue{
            .code = "password_required",
            .message = "必须填写密码",
            .details = {{"field", "password"}, {"field_label", "密码"}},
        };
    }
    if (password.size() < 8 || password.size() > 128) {
        return ValidationIssue{
            .code = "invalid_password_length",
            .message = "密码长度必须在 8 到 128 个字符之间",
            .details =
                {
                    {"field", "password"},
                    {"field_label", "密码"},
                    {"min_length", 8},
                    {"max_length", 128},
                    {"actual_length", static_cast<std::int64_t>(password.size())},
                },
        };
    }

    bool has_upper = false;
    bool has_lower = false;
    bool has_digit = false;
    bool has_special = false;
    for (const unsigned char ch : password) {
        has_upper = has_upper || std::isupper(ch);
        has_lower = has_lower || std::islower(ch);
        has_digit = has_digit || std::isdigit(ch);
        has_special = has_special || is_special_password_char(ch);
    }

    boost::json::array missing_rules;
    boost::json::array missing_rule_labels;
    if (!has_upper) {
        missing_rules.push_back("uppercase_letter");
        missing_rule_labels.push_back("大写字母");
    }
    if (!has_lower) {
        missing_rules.push_back("lowercase_letter");
        missing_rule_labels.push_back("小写字母");
    }
    if (!has_digit) {
        missing_rules.push_back("digit");
        missing_rule_labels.push_back("数字");
    }
    if (!has_special) {
        missing_rules.push_back("special_character");
        missing_rule_labels.push_back("特殊字符");
    }
    if (!missing_rules.empty()) {
        return ValidationIssue{
            .code = "weak_password",
            .message = "密码必须同时包含大写字母、小写字母、数字和特殊字符",
            .details =
                {
                    {"field", "password"},
                    {"field_label", "密码"},
                    {"missing_rules", std::move(missing_rules)},
                    {"missing_rule_labels", std::move(missing_rule_labels)},
                },
        };
    }
    return std::nullopt;
}

std::optional<ValidationIssue> validate_email_issue(std::string_view email) {
    if (email.empty()) {
        return std::nullopt;
    }
    const auto at = email.find('@');
    const auto dot = email.rfind('.');
    if (at == std::string_view::npos || at == 0 || at + 1 >= email.size() || dot == std::string_view::npos || dot <= at + 1 || dot + 1 >= email.size()) {
        return ValidationIssue{
            .code = "invalid_email_format",
            .message = "邮箱格式不正确",
            .details = {{"field", "email"}, {"field_label", "邮箱"}},
        };
    }
    return std::nullopt;
}

std::optional<ValidationIssue> validate_phone_number_issue(std::string_view phone_number) {
    if (phone_number.empty()) {
        return std::nullopt;
    }
    if (phone_number.size() > 32) {
        return ValidationIssue{
            .code = "invalid_phone_number_length",
            .message = "手机号长度不能超过 32 个字符",
            .details =
                {
                    {"field", "phone_number"},
                    {"field_label", "手机号"},
                    {"max_length", 32},
                    {"actual_length", static_cast<std::int64_t>(phone_number.size())},
                },
        };
    }
    for (const unsigned char ch : phone_number) {
        if (!(std::isdigit(ch) || ch == '+' || ch == '-' || ch == ' ' || ch == '(' || ch == ')')) {
            return ValidationIssue{
                .code = "invalid_phone_number_character",
                .message = "手机号包含不支持的字符",
                .details =
                    {
                        {"field", "phone_number"},
                        {"field_label", "手机号"},
                        {"invalid_character", std::string(1, static_cast<char>(ch))},
                    },
            };
        }
    }
    return std::nullopt;
}

std::string random_token() {
    std::random_device device;
    std::mt19937_64 generator{device()};
    std::uniform_int_distribution<unsigned long long> distribution;
    return std::to_string(distribution(generator)) + std::to_string(distribution(generator));
}

}  // 匿名命名空间

AccountService::AccountService(
    std::shared_ptr<mf::game::AccountRepository> accounts,
    std::shared_ptr<mf::game::PlayerRepository> players,
    std::shared_ptr<mf::game::AuthSecurityRepository> auth_security,
    std::shared_ptr<PasswordHasher> password_hasher,
    std::shared_ptr<TokenService> token_service,
    std::size_t password_hash_threads,
    std::size_t password_hash_max_pending,
    std::chrono::seconds register_rate_window,
    std::int64_t register_max_per_login_ip,
    std::int64_t register_max_per_ip,
    std::shared_ptr<mf::core::MetricsRegistry> metrics)
    : accounts_(std::move(accounts)),
      players_(std::move(players)),
      auth_security_(std::move(auth_security)),
      password_hasher_(std::move(password_hasher)),
      token_service_(std::move(token_service)),
      login_audit_dispatcher_(std::make_shared<LoginAuditDispatcher>(1024, metrics)),
      password_work_dispatcher_(std::make_shared<PasswordWorkDispatcher>(
          password_hash_threads == 0
              ? default_password_hash_thread_count()
              : password_hash_threads,
          password_hash_max_pending == 0
              ? std::max<std::size_t>(1, (password_hash_threads == 0
                                               ? default_password_hash_thread_count()
                                               : password_hash_threads) * 2)
              : password_hash_max_pending,
          std::move(metrics))),
      register_rate_window_(std::max(std::chrono::seconds{1}, register_rate_window)),
      register_max_per_login_ip_(std::max<std::int64_t>(1, register_max_per_login_ip)),
      register_max_per_ip_(std::max<std::int64_t>(1, register_max_per_ip)) {}

AccountService::~AccountService() = default;

AccountService::LoginAuditDispatcher::LoginAuditDispatcher(
    std::size_t max_pending,
    std::shared_ptr<mf::core::MetricsRegistry> metrics)
    : max_pending_(std::max<std::size_t>(1, max_pending)),
      metrics_(std::move(metrics)),
      worker_([this] { run(); }) {}

AccountService::LoginAuditDispatcher::~LoginAuditDispatcher() {
    {
        std::lock_guard lock{mutex_};
        stopping_ = true;
    }
    condition_.notify_one();
    if (worker_.joinable()) {
        worker_.join();
    }
}

bool AccountService::LoginAuditDispatcher::try_enqueue(
    std::string account_id,
    std::function<void()> task) {
    if (!task) {
        return true;
    }
    {
        std::lock_guard lock{mutex_};
        const auto now = std::chrono::steady_clock::now();
        constexpr auto kAuditMinInterval = std::chrono::seconds{10};
        const auto last_accepted = last_accepted_at_.find(account_id);
        if (pending_accounts_.contains(account_id) ||
            (last_accepted != last_accepted_at_.end() &&
             now - last_accepted->second < kAuditMinInterval)) {
            if (metrics_) {
                metrics_->increment_counter("mf_login_audit_coalesced_total");
            }
            return true;
        }
        if (stopping_ || tasks_.size() >= max_pending_) {
            if (metrics_) {
                metrics_->increment_counter("mf_login_audit_queue_rejected_total");
            }
            return false;
        }
        pending_accounts_.insert(account_id);
        last_accepted_at_[account_id] = now;
        if (last_accepted_at_.size() > 100000) {
            for (auto it = last_accepted_at_.begin(); it != last_accepted_at_.end();) {
                if (now - it->second >= kAuditMinInterval) {
                    it = last_accepted_at_.erase(it);
                } else {
                    ++it;
                }
            }
        }
        tasks_.push_back(Task{
            .account_id = std::move(account_id),
            .function = std::move(task),
        });
        if (metrics_) {
            metrics_->set_gauge("mf_login_audit_queue_pending", static_cast<double>(tasks_.size()));
        }
    }
    condition_.notify_one();
    return true;
}

void AccountService::LoginAuditDispatcher::run() {
    for (;;) {
        Task task;
        {
            std::unique_lock lock{mutex_};
            condition_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
            if (tasks_.empty() && stopping_) {
                return;
            }
            task = std::move(tasks_.front());
            tasks_.pop_front();
            if (metrics_) {
                metrics_->set_gauge("mf_login_audit_queue_pending", static_cast<double>(tasks_.size()));
            }
        }
        try {
            task.function();
        } catch (...) {
            // 审计写入失败不能让后台线程退出；下一次登录仍会继续尝试写入。
        }
        {
            std::lock_guard lock{mutex_};
            pending_accounts_.erase(task.account_id);
        }
    }
}

AccountService::PasswordWorkDispatcher::PasswordWorkDispatcher(
    std::size_t worker_count,
    std::size_t max_pending,
    std::shared_ptr<mf::core::MetricsRegistry> metrics)
    : max_pending_(std::max<std::size_t>(1, max_pending)),
      metrics_(std::move(metrics)) {
    worker_count = std::max<std::size_t>(1, worker_count);
    workers_.reserve(worker_count);
    for (std::size_t index = 0; index < worker_count; ++index) {
        workers_.emplace_back([this] { run(); });
    }
}

AccountService::PasswordWorkDispatcher::~PasswordWorkDispatcher() {
    {
        std::lock_guard lock{mutex_};
        stopping_ = true;
    }
    condition_.notify_all();
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

bool AccountService::PasswordWorkDispatcher::try_enqueue(std::function<void()> task) {
    {
        std::lock_guard lock{mutex_};
        if (stopping_ || tasks_.size() >= max_pending_) {
            if (metrics_) {
                metrics_->increment_counter("mf_password_queue_rejected_total");
            }
            return false;
        }
        tasks_.push_back(Task{
            .function = std::move(task),
            .enqueued_at = std::chrono::steady_clock::now(),
        });
        if (metrics_) {
            metrics_->set_gauge(
                "mf_password_queue_pending",
                static_cast<double>(tasks_.size()));
        }
    }
    condition_.notify_one();
    return true;
}

void AccountService::PasswordWorkDispatcher::run() {
    for (;;) {
        Task task;
        {
            std::unique_lock lock{mutex_};
            condition_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
            if (tasks_.empty() && stopping_) {
                return;
            }
            task = std::move(tasks_.front());
            tasks_.pop_front();
            if (metrics_) {
                metrics_->set_gauge(
                    "mf_password_queue_pending",
                    static_cast<double>(tasks_.size()));
            }
        }
        if (metrics_) {
            metrics_->observe_histogram_ms(
                "mf_password_queue_wait_duration_ms",
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - task.enqueued_at)
                    .count());
        }
        const auto started_at = std::chrono::steady_clock::now();
        task.function();
        if (metrics_) {
            metrics_->observe_histogram_ms(
                "mf_password_task_duration_ms",
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - started_at)
                    .count());
        }
    }
}

std::optional<mf::game::PasswordAccountRecord> AccountService::find_cached_password_account(
    const std::string& login_name) const {
    std::scoped_lock lock{password_account_cache_mutex_};
    const auto it = password_account_cache_.find(login_name);
    if (it == password_account_cache_.end()) {
        return std::nullopt;
    }
    if (std::chrono::steady_clock::now() >= it->second.second) {
        password_account_cache_.erase(it);
        return std::nullopt;
    }
    return it->second.first;
}

void AccountService::cache_password_account(
    const std::string& login_name,
    const mf::game::PasswordAccountRecord& account) const {
    std::scoped_lock lock{password_account_cache_mutex_};
    if (password_account_cache_.size() >= 4096 &&
        password_account_cache_.find(login_name) == password_account_cache_.end()) {
        // 防止攻击者用大量不同登录名把进程内缓存无限撑大。
        password_account_cache_.clear();
    }
    password_account_cache_[login_name] = {
        account,
        std::chrono::steady_clock::now() + kPasswordAccountCacheTtl,
    };
}

void AccountService::clear_password_account_cache() const {
    std::scoped_lock lock{password_account_cache_mutex_, login_player_cache_mutex_};
    password_account_cache_.clear();
    login_player_cache_.clear();
}

std::optional<mf::game::PlayerProfile> AccountService::find_cached_login_player(
    const std::string& account_id,
    const std::string& nickname) const {
    std::scoped_lock lock{login_player_cache_mutex_};
    const auto it = login_player_cache_.find(account_id);
    if (it == login_player_cache_.end()) {
        return std::nullopt;
    }
    if (std::chrono::steady_clock::now() >= it->second.second || it->second.first.nickname != nickname) {
        login_player_cache_.erase(it);
        return std::nullopt;
    }
    return it->second.first;
}

void AccountService::cache_login_player(const mf::game::PlayerProfile& player) const {
    auto identity = player;
    identity.gold = 0;
    identity.gem = 0;
    identity.currencies.clear();
    identity.inventory.clear();
    std::scoped_lock lock{login_player_cache_mutex_};
    if (login_player_cache_.size() >= 4096 &&
        login_player_cache_.find(identity.account_id) == login_player_cache_.end()) {
        // 登录缓存有界，防止大量不同账号请求持续占用进程内存。
        login_player_cache_.clear();
    }
    login_player_cache_[identity.account_id] = {
        std::move(identity),
        std::chrono::steady_clock::now() + kLoginPlayerCacheTtl,
    };
}

ServiceResult<AuthBundle> AccountService::register_account(const RegisterInput& input) const {
    MF_PROFILE_SCOPE("Auth register account");
    if (const auto issue = validate_login_name_issue(input.login_name)) {
        return make_error_result(make_error(boost::beast::http::status::bad_request, issue->code, issue->message, issue->details));
    }
    if (const auto issue = validate_display_name_issue(input.display_name)) {
        return make_error_result(make_error(boost::beast::http::status::bad_request, issue->code, issue->message, issue->details));
    }
    if (const auto issue = validate_password_issue(input.password)) {
        return make_error_result(make_error(boost::beast::http::status::bad_request, issue->code, issue->message, issue->details));
    }
    if (const auto issue = validate_email_issue(input.email)) {
        return make_error_result(make_error(boost::beast::http::status::bad_request, issue->code, issue->message, issue->details));
    }
    if (const auto issue = validate_phone_number_issue(input.phone_number)) {
        return make_error_result(make_error(boost::beast::http::status::bad_request, issue->code, issue->message, issue->details));
    }
    const auto register_risk = auth_security_->record_registration_attempt(
        input.login_name,
        input.client_ip,
        register_rate_window_,
        register_max_per_login_ip_,
        register_max_per_ip_);
    if (register_risk.rate_limited) {
        return make_error_result(make_error(
            boost::beast::http::status::too_many_requests,
            "register_rate_limited",
            "注册请求过于频繁，请稍后再试",
            {{"retry_after_seconds", register_risk.retry_after_seconds},
             {"max_per_login_ip", register_max_per_login_ip_},
             {"max_per_ip", register_max_per_ip_}}));
    }
    if (auth_security_->is_login_blacklisted(input.login_name, input.client_ip)) {
        return make_error_result(make_error(
            boost::beast::http::status::forbidden,
            "auth_blocked",
            "当前对象已被限制注册"));
    }

    auto password_future = password_work_dispatcher_
        ->try_submit([this, password = input.password] {
            return password_hasher_->hash_password(password);
        });
    if (!password_future) {
        return make_error_result(make_error(
            boost::beast::http::status::service_unavailable,
            "password_queue_full",
            "当前注册请求过多，请稍后再试"));
    }
    const auto password_hash = password_future->get();
    const auto account = accounts_->create_password_account(
        input.login_name,
        input.display_name,
        input.email,
        input.phone_number,
        input.platform_channel.empty() ? "native" : input.platform_channel,
        input.register_source.empty() ? "direct_register" : input.register_source,
        password_hash,
        password_hasher_->scheme_name());
    if (!account) {
        return make_error_result(make_error(
            boost::beast::http::status::conflict,
            "login_name_taken",
            "登录名已存在"));
    }
    auth_security_->clear_registration_attempts(input.login_name, input.client_ip);

    // 注册时账号刚刚创建，不可能存在旧会话，因此不需要 single_session 清理。
    // 这个开关会触发 Redis 读取/删除旧会话；在注册热路径里属于无效 I/O。
    const auto profile = players_->create_for_registration(*account);
    const auto session = token_service_->issue(
        account->account_id,
        profile.player_id,
        profile.nickname,
        mf::game::SessionIssueOptions{
            .single_session = false,
            .client_ip = input.client_ip,
            .user_agent = input.user_agent,
        });

    // 注册成功后的最后登录时间/IP只是审计字段，不是签发会话的前置条件。
    // 异步写入可以减少一次注册请求对 MySQL 连接的占用；队列满时同步回退，
    // 避免高峰期静默丢失审计数据。
    const auto account_id = account->account_id;
    const auto client_ip = input.client_ip;
    const auto audit_key = account_id + "\n" + client_ip;
    const auto audit_accounts = accounts_;
    const auto audit_task = [audit_accounts, account_id, client_ip] {
        audit_accounts->record_login_success_without_fetch(account_id, client_ip);
    };
    if (!login_audit_dispatcher_->try_enqueue(audit_key, audit_task)) {
        audit_task();
    }

    auto response_account = *account;
    response_account.last_login_ip = input.client_ip;
    response_account.last_login_at = std::chrono::system_clock::now();
    return ServiceResult<AuthBundle>{
        .value = AuthBundle{
            // 不再执行“UPDATE 后 SELECT”。注册需要的账号字段已经在本地对象中。
            .account = std::move(response_account),
            .player = profile,
            .session = session,
        },
    };
}

ServiceResult<AuthBundle> AccountService::login(const LoginInput& input) const {
    MF_PROFILE_SCOPE("Auth login");
    if (const auto issue = validate_login_name_issue(input.login_name)) {
        return make_error_result(make_error(boost::beast::http::status::bad_request, issue->code, issue->message, issue->details));
    }
    if (input.password.empty()) {
        return make_error_result(make_error(
            boost::beast::http::status::bad_request,
            "password_required",
            "必须填写密码",
            {{"field", "password"}, {"field_label", "密码"}}));
    }
    if (input.password.size() > 128) {
        return make_error_result(make_error(
            boost::beast::http::status::bad_request,
            "invalid_password_length",
            "密码长度不能超过 128 个字符",
            {{"field", "password"}, {"field_label", "密码"}, {"max_length", 128}, {"actual_length", static_cast<std::int64_t>(input.password.size())}}));
    }
    auto risk = auth_security_->inspect_login_access(input.login_name, input.client_ip);
    if (risk.blacklisted) {
        return make_error_result(make_error(
            boost::beast::http::status::forbidden,
            "auth_blocked",
            "当前对象已被限制登录"));
    }

    if (risk.rate_limited) {
        return make_error_result(make_error(
            boost::beast::http::status::too_many_requests,
            "too_many_attempts",
            "登录过于频繁，请稍后再试",
            {{"retry_after_seconds", risk.retry_after_seconds}}));
    }
    if (risk.captcha_required) {
        if (input.captcha_id.empty() || input.captcha_answer.empty()) {
            return make_error_result(make_error(
                boost::beast::http::status::forbidden,
                "captcha_required",
                "登录前需要先完成验证码校验"));
        }
        if (!auth_security_->verify_and_consume_captcha(input.captcha_id, input.captcha_answer)) {
            return make_error_result(make_error(
                boost::beast::http::status::forbidden,
                "captcha_invalid",
                "验证码答案不正确"));
        }
    }

    auto auth_record = find_cached_password_account(input.login_name);
    if (!auth_record) {
        auth_record = accounts_->find_password_account_by_login(input.login_name);
        if (auth_record) {
            cache_password_account(input.login_name, *auth_record);
        }
    }
    if (!auth_record) {
        auth_security_->record_login_failure(
            input.login_name,
            input.client_ip,
            std::chrono::minutes{15},
            3,
            5,
            std::chrono::minutes{15});
        return make_error_result(make_error(
            boost::beast::http::status::unauthorized,
            "invalid_login",
            "登录名或密码错误"));
    }

    if (auth_record->account.status == mf::game::AccountStatus::frozen) {
        return make_error_result(make_error(
            boost::beast::http::status::forbidden,
            "account_frozen",
            auth_record->account.status_reason.empty() ? "账号已被冻结" : auth_record->account.status_reason));
    }
    if (auth_record->account.status == mf::game::AccountStatus::banned) {
        return make_error_result(make_error(
            boost::beast::http::status::forbidden,
            "account_banned",
            auth_record->account.status_reason.empty() ? "账号已被封禁" : auth_record->account.status_reason));
    }
    if (is_locked(auth_record->account)) {
        const auto retry_after = std::chrono::duration_cast<std::chrono::seconds>(
                                     *auth_record->account.locked_until - std::chrono::system_clock::now())
                                     .count();
        return make_error_result(make_error(
            boost::beast::http::status::too_many_requests,
            "account_locked",
            "账号已被临时锁定",
            {{"retry_after_seconds", std::max<std::int64_t>(0, retry_after)}}));
    }

    bool password_matches = false;
    if (PasswordHasher::is_supported_scheme(auth_record->password_scheme) &&
        password_hasher_->has_cached_success(input.password, auth_record->password_hash)) {
        // 成功校验缓存命中时直接放行，不再排队等待 Argon2id worker。
        password_matches = true;
    } else {
        auto password_future = PasswordHasher::is_supported_scheme(auth_record->password_scheme)
            ? password_work_dispatcher_
                ->try_submit([this, password = input.password, encoded_hash = auth_record->password_hash] {
                    return password_hasher_->verify_password(password, encoded_hash);
                })
            : std::nullopt;
        if (!password_future) {
            return make_error_result(make_error(
                boost::beast::http::status::service_unavailable,
                "password_queue_full",
                "当前登录请求过多，请稍后再试"));
        }
        password_matches = password_future->get();
    }
    if (!password_matches) {
        // 失败登录可能触发账号锁定，不能继续使用旧的状态缓存。
        clear_password_account_cache();
        accounts_->record_login_failure(auth_record->account.account_id, 5, std::chrono::minutes{15});
        auth_security_->record_login_failure(
            input.login_name,
            input.client_ip,
            std::chrono::minutes{15},
            3,
            5,
            std::chrono::minutes{15});
        return make_error_result(make_error(
            boost::beast::http::status::unauthorized,
            "invalid_login",
            "登录名或密码错误"));
    }

    if (risk.failure_count > 0) {
        auth_security_->clear_login_failures(input.login_name, input.client_ip);
    }
    // 登录查询已经拿到了完整账号记录。成功登录只需更新数据库中的审计字段，
    // 不再执行“UPDATE 后 SELECT”，并在本地同步同一份对象，避免额外阻塞 I/O。
    auto account = auth_record->account;
    // 有失败记录时同步清理账号锁定状态，避免异步写入和失败登录并发时产生顺序问题。
    // 普通成功登录只把最后登录审计字段放入后台队列，不阻塞登录响应。
    const auto account_id = account.account_id;
    const auto client_ip = input.client_ip;
    const auto audit_key = account_id + "\n" + client_ip;
    const auto audit_accounts = accounts_;
    const auto audit_task = [audit_accounts, account_id, client_ip] {
        audit_accounts->record_login_success_without_fetch(account_id, client_ip);
    };
    if (risk.failure_count > 0 || !login_audit_dispatcher_->try_enqueue(audit_key, audit_task)) {
        audit_task();
    }
    const auto login_time = std::chrono::system_clock::now();
    account.failed_login_attempts = 0;
    account.locked_until.reset();
    account.last_login_ip = input.client_ip;
    account.last_login_at = login_time;
    account.updated_at = login_time;

    // 成功登录会更新 last_login_* 和锁定字段；更新缓存副本而不是清空，
    // 让同一登录高峰继续复用密码哈希查询结果。
    auth_record->account = account;
    cache_password_account(input.login_name, *auth_record);
    auto profile = find_cached_login_player(account.account_id, account.display_name);
    if (!profile) {
        // 密码查询已经顺便拿到了 player_id 时，登录不再额外回源 player_profile。
        // 历史上没有角色的账号仍回退到下面的兼容初始化流程。
        if (auth_record->player_id) {
            profile = mf::game::PlayerProfile{
                .player_id = *auth_record->player_id,
                .account_id = account.account_id,
                .nickname = account.display_name,
            };
            cache_login_player(*profile);
        }
    }
    if (!profile) {
        // 多个登录请求同时到达时，只有第一个请求访问 MySQL；
        // 其他请求拿锁后再次检查缓存，避免冷启动瞬间形成查询风暴。
        const auto lock_shard = std::hash<std::string>{}(account.account_id) %
                                kLoginPlayerCacheLockShardCount;
        std::scoped_lock fill_lock{login_player_fill_mutexes_[lock_shard]};
        profile = find_cached_login_player(account.account_id, account.display_name);
        if (!profile) {
            profile = players_->upsert_for_login(account);
            cache_login_player(*profile);
        }
    }
    const auto session = token_service_->issue(
        account.account_id,
        profile->player_id,
        profile->nickname,
        mf::game::SessionIssueOptions{
            .single_session = input.single_session,
            .client_ip = input.client_ip,
            .user_agent = input.user_agent,
        });

    return ServiceResult<AuthBundle>{
        .value = AuthBundle{
            .account = account,
            .player = *profile,
            .session = session,
        },
    };
}

ServiceResult<AuthBundle> AccountService::quick_login(
    std::string login_name,
    std::string display_name,
    std::string client_ip,
    std::string user_agent) const {
    if (const auto issue = validate_login_name_issue(login_name)) {
        return make_error_result(make_error(boost::beast::http::status::bad_request, issue->code, issue->message, issue->details));
    }
    if (display_name.empty()) {
        // quick-login 允许前端只传 login_name，服务端这里补一层兜底，
        // 避免不同客户端实现不一致时把联调接口打成 500。
        display_name = login_name;
    }
    if (const auto issue = validate_display_name_issue(display_name)) {
        return make_error_result(make_error(boost::beast::http::status::bad_request, issue->code, issue->message, issue->details));
    }
    const auto account = accounts_->upsert_quick_account(std::move(login_name), std::move(display_name));
    if (!account) {
        return make_error_result(make_error(
            boost::beast::http::status::conflict,
            "quick_login_blocked",
            "该登录名已被正式账号占用，不能快速登录"));
    }

    const auto profile = players_->upsert_for_login(*account);
    const auto session = token_service_->issue(
        account->account_id,
        profile.player_id,
        profile.nickname,
        mf::game::SessionIssueOptions{
            .single_session = false,
            .client_ip = std::move(client_ip),
            .user_agent = std::move(user_agent),
        });
    return ServiceResult<AuthBundle>{
        .value = AuthBundle{
            .account = *account,
            .player = profile,
            .session = session,
        },
    };
}

ServiceResult<mf::game::Account> AccountService::change_password(const ChangePasswordInput& input) const {
    if (input.current_password.empty()) {
        return make_account_error_result(make_error(
            boost::beast::http::status::bad_request,
            "current_password_required",
            "必须填写当前密码",
            {{"field", "current_password"}, {"field_label", "当前密码"}}));
    }
    if (const auto issue = validate_password_issue(input.new_password)) {
        return make_account_error_result(make_error(boost::beast::http::status::bad_request, issue->code, issue->message, issue->details));
    }

    const auto account = accounts_->find_by_account_id(input.account_id);
    if (!account) {
        return make_account_error_result(make_error(
            boost::beast::http::status::not_found,
            "account_not_found",
            "账号不存在"));
    }
    const auto auth_record = accounts_->find_password_account_by_login(account->login_name);
    if (!auth_record) {
        return make_account_error_result(make_error(
            boost::beast::http::status::unauthorized,
            "invalid_password",
            "当前密码不正确"));
    }
    auto password_future = password_work_dispatcher_
        ->try_submit([this, password = input.current_password, encoded_hash = auth_record->password_hash] {
                return password_hasher_->verify_password(password, encoded_hash);
            });
    if (!password_future) {
        return make_account_error_result(make_error(
            boost::beast::http::status::service_unavailable,
            "password_queue_full",
            "当前密码校验请求过多，请稍后再试"));
    }
    const auto password_matches = password_future->get();
    if (!password_matches) {
        return make_account_error_result(make_error(
            boost::beast::http::status::unauthorized,
            "invalid_password",
            "当前密码不正确"));
    }

    auto new_password_future = password_work_dispatcher_
        ->try_submit([this, password = input.new_password] {
            return password_hasher_->hash_password(password);
        });
    if (!new_password_future) {
        return make_account_error_result(make_error(
            boost::beast::http::status::service_unavailable,
            "password_queue_full",
            "当前改密请求过多，请稍后再试"));
    }
    const auto password_hash = new_password_future->get();
    const auto updated = accounts_->update_password_credentials(
            input.account_id,
            password_hash,
            password_hasher_->scheme_name());
    clear_password_account_cache();
    return ServiceResult<mf::game::Account>{.value = updated};
}

ServiceResult<mf::game::PasswordResetTicket> AccountService::forgot_password(const ForgotPasswordInput& input) const {
    if (const auto issue = validate_login_name_issue(input.login_name)) {
        return make_reset_error_result(make_error(boost::beast::http::status::bad_request, issue->code, issue->message, issue->details));
    }
    if (const auto issue = validate_email_issue(input.email)) {
        return make_reset_error_result(make_error(boost::beast::http::status::bad_request, issue->code, issue->message, issue->details));
    }
    const auto ticket = accounts_->create_password_reset_ticket(
        input.login_name,
        input.email,
        "reset_" + random_token(),
        std::chrono::system_clock::now() + std::chrono::minutes{15});
    if (!ticket) {
        return make_reset_error_result(make_error(
            boost::beast::http::status::not_found,
            "reset_target_not_found",
            "账号不存在，或邮箱与账号不匹配"));
    }
    return ServiceResult<mf::game::PasswordResetTicket>{.value = ticket};
}

ServiceResult<mf::game::Account> AccountService::reset_password(const ResetPasswordInput& input) const {
    if (input.reset_token.empty()) {
        return make_account_error_result(make_error(
            boost::beast::http::status::bad_request,
            "reset_token_required",
            "必须填写重置令牌",
            {{"field", "reset_token"}, {"field_label", "重置令牌"}}));
    }
    if (const auto issue = validate_password_issue(input.new_password)) {
        return make_account_error_result(make_error(boost::beast::http::status::bad_request, issue->code, issue->message, issue->details));
    }

    auto password_future = password_work_dispatcher_
        ->try_submit([this, password = input.new_password] {
            return password_hasher_->hash_password(password);
        });
    if (!password_future) {
        return make_account_error_result(make_error(
            boost::beast::http::status::service_unavailable,
            "password_queue_full",
            "当前找回密码请求过多，请稍后再试"));
    }
    const auto password_hash = password_future->get();
    const auto account = accounts_->reset_password_with_ticket(
        input.reset_token,
        password_hash,
        password_hasher_->scheme_name());
    if (!account) {
        return make_account_error_result(make_error(
            boost::beast::http::status::bad_request,
            "reset_token_invalid",
            "重置令牌无效或已过期"));
    }
    clear_password_account_cache();
    return ServiceResult<mf::game::Account>{.value = account};
}

ServiceResult<mf::game::Account> AccountService::set_account_status(
    const std::string& account_id,
    mf::game::AccountStatus status,
    std::string reason) const {
    const auto account = accounts_->set_account_status(account_id, status, std::move(reason));
    if (!account) {
        return make_account_error_result(make_error(
            boost::beast::http::status::not_found,
            "account_not_found",
            "账号不存在"));
    }
    clear_password_account_cache();
    return ServiceResult<mf::game::Account>{.value = account};
}

ServiceResult<mf::game::BlacklistEntry> AccountService::add_blacklist(
    std::string subject_type,
    std::string subject_value,
    std::string reason,
    std::optional<std::chrono::system_clock::time_point> expires_at) const {
    if (subject_type.empty() || subject_value.empty()) {
        return make_blacklist_error_result(make_error(
            boost::beast::http::status::bad_request,
            "invalid_blacklist_subject",
            "subject_type 和 subject_value 不能为空"));
    }
    return ServiceResult<mf::game::BlacklistEntry>{
        .value = auth_security_->add_blacklist_entry(
            std::move(subject_type),
            std::move(subject_value),
            std::move(reason),
            expires_at),
    };
}

mf::game::CaptchaChallenge AccountService::issue_captcha() const {
    std::random_device device;
    std::mt19937 generator{device()};
    std::uniform_int_distribution<int> distribution(1, 9);
    const auto left = distribution(generator);
    const auto right = distribution(generator);
    return auth_security_->create_captcha(
        make_captcha_prompt(left, right),
        std::to_string(left + right),
        std::chrono::system_clock::now() + std::chrono::minutes{5});
}

bool AccountService::is_valid_login_name(std::string_view login_name) {
    return !validate_login_name_issue(login_name).has_value();
}

bool AccountService::is_valid_password(std::string_view password) {
    return !validate_password_issue(password).has_value();
}

bool AccountService::is_valid_display_name(std::string_view display_name) {
    return !validate_display_name_issue(display_name).has_value();
}

std::string AccountService::make_captcha_prompt(int left, int right) {
    return std::to_string(left) + " + " + std::to_string(right) + " = ?";
}

ServiceResult<AuthBundle> AccountService::make_error_result(mf::core::ApiError error) {
    return ServiceResult<AuthBundle>{
        .value = std::nullopt,
        .error = std::move(error),
    };
}

ServiceResult<mf::game::Account> AccountService::make_account_error_result(mf::core::ApiError error) {
    return ServiceResult<mf::game::Account>{
        .value = std::nullopt,
        .error = std::move(error),
    };
}

ServiceResult<mf::game::PasswordResetTicket> AccountService::make_reset_error_result(mf::core::ApiError error) {
    return ServiceResult<mf::game::PasswordResetTicket>{
        .value = std::nullopt,
        .error = std::move(error),
    };
}

ServiceResult<mf::game::BlacklistEntry> AccountService::make_blacklist_error_result(mf::core::ApiError error) {
    return ServiceResult<mf::game::BlacklistEntry>{
        .value = std::nullopt,
        .error = std::move(error),
    };
}

}  // namespace mf::auth 命名空间
