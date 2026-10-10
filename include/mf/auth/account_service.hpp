#pragma once

// 文件职责：
// 聚合账号注册、登录、改密、找回、风控、会话策略等跨仓储流程。
// 调用流程：
// HTTP handler 把请求字段整理成 RegisterInput/LoginInput/... ->
// AccountService 负责串起 AccountRepository、PlayerRepository、PasswordHasher、TokenService、AuthSecurityRepository ->
// 返回统一的成功结果或可直接映射到 API 的错误对象。

#include "mf/auth/password_hasher.hpp"
#include "mf/auth/token_service.hpp"
#include "mf/core/api.hpp"
#include "mf/core/metrics.hpp"
#include "mf/game/repositories.hpp"

#include <optional>
#include <array>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace mf::auth {

template <class T>
struct ServiceResult {
    std::optional<T> value;
    std::optional<mf::core::ApiError> error;
};

struct AuthBundle {
    mf::game::Account account;
    mf::game::PlayerProfile player;
    mf::game::Session session;
};

struct RegisterInput {
    std::string login_name;
    std::string display_name;
    std::string email;
    std::string phone_number;
    std::string platform_channel;
    std::string register_source;
    std::string password;
    std::string client_ip;
    std::string user_agent;
};

struct LoginInput {
    std::string login_name;
    std::string password;
    std::string client_ip;
    std::string user_agent;
    std::string captcha_id;
    std::string captcha_answer;
    // 普通登录默认允许多端会话，避免登录热路径执行旧会话清理；
    // 需要单端策略时由客户端显式传 true。
    bool single_session = false;
};

struct ChangePasswordInput {
    std::string account_id;
    std::string current_password;
    std::string new_password;
};

struct ForgotPasswordInput {
    std::string login_name;
    std::string email;
};

struct ResetPasswordInput {
    std::string reset_token;
    std::string new_password;
};

class AccountService {
public:
    AccountService(
        std::shared_ptr<mf::game::AccountRepository> accounts,
        std::shared_ptr<mf::game::PlayerRepository> players,
        std::shared_ptr<mf::game::AuthSecurityRepository> auth_security,
        std::shared_ptr<PasswordHasher> password_hasher,
        std::shared_ptr<TokenService> token_service,
        std::size_t password_hash_threads = 0,
        std::size_t password_hash_max_pending = 0,
        std::chrono::seconds register_rate_window = std::chrono::minutes{1},
        std::int64_t register_max_per_login_ip = 3,
        std::int64_t register_max_per_ip = 30,
        std::shared_ptr<mf::core::MetricsRegistry> metrics = nullptr);
    ~AccountService();

    ServiceResult<AuthBundle> register_account(const RegisterInput& input) const;
    ServiceResult<AuthBundle> login(const LoginInput& input) const;
    ServiceResult<AuthBundle> quick_login(
        std::string login_name,
        std::string display_name,
        std::string client_ip,
        std::string user_agent) const;
    ServiceResult<mf::game::Account> change_password(const ChangePasswordInput& input) const;
    ServiceResult<mf::game::PasswordResetTicket> forgot_password(const ForgotPasswordInput& input) const;
    ServiceResult<mf::game::Account> reset_password(const ResetPasswordInput& input) const;
    ServiceResult<mf::game::Account> set_account_status(
        const std::string& account_id,
        mf::game::AccountStatus status,
        std::string reason) const;
    ServiceResult<mf::game::BlacklistEntry> add_blacklist(
        std::string subject_type,
        std::string subject_value,
        std::string reason,
        std::optional<std::chrono::system_clock::time_point> expires_at) const;
    mf::game::CaptchaChallenge issue_captcha() const;

private:
    // 登录审计不是生成会话的必要条件，因此普通成功登录可以先返回，
    // 再由单线程有界队列写入 MySQL；队列满时调用方会回退到同步写入。
    class LoginAuditDispatcher {
    public:
        LoginAuditDispatcher(
            std::size_t max_pending,
            std::shared_ptr<mf::core::MetricsRegistry> metrics);
        ~LoginAuditDispatcher();

        LoginAuditDispatcher(const LoginAuditDispatcher&) = delete;
        LoginAuditDispatcher& operator=(const LoginAuditDispatcher&) = delete;

        bool try_enqueue(std::string account_id, std::function<void()> task);

    private:
        struct Task {
            std::string account_id;
            std::function<void()> function;
        };

        void run();

        const std::size_t max_pending_;
        std::shared_ptr<mf::core::MetricsRegistry> metrics_;
        std::mutex mutex_;
        std::condition_variable condition_;
        std::deque<Task> tasks_;
        std::unordered_set<std::string> pending_accounts_;
        std::unordered_map<std::string, std::chrono::steady_clock::time_point> last_accepted_at_;
        bool stopping_ = false;
        std::thread worker_;
    };

    // 密码哈希是 CPU 密集型工作，单独放在线程池中，避免注册/登录高峰占满 HTTP 业务线程。
    class PasswordWorkDispatcher {
    public:
        PasswordWorkDispatcher(
            std::size_t worker_count,
            std::size_t max_pending,
            std::shared_ptr<mf::core::MetricsRegistry> metrics);
        ~PasswordWorkDispatcher();
        PasswordWorkDispatcher(const PasswordWorkDispatcher&) = delete;
        PasswordWorkDispatcher& operator=(const PasswordWorkDispatcher&) = delete;

        template <class Fn>
        auto try_submit(Fn&& function) -> std::optional<std::future<std::invoke_result_t<Fn>>> {
            using Result = std::invoke_result_t<Fn>;
            auto promise = std::make_shared<std::promise<Result>>();
            auto future = promise->get_future();
            if (!try_enqueue([promise, function = std::forward<Fn>(function)]() mutable {
                try {
                    if constexpr (std::is_void_v<Result>) {
                        function();
                        promise->set_value();
                    } else {
                        promise->set_value(function());
                    }
                } catch (...) {
                    promise->set_exception(std::current_exception());
                }
            })) {
                return std::nullopt;
            }
            return future;
        }

    private:
        struct Task {
            std::function<void()> function;
            std::chrono::steady_clock::time_point enqueued_at;
        };

        bool try_enqueue(std::function<void()> task);
        void run();
        std::mutex mutex_;
        std::condition_variable condition_;
        std::deque<Task> tasks_;
        const std::size_t max_pending_;
        std::shared_ptr<mf::core::MetricsRegistry> metrics_;
        bool stopping_ = false;
        std::vector<std::thread> workers_;
    };

    static bool is_valid_login_name(std::string_view login_name);
    static bool is_valid_password(std::string_view password);
    static bool is_valid_display_name(std::string_view display_name);
    static std::string make_captcha_prompt(int left, int right);
    static ServiceResult<AuthBundle> make_error_result(mf::core::ApiError error);
    static ServiceResult<mf::game::Account> make_account_error_result(mf::core::ApiError error);
    static ServiceResult<mf::game::PasswordResetTicket> make_reset_error_result(mf::core::ApiError error);
    static ServiceResult<mf::game::BlacklistEntry> make_blacklist_error_result(mf::core::ApiError error);

    // 认证记录只做很短时间的进程内缓存，减少登录高峰期重复读取 MySQL。
    // 缓存不保存明文密码；账号状态、改密、封禁等变更会主动清空它。
    std::optional<mf::game::PasswordAccountRecord> find_cached_password_account(
        const std::string& login_name) const;
    void cache_password_account(
        const std::string& login_name,
        const mf::game::PasswordAccountRecord& account) const;
    void clear_password_account_cache() const;

    // 只缓存账号到玩家的身份映射，不缓存金币、宝石或背包，避免资产数据过期。
    std::optional<mf::game::PlayerProfile> find_cached_login_player(
        const std::string& account_id,
        const std::string& nickname) const;
    void cache_login_player(const mf::game::PlayerProfile& player) const;

    std::shared_ptr<mf::game::AccountRepository> accounts_;
    std::shared_ptr<mf::game::PlayerRepository> players_;
    std::shared_ptr<mf::game::AuthSecurityRepository> auth_security_;
    std::shared_ptr<PasswordHasher> password_hasher_;
    std::shared_ptr<TokenService> token_service_;
    std::shared_ptr<LoginAuditDispatcher> login_audit_dispatcher_;
    std::shared_ptr<PasswordWorkDispatcher> password_work_dispatcher_;
    std::chrono::seconds register_rate_window_;
    std::int64_t register_max_per_login_ip_;
    std::int64_t register_max_per_ip_;
    mutable std::mutex password_account_cache_mutex_;
    // 玩家身份缓存与密码账号缓存分开加锁，避免读玩家映射阻塞密码缓存访问。
    mutable std::mutex login_player_cache_mutex_;
    // 只保护同一账号首次填充玩家身份缓存的短路径；缓存命中时不会获取锁。
    // 分片后不同账号可以并行回源 MySQL，只有同一账号的并发冷启动会互相合并。
    static constexpr std::size_t kLoginPlayerCacheLockShardCount = 32;
    mutable std::array<std::mutex, kLoginPlayerCacheLockShardCount> login_player_fill_mutexes_;
    mutable std::unordered_map<
        std::string,
        std::pair<mf::game::PasswordAccountRecord, std::chrono::steady_clock::time_point>>
        password_account_cache_;
    mutable std::unordered_map<
        std::string,
        std::pair<mf::game::PlayerProfile, std::chrono::steady_clock::time_point>>
        login_player_cache_;
};

}  // namespace mf::auth 命名空间
