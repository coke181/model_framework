#include "mf/agent/repository.hpp"
#include "mf/game/repositories.hpp"
#include "mf/multiplayer/repository.hpp"
#include "mf/voice/repository.hpp"
#include "mf/core/profiling.hpp"

// 文件职责：
// 提供所有仓储接口的内存版实现，供当前示例在无数据库时跑通完整业务链路。
// 调用流程：
// main() 调用 make_in_memory_repositories() ->
// 得到 Account/Player/Friend/Session/Chat 五类仓储实现 ->
// 登录、好友、聊天接口全部通过这些仓储读写内存状态。

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <random>
#include <shared_mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace mf::game {
namespace {

std::int64_t to_unix_seconds(std::chrono::system_clock::time_point time_point) {
    return std::chrono::duration_cast<std::chrono::seconds>(time_point.time_since_epoch()).count();
}

class InMemoryAgentRepository final : public mf::agent::AgentRepository {
public:
    mf::agent::AgentConversation save_conversation(mf::agent::AgentConversation conversation) override {
        std::scoped_lock lock{mutex_};
        const auto now = std::chrono::system_clock::now();
        if (const auto it = conversations_.find(conversation.conversation_id); it != conversations_.end()) {
            conversation.created_at = it->second.created_at;
        } else {
            conversation.created_at = now;
        }
        conversation.updated_at = now;
        conversations_[conversation.conversation_id] = conversation;
        return conversation;
    }

    std::optional<mf::agent::AgentConversation> find_conversation(
        const std::string& conversation_id,
        const std::string& player_id) const override {
        std::scoped_lock lock{mutex_};
        const auto it = conversations_.find(conversation_id);
        if (it != conversations_.end() && it->second.player_id == player_id && it->second.status == "active") {
            return it->second;
        }
        return std::nullopt;
    }

    bool delete_conversation(const std::string& conversation_id, const std::string& player_id) override {
        std::scoped_lock lock{mutex_};
        const auto it = conversations_.find(conversation_id);
        if (it == conversations_.end() || it->second.player_id != player_id || it->second.status == "deleted") return false;
        it->second.status = "deleted";
        it->second.updated_at = std::chrono::system_clock::now();
        return true;
    }

    mf::agent::AgentMessage append_message(mf::agent::AgentMessage message) override {
        std::scoped_lock lock{mutex_};
        message.created_at = std::chrono::system_clock::now();
        messages_[message.message_id] = message;
        return message;
    }

    std::vector<mf::agent::AgentMessage> list_messages(
        const std::string& conversation_id,
        const std::string& player_id,
        std::size_t limit) const override {
        std::scoped_lock lock{mutex_};
        std::vector<mf::agent::AgentMessage> values;
        for (const auto& [id, message] : messages_) {
            (void)id;
            if (message.conversation_id == conversation_id && message.player_id == player_id) values.push_back(message);
        }
        std::sort(values.begin(), values.end(), [](const auto& left, const auto& right) {
            return left.created_at.value_or(std::chrono::system_clock::time_point{}) <
                   right.created_at.value_or(std::chrono::system_clock::time_point{});
        });
        if (values.size() > limit) values.erase(values.begin(), values.end() - static_cast<std::ptrdiff_t>(limit));
        return values;
    }

    std::optional<mf::agent::AgentMessage> find_assistant_message_by_request(
        const std::string& player_id,
        const std::string& client_request_id) const override {
        std::scoped_lock lock{mutex_};
        for (const auto& [id, message] : messages_) {
            (void)id;
            const auto conversation = conversations_.find(message.conversation_id);
            if (message.player_id == player_id && message.client_request_id == client_request_id &&
                message.role == "assistant" && conversation != conversations_.end() &&
                conversation->second.status == "active") {
                return message;
            }
        }
        return std::nullopt;
    }

    mf::agent::AgentToolCall save_tool_call(mf::agent::AgentToolCall tool_call) override {
        std::scoped_lock lock{mutex_};
        if (const auto it = tool_calls_.find(tool_call.call_id); it != tool_calls_.end()) {
            tool_call.created_at = it->second.created_at;
        } else {
            tool_call.created_at = std::chrono::system_clock::now();
        }
        tool_calls_[tool_call.call_id] = tool_call;
        return tool_call;
    }

    std::optional<mf::agent::AgentToolCall> find_tool_call(
        const std::string& call_id,
        const std::string& conversation_id,
        const std::string& player_id) const override {
        std::scoped_lock lock{mutex_};
        const auto it = tool_calls_.find(call_id);
        const auto conversation = conversations_.find(conversation_id);
        if (it != tool_calls_.end() && it->second.conversation_id == conversation_id &&
            it->second.player_id == player_id && conversation != conversations_.end() &&
            conversation->second.status == "active") {
            return it->second;
        }
        return std::nullopt;
    }

    std::vector<mf::agent::AgentToolCall> list_tool_calls_for_message(const std::string& message_id) const override {
        std::scoped_lock lock{mutex_};
        std::vector<mf::agent::AgentToolCall> values;
        for (const auto& [id, call] : tool_calls_) {
            (void)id;
            if (call.message_id == message_id) values.push_back(call);
        }
        return values;
    }

    mf::agent::AgentUsageDaily get_daily_usage(
        const std::string& player_id,
        const std::string& usage_date) const override {
        std::scoped_lock lock{mutex_};
        if (const auto it = usage_.find(player_id + '|' + usage_date); it != usage_.end()) return it->second;
        return {.player_id = player_id, .usage_date = usage_date};
    }

    mf::agent::AgentUsageDaily record_usage(
        const std::string& player_id,
        const std::string& usage_date,
        std::int64_t prompt_tokens,
        std::int64_t completion_tokens,
        bool failed) override {
        std::scoped_lock lock{mutex_};
        auto& usage = usage_[player_id + '|' + usage_date];
        usage.player_id = player_id;
        usage.usage_date = usage_date;
        ++usage.request_count;
        usage.prompt_tokens += prompt_tokens;
        usage.completion_tokens += completion_tokens;
        if (failed) ++usage.failed_count;
        return usage;
    }

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, mf::agent::AgentConversation> conversations_;
    std::unordered_map<std::string, mf::agent::AgentMessage> messages_;
    std::unordered_map<std::string, mf::agent::AgentToolCall> tool_calls_;
    std::unordered_map<std::string, mf::agent::AgentUsageDaily> usage_;
};

std::size_t next_message_id = 1;
std::size_t next_session_id = 1;
std::size_t next_blacklist_id = 1;
std::size_t next_reward_grant_id = 1;
std::size_t next_mail_id = 1;
std::atomic<std::size_t> next_inventory_instance_id{1};

std::string make_random_token() {
    std::random_device device;
    std::mt19937_64 generator{device()};
    std::uniform_int_distribution<unsigned long long> distribution;

    std::ostringstream output;
    output << std::hex << distribution(generator) << distribution(generator);
    return output.str();
}

std::vector<InventoryItem> default_inventory() {
    return {
        InventoryItem{
            .instance_id = "item_inst_" + std::to_string(next_inventory_instance_id.fetch_add(1, std::memory_order_relaxed)),
            .item_id = "starter_sword",
            .quantity = 1,
        },
        InventoryItem{
            .instance_id = "item_inst_" + std::to_string(next_inventory_instance_id.fetch_add(1, std::memory_order_relaxed)),
            .item_id = "starter_potion",
            .quantity = 1,
        },
    };
}

class InMemoryAccountRepository final : public AccountRepository {
public:
    std::optional<Account> upsert_quick_account(std::string login_name, std::string display_name) override {
        std::scoped_lock lock{mutex_};

        // login name 被视为外部身份主键。
        // 再次登录时只更新展示信息，不更换 account id，这符合常见游戏账号流程。
        if (const auto it = account_ids_by_login_.find(login_name); it != account_ids_by_login_.end()) {
            auto& account = accounts_by_id_.at(it->second);
            if (account.auth_mode != AccountAuthMode::quick) {
                return std::nullopt;
            }
            account.display_name = std::move(display_name);
            return account;
        }

        Account account{
            .account_id = "acc_" + std::to_string(next_id_++),
            .login_name = std::move(login_name),
            .display_name = std::move(display_name),
            .auth_mode = AccountAuthMode::quick,
            .created_at = std::chrono::system_clock::now(),
            .updated_at = std::chrono::system_clock::now(),
        };
        account_ids_by_login_[account.login_name] = account.account_id;
        accounts_by_id_[account.account_id] = account;
        return account;
    }

    std::optional<Account> create_password_account(
        std::string login_name,
        std::string display_name,
        std::string email,
        std::string phone_number,
        std::string platform_channel,
        std::string register_source,
        std::string password_hash,
        std::string password_scheme) override {
        std::scoped_lock lock{mutex_};

        if (account_ids_by_login_.contains(login_name)) {
            return std::nullopt;
        }

        Account account{
            .account_id = "acc_" + std::to_string(next_id_++),
            .login_name = std::move(login_name),
            .display_name = std::move(display_name),
            .email = std::move(email),
            .phone_number = std::move(phone_number),
            .platform_channel = std::move(platform_channel),
            .register_source = std::move(register_source),
            .auth_mode = AccountAuthMode::password,
            .created_at = std::chrono::system_clock::now(),
            .updated_at = std::chrono::system_clock::now(),
        };

        password_accounts_by_login_[account.login_name] = PasswordAccountRecord{
            .account = account,
            .password_hash = std::move(password_hash),
            .password_scheme = std::move(password_scheme),
        };
        account_ids_by_login_[account.login_name] = account.account_id;
        accounts_by_id_[account.account_id] = account;
        return account;
    }

    std::optional<PasswordAccountRecord> find_password_account_by_login(const std::string& login_name) const override {
        std::scoped_lock lock{mutex_};
        if (const auto it = password_accounts_by_login_.find(login_name); it != password_accounts_by_login_.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    std::optional<Account> find_by_login_name(const std::string& login_name) const override {
        std::scoped_lock lock{mutex_};
        if (const auto id_it = account_ids_by_login_.find(login_name); id_it != account_ids_by_login_.end()) {
            return accounts_by_id_.at(id_it->second);
        }
        return std::nullopt;
    }

    std::optional<Account> update_password_credentials(
        const std::string& account_id,
        std::string password_hash,
        std::string password_scheme) override {
        std::scoped_lock lock{mutex_};
        auto it = accounts_by_id_.find(account_id);
        if (it == accounts_by_id_.end()) {
            return std::nullopt;
        }
        it->second.updated_at = std::chrono::system_clock::now();
        it->second.failed_login_attempts = 0;
        it->second.locked_until.reset();

        auto& record = password_accounts_by_login_[it->second.login_name];
        record.account = it->second;
        record.password_hash = std::move(password_hash);
        record.password_scheme = std::move(password_scheme);
        record.reset_token.clear();
        record.reset_token_expires_at.reset();
        return it->second;
    }

    std::optional<PasswordResetTicket> create_password_reset_ticket(
        const std::string& login_name,
        const std::string& email,
        std::string reset_token,
        std::chrono::system_clock::time_point expires_at) override {
        std::scoped_lock lock{mutex_};
        auto it = password_accounts_by_login_.find(login_name);
        if (it == password_accounts_by_login_.end()) {
            return std::nullopt;
        }
        if (!email.empty() && it->second.account.email != email) {
            return std::nullopt;
        }

        it->second.reset_token = reset_token;
        it->second.reset_token_expires_at = expires_at;
        return PasswordResetTicket{
            .account_id = it->second.account.account_id,
            .login_name = it->second.account.login_name,
            .reset_token = std::move(reset_token),
            .expires_at = expires_at,
        };
    }

    std::optional<Account> reset_password_with_ticket(
        const std::string& reset_token,
        std::string password_hash,
        std::string password_scheme) override {
        std::scoped_lock lock{mutex_};
        const auto now = std::chrono::system_clock::now();
        for (auto& [_, record] : password_accounts_by_login_) {
            if (record.reset_token == reset_token &&
                record.reset_token_expires_at &&
                *record.reset_token_expires_at >= now) {
                record.password_hash = std::move(password_hash);
                record.password_scheme = std::move(password_scheme);
                record.reset_token.clear();
                record.reset_token_expires_at.reset();
                auto& account = accounts_by_id_.at(record.account.account_id);
                account.failed_login_attempts = 0;
                account.locked_until.reset();
                account.updated_at = now;
                record.account = account;
                return account;
            }
        }
        return std::nullopt;
    }

    std::optional<Account> set_account_status(
        const std::string& account_id,
        AccountStatus status,
        std::string reason) override {
        std::scoped_lock lock{mutex_};
        auto it = accounts_by_id_.find(account_id);
        if (it == accounts_by_id_.end()) {
            return std::nullopt;
        }
        it->second.status = status;
        it->second.status_reason = std::move(reason);
        it->second.updated_at = std::chrono::system_clock::now();
        if (auto password_it = password_accounts_by_login_.find(it->second.login_name);
            password_it != password_accounts_by_login_.end()) {
            password_it->second.account = it->second;
        }
        return it->second;
    }

    std::optional<Account> record_login_success(
        const std::string& account_id,
        const std::string& client_ip) override {
        std::scoped_lock lock{mutex_};
        auto it = accounts_by_id_.find(account_id);
        if (it == accounts_by_id_.end()) {
            return std::nullopt;
        }
        it->second.failed_login_attempts = 0;
        it->second.locked_until.reset();
        it->second.last_login_ip = client_ip;
        it->second.last_login_at = std::chrono::system_clock::now();
        it->second.updated_at = it->second.last_login_at;
        if (auto password_it = password_accounts_by_login_.find(it->second.login_name);
            password_it != password_accounts_by_login_.end()) {
            password_it->second.account = it->second;
        }
        return it->second;
    }

    std::optional<Account> record_login_failure(
        const std::string& account_id,
        std::int64_t lock_threshold,
        std::chrono::seconds lock_duration) override {
        std::scoped_lock lock{mutex_};
        auto it = accounts_by_id_.find(account_id);
        if (it == accounts_by_id_.end()) {
            return std::nullopt;
        }
        ++it->second.failed_login_attempts;
        if (it->second.failed_login_attempts >= lock_threshold) {
            it->second.locked_until = std::chrono::system_clock::now() + lock_duration;
        }
        it->second.updated_at = std::chrono::system_clock::now();
        if (auto password_it = password_accounts_by_login_.find(it->second.login_name);
            password_it != password_accounts_by_login_.end()) {
            password_it->second.account = it->second;
        }
        return it->second;
    }

    std::optional<Account> find_by_account_id(const std::string& account_id) const override {
        std::scoped_lock lock{mutex_};
        if (const auto it = accounts_by_id_.find(account_id); it != accounts_by_id_.end()) {
            return it->second;
        }
        return std::nullopt;
    }

private:
    mutable std::mutex mutex_;
    // 内存版从 8 位数字开始递增，方便本地联调时直接拿 player_id 加好友。
    std::size_t next_id_ = 10000001;
    std::unordered_map<std::string, std::string> account_ids_by_login_;
    std::unordered_map<std::string, Account> accounts_by_id_;
    std::unordered_map<std::string, PasswordAccountRecord> password_accounts_by_login_;
};

class InMemoryPlayerRepository final : public PlayerRepository {
public:
    PlayerProfile upsert_for_account(const Account& account) override {
        std::scoped_lock account_lock{account_mutex_};

        // 这个起步模型里，一个账号只映射一个玩家资料。
        // 如果以后支持多角色，这个仓储边界就是扩展账号到角色关系的地方。
        if (const auto it = player_ids_by_account_.find(account.account_id); it != player_ids_by_account_.end()) {
            std::unique_lock profile_lock{shard_mutexes_[shard_index(it->second)]};
            auto& profile = profiles_by_id_.at(it->second);
            profile.nickname = account.display_name;
            return profile;
        }

        PlayerProfile profile{
            .player_id = std::to_string(next_id_.fetch_add(1, std::memory_order_relaxed)),
            .account_id = account.account_id,
            .nickname = account.display_name,
            .gold = 1000,
            .gem = 0,
            .currencies = {CurrencyBalance{.currency_id = "gold", .amount = 1000},
                           CurrencyBalance{.currency_id = "gem", .amount = 0}},
            .inventory = default_inventory(),
        };
        player_ids_by_account_[account.account_id] = profile.player_id;
        {
            std::unique_lock profile_lock{shard_mutexes_[shard_index(profile.player_id)]};
            profiles_by_id_[profile.player_id] = profile;
        }
        return profile;
    }

    std::optional<PlayerProfile> find_by_player_id(const std::string& player_id) const override {
        std::shared_lock lock{shard_mutexes_[shard_index(player_id)]};
        if (const auto it = profiles_by_id_.find(player_id); it != profiles_by_id_.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    std::vector<PlayerSummary> find_summaries_by_player_ids(
        const std::vector<std::string>& player_ids) const override {
        std::vector<std::shared_lock<std::shared_mutex>> locks;
        locks.reserve(kShardCount);
        for (std::size_t index = 0; index < kShardCount; ++index) {
            locks.emplace_back(shard_mutexes_[index]);
        }

        std::unordered_set<std::string> wanted;
        wanted.reserve(player_ids.size());
        for (const auto& player_id : player_ids) {
            wanted.insert(player_id);
        }

        std::vector<PlayerSummary> summaries;
        summaries.reserve(player_ids.size());
        for (const auto& [player_id, profile] : profiles_by_id_) {
            if (!wanted.contains(player_id)) {
                continue;
            }
            summaries.push_back(PlayerSummary{
                .player_id = profile.player_id,
                .account_id = profile.account_id,
                .nickname = profile.nickname,
            });
        }
        return summaries;
    }

    std::vector<PlayerProfile> list_all() const override {
        std::vector<std::shared_lock<std::shared_mutex>> locks;
        locks.reserve(kShardCount);
        for (std::size_t index = 0; index < kShardCount; ++index) {
            locks.emplace_back(shard_mutexes_[index]);
        }
        std::vector<PlayerProfile> values;
        values.reserve(profiles_by_id_.size());
        for (const auto& [_, profile] : profiles_by_id_) {
            values.push_back(profile);
        }
        return values;
    }

    std::size_t count_filtered(
        const std::optional<std::string>& player_id_filter,
        const std::optional<std::string>& account_id_filter,
        const std::optional<std::string>& nickname_filter) const override {
        return list_filtered(
                   static_cast<std::size_t>(-1),
                   0,
                   player_id_filter,
                   account_id_filter,
                   nickname_filter)
            .size();
    }

    std::vector<PlayerProfile> list_filtered(
        std::size_t limit,
        std::size_t offset,
        const std::optional<std::string>& player_id_filter,
        const std::optional<std::string>& account_id_filter,
        const std::optional<std::string>& nickname_filter) const override {
        auto items = list_all();
        items.erase(
            std::remove_if(
                items.begin(),
                items.end(),
                [&](const PlayerProfile& profile) {
                    const auto contains = [](const std::string& haystack, const std::optional<std::string>& needle) {
                        return !needle || haystack.find(*needle) != std::string::npos;
                    };
                    return !contains(profile.player_id, player_id_filter) ||
                        !contains(profile.account_id, account_id_filter) ||
                        !contains(profile.nickname, nickname_filter);
                }),
            items.end());

        std::sort(
            items.begin(),
            items.end(),
            [](const PlayerProfile& left, const PlayerProfile& right) {
                return left.player_id < right.player_id;
            });

        const auto bounded_offset = std::min(offset, items.size());
        const auto end = std::min(items.size(), bounded_offset + limit);
        std::vector<PlayerProfile> paged;
        paged.reserve(end - bounded_offset);
        for (std::size_t index = bounded_offset; index < end; ++index) {
            paged.push_back(items[index]);
        }
        return paged;
    }

    PlayerAdminPage list_admin_rows(
        std::size_t limit,
        const std::optional<PlayerPageCursor>& cursor,
        const std::optional<std::string>& player_id_filter,
        const std::optional<std::string>& account_id_filter,
        const std::optional<std::string>& nickname_filter) const override {
        PlayerAdminPage page;
        auto profiles = list_all();
        profiles.erase(
            std::remove_if(
                profiles.begin(),
                profiles.end(),
                [&](const PlayerProfile& profile) {
                    const auto contains = [](const std::string& haystack, const std::optional<std::string>& needle) {
                        return !needle || haystack.find(*needle) != std::string::npos;
                    };
                    return !contains(profile.player_id, player_id_filter) ||
                        !contains(profile.account_id, account_id_filter) ||
                        !contains(profile.nickname, nickname_filter);
                }),
            profiles.end());
        std::sort(
            profiles.begin(),
            profiles.end(),
            [](const PlayerProfile& left, const PlayerProfile& right) {
                return left.player_id < right.player_id;
            });

        auto begin = profiles.begin();
        if (cursor && !cursor->last_player_id.empty()) {
            begin = std::upper_bound(
                profiles.begin(),
                profiles.end(),
                cursor->last_player_id,
                [](const std::string& value, const PlayerProfile& profile) {
                    return value < profile.player_id;
                });
        }

        const auto bounded_limit = std::clamp<std::size_t>(limit, 1, 500);
        const auto remaining = static_cast<std::size_t>(std::distance(begin, profiles.end()));
        const auto count = std::min<std::size_t>(bounded_limit, remaining);
        page.rows.reserve(count);
        for (auto it = begin; it != begin + static_cast<std::ptrdiff_t>(count); ++it) {
            page.rows.push_back(PlayerAdminRow{
                .player_id = it->player_id,
                .account_id = it->account_id,
                .nickname = it->nickname,
                .gold = it->gold,
                .gem = it->gem,
            });
        }
        page.has_more = count < remaining;
        if (page.has_more) {
            page.next_player_id = page.rows.back().player_id;
        }
        return page;
    }

    std::optional<PlayerProfile> apply_rewards(
        const std::string& player_id,
        const std::vector<RewardEntry>& rewards,
        const std::string& grant_id) override {
        std::unique_lock profile_lock{shard_mutexes_[shard_index(player_id)]};
        auto it = profiles_by_id_.find(player_id);
        if (it == profiles_by_id_.end()) {
            return std::nullopt;
        }

        if (!grant_id.empty()) {
            std::scoped_lock grant_lock{grant_mutex_};
            if (!applied_grant_ids_.insert(grant_id).second) {
                return it->second;
            }
        }

        for (const auto& reward : rewards) {
            if (reward.amount <= 0) {
                continue;
            }

            switch (reward.kind) {
            case RewardKind::gold:
                it->second.gold += reward.amount;
                upsert_currency_balance(it->second.currencies, "gold", reward.amount);
                break;
            case RewardKind::currency:
                upsert_currency_balance(it->second.currencies, reward.resource_id, reward.amount);
                if (reward.resource_id == "gold") {
                    it->second.gold += reward.amount;
                } else if (reward.resource_id == "gem") {
                    it->second.gem += reward.amount;
                }
                break;
            case RewardKind::item:
                upsert_inventory_stack(it->second.inventory, reward.resource_id, reward.amount);
                break;
            case RewardKind::attachment_template:
                break;
            }
        }

        return it->second;
    }

    std::optional<PlayerProfile> set_inventory_item_quantity(
        const std::string& player_id,
        const std::string& item_id,
        std::int64_t quantity) override {
        std::unique_lock profile_lock{shard_mutexes_[shard_index(player_id)]};
        auto it = profiles_by_id_.find(player_id);
        if (it == profiles_by_id_.end()) {
            return std::nullopt;
        }

        set_inventory_stack_quantity(it->second.inventory, item_id, quantity);
        return it->second;
    }

    InventoryPage list_inventory_items(
        const std::string& player_id,
        std::size_t limit,
        const std::optional<std::string>& cursor) const override {
        InventoryPage page;
        const auto profile = find_by_player_id(player_id);
        if (!profile) {
            return page;
        }

        auto items = profile->inventory;
        std::sort(
            items.begin(),
            items.end(),
            [](const InventoryItem& left, const InventoryItem& right) {
                return left.item_id < right.item_id;
            });

        auto begin = items.begin();
        if (cursor && !cursor->empty()) {
            begin = std::upper_bound(
                items.begin(),
                items.end(),
                *cursor,
                [](const std::string& value, const InventoryItem& item) {
                    return value < item.item_id;
                });
        }

        const auto bounded_limit = std::clamp<std::size_t>(limit, 1, 500);
        const auto remaining = static_cast<std::size_t>(std::distance(begin, items.end()));
        const auto count = std::min<std::size_t>(bounded_limit, remaining);
        page.items.assign(begin, begin + static_cast<std::ptrdiff_t>(count));
        page.has_more = count < remaining;
        if (page.has_more) {
            page.next_cursor = page.items.back().item_id;
        }
        return page;
    }

    std::optional<PlayerProfile> spend_currency(
        const std::string& player_id,
        const std::string& currency_id,
        std::int64_t amount) override {
        std::unique_lock profile_lock{shard_mutexes_[shard_index(player_id)]};
        auto it = profiles_by_id_.find(player_id);
        if (it == profiles_by_id_.end() || amount < 0) {
            return std::nullopt;
        }
        if (amount == 0) {
            return it->second;
        }

        auto currency_it = std::find_if(
            it->second.currencies.begin(),
            it->second.currencies.end(),
            [&](const CurrencyBalance& balance) { return balance.currency_id == currency_id; });
        if (currency_it == it->second.currencies.end() || currency_it->amount < amount) {
            return std::nullopt;
        }

        currency_it->amount -= amount;
        if (currency_id == "gold") {
            it->second.gold -= amount;
        } else if (currency_id == "gem") {
            it->second.gem -= amount;
        }
        return it->second;
    }

private:
    static std::size_t shard_index(const std::string& player_id) {
        return std::hash<std::string>{}(player_id) % kShardCount;
    }

    static void upsert_currency_balance(
        std::vector<CurrencyBalance>& currencies,
        const std::string& currency_id,
        std::int64_t delta) {
        const auto it = std::find_if(
            currencies.begin(),
            currencies.end(),
            [&](const CurrencyBalance& balance) { return balance.currency_id == currency_id; });
        if (it != currencies.end()) {
            it->amount += delta;
            return;
        }
        currencies.push_back(CurrencyBalance{
            .currency_id = currency_id,
            .amount = delta,
        });
    }

    static void upsert_inventory_stack(
        std::vector<InventoryItem>& inventory,
        const std::string& item_id,
        std::int64_t delta_quantity) {
        const auto it = std::find_if(
            inventory.begin(),
            inventory.end(),
            [&](const InventoryItem& item) { return item.item_id == item_id; });
        if (it != inventory.end()) {
            it->quantity += delta_quantity;
            return;
        }
        inventory.push_back(InventoryItem{
            .instance_id = "item_inst_" + std::to_string(next_inventory_instance_id.fetch_add(1, std::memory_order_relaxed)),
            .item_id = item_id,
            .quantity = delta_quantity,
        });
    }

    static void set_inventory_stack_quantity(
        std::vector<InventoryItem>& inventory,
        const std::string& item_id,
        std::int64_t quantity) {
        const auto it = std::find_if(
            inventory.begin(),
            inventory.end(),
            [&](const InventoryItem& item) { return item.item_id == item_id; });
        if (quantity <= 0) {
            if (it != inventory.end()) {
                inventory.erase(it);
            }
            return;
        }
        if (it != inventory.end()) {
            it->quantity = quantity;
            return;
        }
        inventory.push_back(InventoryItem{
            .instance_id = "item_inst_" + std::to_string(next_inventory_instance_id.fetch_add(1, std::memory_order_relaxed)),
            .item_id = item_id,
            .quantity = quantity,
        });
    }

    static constexpr std::size_t kShardCount = 64;
    mutable std::array<std::shared_mutex, kShardCount> shard_mutexes_;
    mutable std::mutex account_mutex_;
    mutable std::mutex grant_mutex_;
    std::atomic<std::size_t> next_id_{1};
    std::unordered_map<std::string, std::string> player_ids_by_account_;
    std::unordered_map<std::string, PlayerProfile> profiles_by_id_;
    std::unordered_set<std::string> applied_grant_ids_;
};

class InMemoryItemCatalogRepository final : public ItemCatalogRepository {
public:
    ItemDefinition upsert_item(ItemDefinition item) override {
        std::scoped_lock lock{mutex_};
        const auto now = std::chrono::system_clock::now();
        if (const auto it = items_by_id_.find(item.item_id); it != items_by_id_.end()) {
            item.created_at = it->second.created_at;
        } else {
            item.created_at = now;
        }
        item.updated_at = now;
        items_by_id_[item.item_id] = item;
        return item;
    }

    std::optional<ItemDefinition> find_by_item_id(const std::string& item_id) const override {
        std::scoped_lock lock{mutex_};
        const auto it = items_by_id_.find(item_id);
        if (it == items_by_id_.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    std::vector<ItemDefinition> list_items(
        std::size_t limit,
        std::size_t offset,
        const std::optional<std::string>& item_id_filter,
        const std::optional<std::string>& name_filter,
        const std::optional<std::string>& rarity_filter,
        const std::optional<bool>& enabled_filter) const override {
        std::scoped_lock lock{mutex_};
        auto items = filtered_items(item_id_filter, name_filter, rarity_filter, enabled_filter);
        std::sort(items.begin(), items.end(), [](const ItemDefinition& left, const ItemDefinition& right) {
            return left.item_id < right.item_id;
        });
        if (offset >= items.size()) {
            return {};
        }
        const auto end = std::min(items.size(), offset + limit);
        return std::vector<ItemDefinition>(
            items.begin() + static_cast<std::ptrdiff_t>(offset),
            items.begin() + static_cast<std::ptrdiff_t>(end));
    }

    std::size_t count_items(
        const std::optional<std::string>& item_id_filter,
        const std::optional<std::string>& name_filter,
        const std::optional<std::string>& rarity_filter,
        const std::optional<bool>& enabled_filter) const override {
        std::scoped_lock lock{mutex_};
        return filtered_items(item_id_filter, name_filter, rarity_filter, enabled_filter).size();
    }

private:
    std::vector<ItemDefinition> filtered_items(
        const std::optional<std::string>& item_id_filter,
        const std::optional<std::string>& name_filter,
        const std::optional<std::string>& rarity_filter,
        const std::optional<bool>& enabled_filter) const {
        std::vector<ItemDefinition> items;
        for (const auto& [_, item] : items_by_id_) {
            if (item_id_filter && !item_id_filter->empty() && item.item_id.find(*item_id_filter) == std::string::npos) {
                continue;
            }
            if (name_filter && !name_filter->empty() && item.name.find(*name_filter) == std::string::npos) {
                continue;
            }
            if (rarity_filter && !rarity_filter->empty() && item.rarity != *rarity_filter) {
                continue;
            }
            if (enabled_filter && item.enabled != *enabled_filter) {
                continue;
            }
            items.push_back(item);
        }
        return items;
    }

    mutable std::mutex mutex_;
    std::unordered_map<std::string, ItemDefinition> items_by_id_;
};

class InMemoryGachaRepository final : public GachaRepository {
public:
    GachaPool upsert_pool(GachaPool pool) override {
        std::scoped_lock lock{mutex_};
        const auto now = std::chrono::system_clock::now();
        if (const auto it = pools_by_id_.find(pool.pool_id); it != pools_by_id_.end()) {
            pool.created_at = it->second.created_at;
        } else {
            pool.created_at = now;
        }
        pool.updated_at = now;
        pools_by_id_[pool.pool_id] = pool;
        return pool;
    }

    bool delete_pool(const std::string& pool_id) override {
        std::scoped_lock lock{mutex_};
        const auto erased = pools_by_id_.erase(pool_id) > 0;
        items_by_pool_.erase(pool_id);
        for (auto it = states_by_key_.begin(); it != states_by_key_.end();) {
            if (it->second.pool_id == pool_id) {
                it = states_by_key_.erase(it);
            } else {
                ++it;
            }
        }
        return erased;
    }

    std::optional<GachaPool> find_pool(const std::string& pool_id) const override {
        std::scoped_lock lock{mutex_};
        if (const auto it = pools_by_id_.find(pool_id); it != pools_by_id_.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    std::vector<GachaPool> list_pools(bool include_disabled, std::chrono::system_clock::time_point now) const override {
        MF_PROFILE_SCOPE("Memory gacha pools");
        std::scoped_lock lock{mutex_};
        std::vector<GachaPool> pools;
        for (const auto& [_, pool] : pools_by_id_) {
            if (!include_disabled && !pool.enabled) {
                continue;
            }
            if (!include_disabled && pool.starts_at && now < *pool.starts_at) {
                continue;
            }
            if (!include_disabled && pool.ends_at && now > *pool.ends_at) {
                continue;
            }
            pools.push_back(pool);
        }
        std::sort(pools.begin(), pools.end(), [](const GachaPool& left, const GachaPool& right) {
            return left.pool_id < right.pool_id;
        });
        return pools;
    }

    GachaPoolItem upsert_pool_item(GachaPoolItem item) override {
        std::scoped_lock lock{mutex_};
        items_by_pool_[item.pool_id][item.item_key] = item;
        return item;
    }

    bool delete_pool_item(const std::string& pool_id, const std::string& item_key) override {
        std::scoped_lock lock{mutex_};
        const auto pool_it = items_by_pool_.find(pool_id);
        if (pool_it == items_by_pool_.end()) {
            return false;
        }
        return pool_it->second.erase(item_key) > 0;
    }

    std::vector<GachaPoolItem> list_pool_items(const std::string& pool_id) const override {
        std::scoped_lock lock{mutex_};
        std::vector<GachaPoolItem> items;
        const auto pool_it = items_by_pool_.find(pool_id);
        if (pool_it == items_by_pool_.end()) {
            return items;
        }
        for (const auto& [_, item] : pool_it->second) {
            items.push_back(item);
        }
        std::sort(items.begin(), items.end(), [](const GachaPoolItem& left, const GachaPoolItem& right) {
            if (left.rarity != right.rarity) {
                return left.rarity > right.rarity;
            }
            return left.item_key < right.item_key;
        });
        return items;
    }

    GachaPlayerState get_player_state(const std::string& player_id, const std::string& pool_id) const override {
        std::scoped_lock lock{mutex_};
        const auto key = player_id + "|" + pool_id;
        if (const auto it = states_by_key_.find(key); it != states_by_key_.end()) {
            return it->second;
        }
        return GachaPlayerState{.player_id = player_id, .pool_id = pool_id};
    }

    GachaPlayerState save_player_state(GachaPlayerState state) override {
        std::scoped_lock lock{mutex_};
        state.updated_at = std::chrono::system_clock::now();
        states_by_key_[state.player_id + "|" + state.pool_id] = state;
        return state;
    }

    void append_history(const std::vector<GachaHistoryEntry>& entries) override {
        std::scoped_lock lock{mutex_};
        for (const auto& entry : entries) {
            history_.push_back(entry);
        }
    }

    GachaHistoryPage list_history(
        const std::string& player_id,
        const std::string& pool_id,
        std::size_t limit,
        const std::optional<GachaHistoryCursor>& cursor) const override {
        MF_PROFILE_SCOPE("Memory gacha history");
        std::scoped_lock lock{mutex_};
        GachaHistoryPage page;
        std::vector<GachaHistoryEntry> matched;
        for (const auto& entry : history_) {
            if (entry.player_id != player_id) {
                continue;
            }
            if (!pool_id.empty() && entry.pool_id != pool_id) {
                continue;
            }
            matched.push_back(entry);
        }
        std::sort(matched.begin(), matched.end(), [](const GachaHistoryEntry& left, const GachaHistoryEntry& right) {
            const auto left_time = left.drawn_at.value_or(std::chrono::system_clock::time_point{});
            const auto right_time = right.drawn_at.value_or(std::chrono::system_clock::time_point{});
            if (left_time != right_time) {
                return left_time > right_time;
            }
            return left.history_id > right.history_id;
        });

        std::size_t start = 0;
        if (cursor && !cursor->last_history_id.empty()) {
            const auto it = std::find_if(
                matched.begin(),
                matched.end(),
                [&](const GachaHistoryEntry& entry) {
                    return entry.history_id == cursor->last_history_id;
                });
            if (it == matched.end()) {
                return page;
            }
            start = static_cast<std::size_t>(std::distance(matched.begin(), it)) + 1;
        }

        const auto bounded_limit = std::clamp<std::size_t>(limit, 1, 100);
        const auto remaining = matched.size() - start;
        const auto count = std::min<std::size_t>(bounded_limit, remaining);
        page.items.assign(
            matched.begin() + static_cast<std::ptrdiff_t>(start),
            matched.begin() + static_cast<std::ptrdiff_t>(start + count));
        page.has_more = count < remaining;
        if (page.has_more) {
            const auto& oldest = page.items.back();
            page.next_drawn_at = to_unix_seconds(oldest.drawn_at.value_or(std::chrono::system_clock::time_point{}));
            page.next_history_id = oldest.history_id;
        }
        return page;
    }

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, GachaPool> pools_by_id_;
    std::unordered_map<std::string, std::unordered_map<std::string, GachaPoolItem>> items_by_pool_;
    std::unordered_map<std::string, GachaPlayerState> states_by_key_;
    std::vector<GachaHistoryEntry> history_;
};

class InMemoryShopRepository final : public ShopRepository {
public:
    // 新增或更新商城商品。
    // 内存版主要用于本地开发，逻辑和 MySQL 版保持一致，方便先只看一个实现理解业务。
    ShopProduct upsert_product(ShopProduct product) override {
        std::scoped_lock lock{mutex_};
        const auto now = std::chrono::system_clock::now();
        if (const auto it = products_by_id_.find(product.product_id); it != products_by_id_.end()) {
            product.created_at = it->second.created_at;
        } else {
            product.created_at = now;
        }
        product.updated_at = now;
        products_by_id_[product.product_id] = product;
        return product;
    }

    // 删除商品配置。
    // 已经产生的购买流水不会删除，否则运营审计会断链。
    bool delete_product(const std::string& product_id) override {
        std::scoped_lock lock{mutex_};
        return products_by_id_.erase(product_id) > 0;
    }

    // 按商品 ID 查单个商品。
    std::optional<ShopProduct> find_product(const std::string& product_id) const override {
        std::scoped_lock lock{mutex_};
        if (const auto it = products_by_id_.find(product_id); it != products_by_id_.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    // 列出商城商品。
    // include_disabled=false 时只返回启用且在开放时间窗口内的商品，给玩家侧接口使用。
    std::vector<ShopProduct> list_products(
        bool include_disabled,
        std::chrono::system_clock::time_point now) const override {
        MF_PROFILE_SCOPE("Memory shop products");
        std::scoped_lock lock{mutex_};
        std::vector<ShopProduct> products;
        for (const auto& [_, product] : products_by_id_) {
            if (!include_disabled && !product.enabled) {
                continue;
            }
            if (!include_disabled && product.starts_at && now < *product.starts_at) {
                continue;
            }
            if (!include_disabled && product.ends_at && now > *product.ends_at) {
                continue;
            }
            products.push_back(product);
        }
        std::sort(products.begin(), products.end(), [](const ShopProduct& left, const ShopProduct& right) {
            if (left.category != right.category) {
                return left.category < right.category;
            }
            return left.product_id < right.product_id;
        });
        return products;
    }

    // 统计玩家对某个商品已经购买了几次，用于限购判断。
    std::int64_t count_purchases(const std::string& player_id, const std::string& product_id) const override {
        std::scoped_lock lock{mutex_};
        std::int64_t count = 0;
        for (const auto& purchase : purchases_) {
            if (purchase.player_id == player_id && purchase.product_id == product_id) {
                ++count;
            }
        }
        return count;
    }

    // 写入购买流水。
    ShopPurchaseRecord append_purchase(ShopPurchaseRecord purchase) override {
        std::scoped_lock lock{mutex_};
        purchase.purchased_at = purchase.purchased_at.value_or(std::chrono::system_clock::now());
        purchases_.push_back(purchase);
        return purchase;
    }

    // 查询玩家购买历史，供 UE 或后台排查展示。
    ShopPurchasePage list_purchases(
        const std::string& player_id,
        std::size_t limit,
        const std::optional<ShopPurchaseCursor>& cursor) const override {
        MF_PROFILE_SCOPE("Memory shop history");
        std::scoped_lock lock{mutex_};
        ShopPurchasePage page;
        std::vector<ShopPurchaseRecord> matched;
        for (const auto& purchase : purchases_) {
            if (purchase.player_id == player_id) {
                matched.push_back(purchase);
            }
        }
        std::sort(matched.begin(), matched.end(), [](const ShopPurchaseRecord& left, const ShopPurchaseRecord& right) {
            const auto left_time = left.purchased_at.value_or(std::chrono::system_clock::time_point{});
            const auto right_time = right.purchased_at.value_or(std::chrono::system_clock::time_point{});
            if (left_time != right_time) {
                return left_time > right_time;
            }
            return left.purchase_id > right.purchase_id;
        });

        std::size_t start = 0;
        if (cursor && !cursor->last_purchase_id.empty()) {
            const auto it = std::find_if(
                matched.begin(),
                matched.end(),
                [&](const ShopPurchaseRecord& purchase) {
                    return purchase.purchase_id == cursor->last_purchase_id;
                });
            if (it == matched.end()) {
                return page;
            }
            start = static_cast<std::size_t>(std::distance(matched.begin(), it)) + 1;
        }

        const auto bounded_limit = std::clamp<std::size_t>(limit, 1, 100);
        const auto remaining = matched.size() - start;
        const auto count = std::min<std::size_t>(bounded_limit, remaining);
        page.items.assign(
            matched.begin() + static_cast<std::ptrdiff_t>(start),
            matched.begin() + static_cast<std::ptrdiff_t>(start + count));
        page.has_more = count < remaining;
        if (page.has_more) {
            const auto& oldest = page.items.back();
            page.next_purchased_at = to_unix_seconds(oldest.purchased_at.value_or(std::chrono::system_clock::time_point{}));
            page.next_purchase_id = oldest.purchase_id;
        }
        return page;
    }

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, ShopProduct> products_by_id_;
    std::vector<ShopPurchaseRecord> purchases_;
};

class InMemoryLootRepository final : public LootRepository {
public:
    // 创建掉落记录。
    LootDrop create_drop(LootDrop drop) override {
        std::scoped_lock lock{mutex_};
        drop.spawned_at = drop.spawned_at.value_or(std::chrono::system_clock::now());
        drops_by_id_[drop.drop_id] = drop;
        return drop;
    }

    // 按 drop_id 查掉落。
    std::optional<LootDrop> find_drop(const std::string& drop_id) const override {
        std::scoped_lock lock{mutex_};
        if (const auto it = drops_by_id_.find(drop_id); it != drops_by_id_.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    // 把掉落从“未拾取”切换为“已拾取”。
    // 如果已经被拾取则返回 nullopt，服务层据此拒绝重复领取。
    std::optional<LootDrop> mark_picked(
        const std::string& drop_id,
        const std::string& picked_by_player_id,
        std::chrono::system_clock::time_point picked_at) override {
        std::scoped_lock lock{mutex_};
        const auto it = drops_by_id_.find(drop_id);
        if (it == drops_by_id_.end() || it->second.picked) {
            return std::nullopt;
        }
        it->second.picked = true;
        it->second.picked_by_player_id = picked_by_player_id;
        it->second.picked_at = picked_at;
        return it->second;
    }

    // 查询某玩家可见掉落。
    LootDropPage list_drops(
        const std::string& owner_player_id,
        bool include_picked,
        std::chrono::system_clock::time_point now,
        std::size_t limit,
        const std::optional<LootDropCursor>& cursor) const override {
        std::scoped_lock lock{mutex_};
        LootDropPage page;
        std::vector<LootDrop> drops;
        for (const auto& [_, drop] : drops_by_id_) {
            if (!owner_player_id.empty() && drop.owner_player_id != owner_player_id) {
                continue;
            }
            if (!include_picked && drop.picked) {
                continue;
            }
            if (!include_picked && drop.expires_at && now > *drop.expires_at) {
                continue;
            }
            drops.push_back(drop);
        }
        std::sort(drops.begin(), drops.end(), [](const LootDrop& left, const LootDrop& right) {
            const auto left_time = left.spawned_at.value_or(std::chrono::system_clock::time_point{});
            const auto right_time = right.spawned_at.value_or(std::chrono::system_clock::time_point{});
            if (left_time != right_time) {
                return left_time > right_time;
            }
            return left.drop_id > right.drop_id;
        });

        std::size_t start = 0;
        if (cursor && !cursor->last_drop_id.empty()) {
            const auto it = std::find_if(
                drops.begin(),
                drops.end(),
                [&](const LootDrop& drop) {
                    return drop.drop_id == cursor->last_drop_id;
                });
            if (it == drops.end()) {
                return page;
            }
            start = static_cast<std::size_t>(std::distance(drops.begin(), it)) + 1;
        }

        const auto bounded_limit = std::clamp<std::size_t>(limit, 1, 100);
        const auto remaining = drops.size() - start;
        const auto count = std::min<std::size_t>(bounded_limit, remaining);
        page.items.assign(
            drops.begin() + static_cast<std::ptrdiff_t>(start),
            drops.begin() + static_cast<std::ptrdiff_t>(start + count));
        page.has_more = count < remaining;
        if (page.has_more) {
            const auto& oldest = page.items.back();
            page.next_spawned_at = to_unix_seconds(oldest.spawned_at.value_or(std::chrono::system_clock::time_point{}));
            page.next_drop_id = oldest.drop_id;
        }
        return page;
    }

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, LootDrop> drops_by_id_;
};

class InMemoryFriendRepository final : public FriendRepository {
public:
    bool add_friendship(const std::string& player_id, const std::string& friend_player_id) override {
        if (player_id == friend_player_id) {
            return false;
        }

        std::scoped_lock lock{mutex_};
        auto& outgoing = friends_by_player_[player_id];
        if (std::find(outgoing.begin(), outgoing.end(), friend_player_id) != outgoing.end()) {
            return false;
        }

        // 示例里把好友关系对称存两份，换读取简单。
        // 真正数据库版本通常会建一条关系记录，再配 pending/accepted/blocked 等状态字段。
        outgoing.push_back(friend_player_id);
        friends_by_player_[friend_player_id].push_back(player_id);
        return true;
    }

    std::vector<FriendLink> list_friends(const std::string& player_id) const override {
        MF_PROFILE_SCOPE("Memory friend list");
        std::scoped_lock lock{mutex_};
        std::vector<FriendLink> links;
        if (const auto it = friends_by_player_.find(player_id); it != friends_by_player_.end()) {
            links.reserve(it->second.size());
            for (const auto& friend_player_id : it->second) {
                links.push_back(FriendLink{
                    .player_id = player_id,
                    .friend_player_id = friend_player_id,
                });
            }
        }
        return links;
    }

    std::vector<FriendLink> list_friends_by_last_message(
        const std::string& player_id,
        std::size_t limit,
        std::size_t offset,
        const std::optional<FriendPageCursor>& cursor) const override {
        auto links = list_friends(player_id);
        std::sort(
            links.begin(),
            links.end(),
            [](const FriendLink& left, const FriendLink& right) {
                return left.friend_player_id < right.friend_player_id;
            });
        std::size_t begin = 0;
        if (cursor) {
            const auto first_after = std::upper_bound(
                links.begin(),
                links.end(),
                cursor->last_friend_player_id,
                [](const std::string& key, const FriendLink& link) {
                    return key < link.friend_player_id;
                });
            begin = static_cast<std::size_t>(std::distance(links.begin(), first_after));
        } else {
            begin = std::min(offset, links.size());
        }
        if (begin >= links.size()) {
            return {};
        }
        const auto available = links.size() - begin;
        const auto end = begin + std::min(limit, available);
        return std::vector<FriendLink>(
            links.begin() + static_cast<std::ptrdiff_t>(begin),
            links.begin() + static_cast<std::ptrdiff_t>(end));
    }

    bool send_friend_request(const std::string& requester_player_id, const std::string& target_player_id) override {
        if (requester_player_id == target_player_id) {
            return false;
        }

        std::scoped_lock lock{mutex_};
        if (is_friend_unlocked(requester_player_id, target_player_id)) {
            return false;
        }

        const auto key = request_key(requester_player_id, target_player_id);
        if (friend_requests_.contains(key)) {
            return false;
        }

        friend_requests_[key] = FriendRequest{
            .requester_player_id = requester_player_id,
            .target_player_id = target_player_id,
            .status = FriendRequestStatus::pending_outgoing,
            .created_at = std::chrono::system_clock::now(),
        };
        return true;
    }

    std::vector<FriendRequest> list_incoming_requests(const std::string& player_id) const override {
        MF_PROFILE_SCOPE("Memory incoming friend requests");
        std::scoped_lock lock{mutex_};
        std::vector<FriendRequest> items;
        for (const auto& [_, request] : friend_requests_) {
            if (request.target_player_id == player_id) {
                items.push_back(FriendRequest{
                    .requester_player_id = request.requester_player_id,
                    .target_player_id = request.target_player_id,
                    .status = FriendRequestStatus::pending_incoming,
                    .created_at = request.created_at,
                });
            }
        }
        return items;
    }

    std::vector<FriendRequest> list_outgoing_requests(const std::string& player_id) const override {
        std::scoped_lock lock{mutex_};
        std::vector<FriendRequest> items;
        for (const auto& [_, request] : friend_requests_) {
            if (request.requester_player_id == player_id) {
                items.push_back(request);
            }
        }
        return items;
    }

    bool respond_friend_request(
        const std::string& requester_player_id,
        const std::string& target_player_id,
        bool accept) override {
        std::scoped_lock lock{mutex_};
        const auto key = request_key(requester_player_id, target_player_id);
        const auto it = friend_requests_.find(key);
        if (it == friend_requests_.end()) {
            return false;
        }

        friend_requests_.erase(it);
        if (accept) {
            friends_by_player_[requester_player_id].push_back(target_player_id);
            friends_by_player_[target_player_id].push_back(requester_player_id);
        }
        return true;
    }

    bool set_chat_block(
        const std::string& player_id,
        const std::string& blocked_player_id,
        bool blocked) override {
        std::scoped_lock lock{mutex_};
        auto& targets = blocked_by_player_[player_id];
        if (blocked) {
            return targets.insert(blocked_player_id).second;
        }
        return targets.erase(blocked_player_id) > 0;
    }

    bool is_chat_blocked(const std::string& player_id, const std::string& blocked_player_id) const override {
        std::scoped_lock lock{mutex_};
        if (const auto it = blocked_by_player_.find(player_id); it != blocked_by_player_.end()) {
            return it->second.contains(blocked_player_id);
        }
        return false;
    }

    std::vector<ChatBlockEntry> list_chat_blocks(const std::string& player_id) const override {
        std::scoped_lock lock{mutex_};
        std::vector<ChatBlockEntry> entries;
        if (const auto it = blocked_by_player_.find(player_id); it != blocked_by_player_.end()) {
            entries.reserve(it->second.size());
            for (const auto& blocked_player_id : it->second) {
                entries.push_back(ChatBlockEntry{
                    .player_id = player_id,
                    .blocked_player_id = blocked_player_id,
                });
            }
        }
        return entries;
    }

private:
    static std::string request_key(const std::string& requester_player_id, const std::string& target_player_id) {
        return requester_player_id + "->" + target_player_id;
    }

    bool is_friend_unlocked(const std::string& player_id, const std::string& friend_player_id) const {
        if (const auto it = friends_by_player_.find(player_id); it != friends_by_player_.end()) {
            return std::find(it->second.begin(), it->second.end(), friend_player_id) != it->second.end();
        }
        return false;
    }

    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::vector<std::string>> friends_by_player_;
    std::unordered_map<std::string, FriendRequest> friend_requests_;
    std::unordered_map<std::string, std::unordered_set<std::string>> blocked_by_player_;
};

class InMemorySessionRepository final : public SessionRepository {
public:
    void save(Session session) override {
        std::scoped_lock lock{mutex_};
        // Session 存储对调用方故意保持“黑盒”。
        // 将来换成 Redis 时，不应该要求 auth 或 handler 跟着改代码。
        sessions_by_token_[session.token] = session;
        sessions_by_refresh_token_[session.refresh_token] = session.token;
        sessions_by_id_[session.session_id] = session.token;
        session_ids_by_player_[session.player_id].push_back(session.session_id);
    }

    void save_single_session(Session session) override {
        std::scoped_lock lock{mutex_};
        if (const auto it = session_ids_by_player_.find(session.player_id);
            it != session_ids_by_player_.end()) {
            const auto existing_ids = it->second;
            for (const auto& session_id : existing_ids) {
                if (const auto token_it = sessions_by_id_.find(session_id);
                    token_it != sessions_by_id_.end()) {
                    revoke_by_token_unlocked(token_it->second);
                }
            }
        }
        sessions_by_token_[session.token] = session;
        sessions_by_refresh_token_[session.refresh_token] = session.token;
        sessions_by_id_[session.session_id] = session.token;
        session_ids_by_player_[session.player_id].push_back(session.session_id);
    }

    std::optional<Session> find_by_token(const std::string& token) const override {
        MF_PROFILE_SCOPE("Memory session lookup");
        std::scoped_lock lock{mutex_};
        if (const auto it = sessions_by_token_.find(token); it != sessions_by_token_.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    std::optional<Session> find_by_refresh_token(const std::string& refresh_token) const override {
        std::scoped_lock lock{mutex_};
        if (const auto it = sessions_by_refresh_token_.find(refresh_token); it != sessions_by_refresh_token_.end()) {
            return find_by_token_unlocked(it->second);
        }
        return std::nullopt;
    }

    std::optional<Session> consume_refresh_token(const std::string& refresh_token) override {
        std::scoped_lock lock{mutex_};
        const auto refresh_it = sessions_by_refresh_token_.find(refresh_token);
        if (refresh_it == sessions_by_refresh_token_.end()) {
            return std::nullopt;
        }
        const auto session = find_by_token_unlocked(refresh_it->second);
        if (!session || !revoke_by_token_unlocked(refresh_it->second)) {
            return std::nullopt;
        }
        return session;
    }

    std::vector<Session> list_by_player_id(const std::string& player_id) const override {
        std::scoped_lock lock{mutex_};
        std::vector<Session> sessions;
        if (const auto it = session_ids_by_player_.find(player_id); it != session_ids_by_player_.end()) {
            sessions.reserve(it->second.size());
            for (const auto& session_id : it->second) {
                if (const auto token_it = sessions_by_id_.find(session_id); token_it != sessions_by_id_.end()) {
                    if (const auto session = find_by_token_unlocked(token_it->second)) {
                        sessions.push_back(*session);
                    }
                }
            }
        }
        return sessions;
    }

    bool revoke_by_token(const std::string& token) override {
        std::scoped_lock lock{mutex_};
        return revoke_by_token_unlocked(token);
    }

    bool revoke_by_session_id(const std::string& session_id) override {
        std::scoped_lock lock{mutex_};
        if (const auto it = sessions_by_id_.find(session_id); it != sessions_by_id_.end()) {
            return revoke_by_token_unlocked(it->second);
        }
        return false;
    }

    std::size_t revoke_all_for_player(
        const std::string& player_id,
        const std::optional<std::string>& except_session_id) override {
        std::scoped_lock lock{mutex_};
        std::size_t removed = 0;
        if (const auto it = session_ids_by_player_.find(player_id); it != session_ids_by_player_.end()) {
            const auto ids = it->second;
            for (const auto& session_id : ids) {
                if (except_session_id && *except_session_id == session_id) {
                    continue;
                }
                if (const auto token_it = sessions_by_id_.find(session_id); token_it != sessions_by_id_.end() &&
                    revoke_by_token_unlocked(token_it->second)) {
                    ++removed;
                }
            }
        }
        return removed;
    }

private:
    std::optional<Session> find_by_token_unlocked(const std::string& token) const {
        if (const auto it = sessions_by_token_.find(token); it != sessions_by_token_.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    bool revoke_by_token_unlocked(const std::string& token) {
        const auto it = sessions_by_token_.find(token);
        if (it == sessions_by_token_.end()) {
            return false;
        }

        const auto session = it->second;
        sessions_by_refresh_token_.erase(session.refresh_token);
        sessions_by_id_.erase(session.session_id);
        sessions_by_token_.erase(it);

        if (auto player_it = session_ids_by_player_.find(session.player_id); player_it != session_ids_by_player_.end()) {
            auto& ids = player_it->second;
            ids.erase(std::remove(ids.begin(), ids.end(), session.session_id), ids.end());
            if (ids.empty()) {
                session_ids_by_player_.erase(player_it);
            }
        }
        return true;
    }

    mutable std::mutex mutex_;
    std::unordered_map<std::string, Session> sessions_by_token_;
    std::unordered_map<std::string, std::string> sessions_by_refresh_token_;
    std::unordered_map<std::string, std::string> sessions_by_id_;
    std::unordered_map<std::string, std::vector<std::string>> session_ids_by_player_;
};

class InMemoryAuthSecurityRepository final : public AuthSecurityRepository {
public:
    CaptchaChallenge create_captcha(
        std::string prompt,
        std::string expected_answer,
        std::chrono::system_clock::time_point expires_at) override {
        std::scoped_lock lock{mutex_};
        const auto challenge_id = "captcha_" + make_random_token();
        captcha_answers_[challenge_id] = {
            .answer = std::move(expected_answer),
            .expires_at = expires_at,
        };
        return CaptchaChallenge{
            .challenge_id = challenge_id,
            .prompt = std::move(prompt),
            .expires_at = expires_at,
        };
    }

    bool verify_and_consume_captcha(const std::string& challenge_id, const std::string& answer) override {
        std::scoped_lock lock{mutex_};
        const auto it = captcha_answers_.find(challenge_id);
        if (it == captcha_answers_.end()) {
            return false;
        }
        const auto valid = it->second.expires_at >= std::chrono::system_clock::now() && it->second.answer == answer;
        captcha_answers_.erase(it);
        return valid;
    }

    LoginRiskState inspect_login_risk(const std::string& login_name, const std::string& client_ip) const override {
        std::scoped_lock lock{mutex_};
        return inspect_login_risk_unlocked(login_name, client_ip);
    }

    LoginRiskState record_login_failure(
        const std::string& login_name,
        const std::string& client_ip,
        std::chrono::seconds,
        std::int64_t captcha_threshold,
        std::int64_t rate_limit_threshold,
        std::chrono::seconds block_duration) override {
        std::scoped_lock lock{mutex_};
        auto& state = risk_states_[risk_key(login_name, client_ip)];
        ++state.failure_count;
        if (state.failure_count >= rate_limit_threshold) {
            state.blocked_until = std::chrono::system_clock::now() + block_duration;
        }
        LoginRiskState result = inspect_login_risk_unlocked(login_name, client_ip);
        result.captcha_required = state.failure_count >= captcha_threshold;
        return result;
    }

    void clear_login_failures(const std::string& login_name, const std::string& client_ip) override {
        std::scoped_lock lock{mutex_};
        risk_states_.erase(risk_key(login_name, client_ip));
    }

    LoginRiskState record_registration_attempt(
        const std::string& login_name,
        const std::string& client_ip,
        std::chrono::seconds window,
        std::int64_t max_per_login_ip,
        std::int64_t max_per_ip) override {
        std::scoped_lock lock{mutex_};
        const auto now = std::chrono::system_clock::now();
        auto& pair_state = registration_states_[risk_key(login_name, client_ip)];
        auto& ip_state = registration_states_["ip|" + client_ip];
        for (auto* state : {&pair_state, &ip_state}) {
            if (state->window_started + window <= now) {
                state->count = 0;
                state->window_started = now;
            }
            ++state->count;
        }
        LoginRiskState result;
        if (pair_state.count > max_per_login_ip || ip_state.count > max_per_ip) {
            result.rate_limited = true;
            const auto retry_pair = std::chrono::duration_cast<std::chrono::seconds>(
                pair_state.window_started + window - now).count();
            const auto retry_ip = std::chrono::duration_cast<std::chrono::seconds>(
                ip_state.window_started + window - now).count();
            result.retry_after_seconds = std::max<std::int64_t>(1, std::max(retry_pair, retry_ip));
        }
        return result;
    }

    void clear_registration_attempts(
        const std::string& login_name,
        const std::string& client_ip) override {
        std::scoped_lock lock{mutex_};
        registration_states_.erase(risk_key(login_name, client_ip));
    }

    bool is_blacklisted(const std::string& subject_type, const std::string& subject_value) const override {
        std::scoped_lock lock{mutex_};
        const auto now = std::chrono::system_clock::now();
        for (const auto& entry : blacklist_entries_) {
            if (entry.subject_type != subject_type || entry.subject_value != subject_value) {
                continue;
            }
            if (!entry.expires_at || *entry.expires_at >= now) {
                return true;
            }
        }
        return false;
    }

    BlacklistEntry add_blacklist_entry(
        std::string subject_type,
        std::string subject_value,
        std::string reason,
        std::optional<std::chrono::system_clock::time_point> expires_at) override {
        std::scoped_lock lock{mutex_};
        BlacklistEntry entry{
            .entry_id = "black_" + std::to_string(next_blacklist_id++),
            .subject_type = std::move(subject_type),
            .subject_value = std::move(subject_value),
            .reason = std::move(reason),
            .expires_at = expires_at,
        };
        blacklist_entries_.push_back(entry);
        return entry;
    }

    std::vector<BlacklistEntry> list_blacklist_entries(std::size_t limit) const override {
        std::scoped_lock lock{mutex_};
        std::vector<BlacklistEntry> items;
        const auto capped = std::min(limit, blacklist_entries_.size());
        items.reserve(capped);
        for (auto it = blacklist_entries_.rbegin(); it != blacklist_entries_.rend() && items.size() < capped; ++it) {
            items.push_back(*it);
        }
        return items;
    }

private:
    struct CaptchaAnswerEntry {
        std::string answer;
        std::chrono::system_clock::time_point expires_at;
    };

    struct RiskStateEntry {
        std::int64_t failure_count = 0;
        std::optional<std::chrono::system_clock::time_point> blocked_until;
    };

    struct RegistrationStateEntry {
        std::int64_t count = 0;
        std::chrono::system_clock::time_point window_started = std::chrono::system_clock::now();
    };

    static std::string risk_key(const std::string& login_name, const std::string& client_ip) {
        return login_name + "|" + client_ip;
    }

    LoginRiskState inspect_login_risk_unlocked(const std::string& login_name, const std::string& client_ip) const {
        LoginRiskState state;
        if (const auto it = risk_states_.find(risk_key(login_name, client_ip)); it != risk_states_.end()) {
            state.failure_count = it->second.failure_count;
            state.captcha_required = state.failure_count >= 3;
            if (it->second.blocked_until && *it->second.blocked_until > std::chrono::system_clock::now()) {
                state.rate_limited = true;
                state.retry_after_seconds = std::chrono::duration_cast<std::chrono::seconds>(
                                                *it->second.blocked_until - std::chrono::system_clock::now())
                                                .count();
            }
        }
        return state;
    }

    mutable std::mutex mutex_;
    std::unordered_map<std::string, CaptchaAnswerEntry> captcha_answers_;
    std::unordered_map<std::string, RiskStateEntry> risk_states_;
    std::unordered_map<std::string, RegistrationStateEntry> registration_states_;
    std::vector<BlacklistEntry> blacklist_entries_;
};

class InMemoryChatRepository final : public ChatRepository {
public:
    ChatMessage append(ChatMessage message) override {
        std::scoped_lock lock{mutex_};
        if (message.message_id.empty()) {
            message.message_id = "msg_" + std::to_string(next_message_id++);
        }
        messages_by_channel_[message.channel.channel_id].push_back(std::move(message));
        return messages_by_channel_[message.channel.channel_id].back();
    }

    std::optional<ChatMessage> recall(
        const std::string& channel_id,
        const std::string& message_id,
        const std::string& recalled_by_player_id) override {
        std::scoped_lock lock{mutex_};
        if (const auto it = messages_by_channel_.find(channel_id); it != messages_by_channel_.end()) {
            for (auto& message : it->second) {
                if (message.message_id == message_id && !message.recalled) {
                    if (message.sender_player_id != recalled_by_player_id) {
                        return std::nullopt;
                    }
                    message.recalled = true;
                    message.recalled_by_player_id = recalled_by_player_id;
                    message.recalled_at = std::chrono::system_clock::now();
                    message.text = "[消息已撤回]";
                    return message;
                }
            }
        }
        return std::nullopt;
    }

    std::vector<ChatMessage> recent_messages(const std::string& channel_id, std::size_t limit) const override {
        std::scoped_lock lock{mutex_};
        std::vector<ChatMessage> result;
        if (const auto it = messages_by_channel_.find(channel_id); it != messages_by_channel_.end()) {
            const auto& messages = it->second;
            // 这里只返回尾部若干条，
            // 模拟大厅或房间 UI 首次进入时常见的“最近聊天记录”查询。
            const auto begin = messages.size() > limit ? messages.size() - limit : 0;
            result.assign(messages.begin() + static_cast<std::ptrdiff_t>(begin), messages.end());
        }
        return result;
    }

    ChatHistoryPage history_page(
        const std::string& channel_id,
        std::string_view cursor,
        std::size_t limit) const override {
        std::scoped_lock lock{mutex_};
        ChatHistoryPage page;
        const auto bounded_limit = std::clamp<std::size_t>(limit, 1, 100);
        const auto it = messages_by_channel_.find(channel_id);
        if (it == messages_by_channel_.end() || it->second.empty()) {
            return page;
        }
        const auto& messages = it->second;

        // cursor 是“下一页要读的最新一条消息”的下标；首屏从末尾开始。
        std::size_t from = messages.size() - 1;
        if (!cursor.empty()) {
            try {
                const auto parsed = std::stoull(std::string(cursor));
                if (parsed >= messages.size()) {
                    return page;
                }
                from = static_cast<std::size_t>(parsed);
            } catch (const std::exception&) {
                return page;
            }
        }

        const auto count = std::min<std::size_t>(bounded_limit, from + 1);
        const auto begin = from + 1 - count;
        page.messages.assign(
            messages.begin() + static_cast<std::ptrdiff_t>(begin),
            messages.begin() + static_cast<std::ptrdiff_t>(from + 1));
        page.has_more = begin > 0;
        if (page.has_more) {
            page.next_cursor = std::to_string(begin - 1);
        }
        return page;
    }

    std::vector<ChatMessage> latest_messages(
        const std::vector<std::string>& channel_ids,
        std::size_t limit) const override {
        std::scoped_lock lock{mutex_};
        std::vector<ChatMessage> result;
        for (const auto& channel_id : channel_ids) {
            if (const auto it = messages_by_channel_.find(channel_id); it != messages_by_channel_.end()) {
                const auto& messages = it->second;
                const auto begin = messages.size() > limit ? messages.size() - limit : 0;
                result.insert(
                    result.end(),
                    messages.begin() + static_cast<std::ptrdiff_t>(begin),
                    messages.end());
            }
        }
        return result;
    }

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::vector<ChatMessage>> messages_by_channel_;
};

class InMemoryChatModerationRepository final : public ChatModerationRepository {
public:
    ChatMuteEntry mute_player(
        const std::string& channel_id,
        const std::string& muted_player_id,
        const std::string& muted_by_player_id,
        std::string reason,
        std::optional<std::chrono::system_clock::time_point> muted_until) override {
        std::scoped_lock lock{mutex_};
        auto entry = ChatMuteEntry{
            .channel_id = channel_id,
            .muted_player_id = muted_player_id,
            .muted_by_player_id = muted_by_player_id,
            .reason = std::move(reason),
            .muted_until = muted_until,
        };
        mute_entries_[mute_key(channel_id, muted_player_id)] = entry;
        return entry;
    }

    void clear_mute(const std::string& channel_id, const std::string& muted_player_id) override {
        std::scoped_lock lock{mutex_};
        mute_entries_.erase(mute_key(channel_id, muted_player_id));
    }

    std::optional<ChatMuteEntry> find_active_mute(
        const std::string& channel_id,
        const std::string& muted_player_id) const override {
        std::scoped_lock lock{mutex_};
        if (const auto it = mute_entries_.find(mute_key(channel_id, muted_player_id)); it != mute_entries_.end()) {
            if (!it->second.muted_until || *it->second.muted_until >= std::chrono::system_clock::now()) {
                return it->second;
            }
        }
        return std::nullopt;
    }

private:
    static std::string mute_key(const std::string& channel_id, const std::string& muted_player_id) {
        return channel_id + "|" + muted_player_id;
    }

    mutable std::mutex mutex_;
    std::unordered_map<std::string, ChatMuteEntry> mute_entries_;
};

class InMemoryRewardRepository final : public RewardRepository {
public:
    std::optional<RewardGrantRecord> find_by_grant_id(const std::string& grant_id) const override {
        std::scoped_lock lock{mutex_};
        if (const auto it = grants_by_id_.find(grant_id); it != grants_by_id_.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    RewardGrantRecord record_grant(
        std::string grant_id,
        const std::string& player_id,
        std::string source_type,
        std::string source_id,
        std::vector<RewardEntry> rewards) override {
        std::scoped_lock lock{mutex_};
        if (grant_id.empty()) {
            grant_id = "grant_" + std::to_string(next_reward_grant_id++);
        }
        if (const auto it = grants_by_id_.find(grant_id); it != grants_by_id_.end()) {
            return it->second;
        }

        RewardGrantRecord grant{
            .grant_id = std::move(grant_id),
            .player_id = player_id,
            .source_type = std::move(source_type),
            .source_id = std::move(source_id),
            .rewards = std::move(rewards),
            .granted_at = std::chrono::system_clock::now(),
        };
        grants_by_player_[player_id].push_back(grant.grant_id);
        grants_by_id_[grant.grant_id] = grant;
        return grant;
    }

    std::vector<RewardGrantRecord> list_by_player_id(const std::string& player_id, std::size_t limit) const override {
        std::scoped_lock lock{mutex_};
        std::vector<RewardGrantRecord> items;
        if (const auto it = grants_by_player_.find(player_id); it != grants_by_player_.end()) {
            const auto capped = std::min(limit, it->second.size());
            items.reserve(capped);
            for (auto grant_it = it->second.rbegin(); grant_it != it->second.rend() && items.size() < capped; ++grant_it) {
                items.push_back(grants_by_id_.at(*grant_it));
            }
        }
        return items;
    }

    std::size_t count_filtered(
        const std::string& player_id,
        const std::optional<std::string>& source_type_filter,
        const std::optional<std::string>& source_id_filter) const override {
        auto items = list_filtered(player_id, static_cast<std::size_t>(-1), 0, source_type_filter, source_id_filter);
        return items.size();
    }

    std::vector<RewardGrantRecord> list_filtered(
        const std::string& player_id,
        std::size_t limit,
        std::size_t offset,
        const std::optional<std::string>& source_type_filter,
        const std::optional<std::string>& source_id_filter) const override {
        auto items = list_by_player_id(player_id, grants_by_id_.size());
        if (source_type_filter) {
            items.erase(
                std::remove_if(items.begin(), items.end(), [&](const RewardGrantRecord& item) {
                    return item.source_type != *source_type_filter;
                }),
                items.end());
        }
        if (source_id_filter) {
            items.erase(
                std::remove_if(items.begin(), items.end(), [&](const RewardGrantRecord& item) {
                    return item.source_id.find(*source_id_filter) == std::string::npos;
                }),
                items.end());
        }
        if (offset >= items.size()) {
            return {};
        }
        const auto end = std::min(items.size(), offset + limit);
        return std::vector<RewardGrantRecord>(
            items.begin() + static_cast<std::ptrdiff_t>(offset),
            items.begin() + static_cast<std::ptrdiff_t>(end));
    }

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, RewardGrantRecord> grants_by_id_;
    std::unordered_map<std::string, std::vector<std::string>> grants_by_player_;
};

class InMemoryMailRepository final : public MailRepository {
public:
    MailMessage send_mail(MailMessage mail) override {
        std::scoped_lock lock{mutex_};
        if (mail.mail_id.empty()) {
            mail.mail_id = "mail_" + std::to_string(next_mail_id++);
        }
        if (!mail.sent_at) {
            mail.sent_at = std::chrono::system_clock::now();
        }
        mailbox_by_player_[mail.player_id].push_back(mail.mail_id);
        mails_by_id_[mail.mail_id] = mail;
        return mail;
    }

    std::vector<MailMessage> list_inbox(const std::string& player_id, bool include_deleted) const override {
        MF_PROFILE_SCOPE("Memory mail list");
        std::scoped_lock lock{mutex_};
        std::vector<MailMessage> items;
        if (const auto it = mailbox_by_player_.find(player_id); it != mailbox_by_player_.end()) {
            items.reserve(it->second.size());
            for (auto mail_it = it->second.rbegin(); mail_it != it->second.rend(); ++mail_it) {
                const auto& mail = mails_by_id_.at(*mail_it);
                if (!include_deleted && mail.deleted) {
                    continue;
                }
                items.push_back(mail);
            }
        }
        return items;
    }

    std::vector<MailMessage> list_inbox_cursor(
        const std::string& player_id,
        bool include_deleted,
        std::size_t limit,
        const std::optional<MailPageCursor>& cursor) const override {
        if (!cursor) {
            return list_filtered(player_id, include_deleted, limit, 0, std::nullopt, std::nullopt);
        }
        auto items = list_inbox(player_id, include_deleted);
        const auto unix_seconds = [](const MailMessage& mail) {
            return mail.sent_at
                       ? static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                             mail.sent_at->time_since_epoch())
                             .count())
                       : 0;
        };
        std::stable_sort(items.begin(), items.end(), [&](const MailMessage& left, const MailMessage& right) {
            const auto left_sent = unix_seconds(left);
            const auto right_sent = unix_seconds(right);
            if (left_sent != right_sent) {
                return left_sent > right_sent;
            }
            return left.mail_id > right.mail_id;
        });
        auto begin = items.begin();
        if (cursor) {
            begin = std::find_if(items.begin(), items.end(), [&](const MailMessage& mail) {
                const auto sent = unix_seconds(mail);
                return sent < cursor->last_sent_at ||
                    (sent == cursor->last_sent_at && mail.mail_id < cursor->last_mail_id);
            });
        }
        if (begin == items.end()) {
            return {};
        }
        const auto remaining = static_cast<std::size_t>(std::distance(begin, items.end()));
        const auto take = std::min(limit, remaining);
        return std::vector<MailMessage>(begin, begin + static_cast<std::ptrdiff_t>(take));
    }

    std::size_t count_filtered(
        const std::string& player_id,
        bool include_deleted,
        const std::optional<bool>& claimed_filter,
        const std::optional<bool>& read_filter) const override {
        auto items = list_filtered(
            player_id,
            include_deleted,
            static_cast<std::size_t>(-1),
            0,
            claimed_filter,
            read_filter);
        return items.size();
    }

    std::vector<MailMessage> list_filtered(
        const std::string& player_id,
        bool include_deleted,
        std::size_t limit,
        std::size_t offset,
        const std::optional<bool>& claimed_filter,
        const std::optional<bool>& read_filter) const override {
        auto items = list_inbox(player_id, include_deleted);
        if (claimed_filter) {
            items.erase(
                std::remove_if(items.begin(), items.end(), [&](const MailMessage& item) {
                    return item.claimed != *claimed_filter;
                }),
                items.end());
        }
        if (read_filter) {
            items.erase(
                std::remove_if(items.begin(), items.end(), [&](const MailMessage& item) {
                    return item.read != *read_filter;
                }),
                items.end());
        }
        if (offset >= items.size()) {
            return {};
        }
        const auto end = std::min(items.size(), offset + limit);
        return std::vector<MailMessage>(
            items.begin() + static_cast<std::ptrdiff_t>(offset),
            items.begin() + static_cast<std::ptrdiff_t>(end));
    }

    std::optional<MailMessage> find_mail(const std::string& player_id, const std::string& mail_id) const override {
        std::scoped_lock lock{mutex_};
        if (const auto it = mails_by_id_.find(mail_id); it != mails_by_id_.end() && it->second.player_id == player_id) {
            return it->second;
        }
        return std::nullopt;
    }

    std::optional<MailMessage> mark_read(
        const std::string& player_id,
        const std::string& mail_id,
        std::chrono::system_clock::time_point read_at) override {
        std::scoped_lock lock{mutex_};
        auto it = mails_by_id_.find(mail_id);
        if (it == mails_by_id_.end() || it->second.player_id != player_id) {
            return std::nullopt;
        }
        it->second.read = true;
        it->second.read_at = read_at;
        return it->second;
    }

    std::optional<MailMessage> mark_claimed(
        const std::string& player_id,
        const std::string& mail_id,
        const std::string& claim_grant_id,
        std::chrono::system_clock::time_point claimed_at) override {
        std::scoped_lock lock{mutex_};
        auto it = mails_by_id_.find(mail_id);
        if (it == mails_by_id_.end() || it->second.player_id != player_id) {
            return std::nullopt;
        }
        it->second.read = true;
        it->second.claimed = true;
        it->second.claim_grant_id = claim_grant_id;
        it->second.read_at = it->second.read_at.value_or(claimed_at);
        it->second.claimed_at = claimed_at;
        return it->second;
    }

    std::optional<MailMessage> mark_deleted(
        const std::string& player_id,
        const std::string& mail_id,
        std::chrono::system_clock::time_point deleted_at) override {
        std::scoped_lock lock{mutex_};
        auto it = mails_by_id_.find(mail_id);
        if (it == mails_by_id_.end() || it->second.player_id != player_id) {
            return std::nullopt;
        }
        it->second.deleted = true;
        it->second.deleted_at = deleted_at;
        return it->second;
    }

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, MailMessage> mails_by_id_;
    std::unordered_map<std::string, std::vector<std::string>> mailbox_by_player_;
};

class InMemoryRewardTemplateRepository final : public RewardTemplateRepository {
public:
    RewardTemplate upsert_template(
        std::string template_id,
        std::string name,
        std::vector<RewardEntry> rewards) override {
        std::scoped_lock lock{mutex_};
        auto& entry = templates_by_id_[template_id];
        entry.template_id = std::move(template_id);
        entry.name = std::move(name);
        entry.rewards = std::move(rewards);
        entry.created_at = entry.created_at.value_or(std::chrono::system_clock::now());
        entry.updated_at = std::chrono::system_clock::now();
        return entry;
    }

    std::optional<RewardTemplate> find_by_template_id(const std::string& template_id) const override {
        std::scoped_lock lock{mutex_};
        if (const auto it = templates_by_id_.find(template_id); it != templates_by_id_.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    std::vector<RewardTemplate> list_all() const override {
        std::scoped_lock lock{mutex_};
        std::vector<RewardTemplate> items;
        items.reserve(templates_by_id_.size());
        for (const auto& [_, value] : templates_by_id_) {
            items.push_back(value);
        }
        return items;
    }

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, RewardTemplate> templates_by_id_;
};

class InMemoryAnnouncementRepository final : public AnnouncementRepository {
public:
    AnnouncementEntry publish(
        ChatChannel channel,
        std::string operator_id,
        std::string title,
        std::string body,
        std::chrono::system_clock::time_point published_at,
        std::optional<std::chrono::system_clock::time_point> expires_at) override {
        std::scoped_lock lock{mutex_};
        AnnouncementEntry entry{
            .announcement_id = "announcement_" + std::to_string(next_id_++),
            .channel = std::move(channel),
            .operator_id = std::move(operator_id),
            .title = std::move(title),
            .body = std::move(body),
            .published_at = published_at,
            .expires_at = expires_at,
        };
        items_.push_back(entry);
        return entry;
    }

    std::vector<AnnouncementEntry> list_recent(
        std::size_t limit,
        const std::optional<ChatChannelKind>& kind_filter,
        const std::optional<std::string>& scope_filter,
        std::chrono::system_clock::time_point now) const override {
        std::scoped_lock lock{mutex_};
        std::vector<AnnouncementEntry> items;
        for (auto it = items_.rbegin(); it != items_.rend() && items.size() < limit; ++it) {
            if (it->expires_at && *it->expires_at < now) {
                continue;
            }
            if (kind_filter && it->channel.kind != *kind_filter) {
                continue;
            }
            if (scope_filter && it->channel.scope_id != *scope_filter) {
                continue;
            }
            items.push_back(*it);
        }
        return items;
    }

private:
    mutable std::mutex mutex_;
    std::vector<AnnouncementEntry> items_;
    std::size_t next_id_ = 1;
};

class InMemoryAuditLogRepository final : public AuditLogRepository {
public:
    AuditLogEntry append(AuditLogWriteRequest request) override {
        std::scoped_lock lock{mutex_};
        AuditLogEntry entry{
            .audit_id = "audit_" + std::to_string(next_id_++),
            .operator_id = std::move(request.operator_id),
            .action = std::move(request.action),
            .target_type = std::move(request.target_type),
            .target_id = std::move(request.target_id),
            .request_ip = std::move(request.request_ip),
            .user_agent = std::move(request.user_agent),
            .trace_id = std::move(request.trace_id),
            .success = request.success,
            .failure_reason = std::move(request.failure_reason),
            .duration_ms = request.duration_ms,
            .before_json = std::move(request.before_json),
            .after_json = std::move(request.after_json),
            .detail_json = std::move(request.detail_json),
            .created_at = std::chrono::system_clock::now(),
        };
        items_.push_back(entry);
        return entry;
    }

    std::vector<AuditLogEntry> list_recent(std::size_t limit) const override {
        std::scoped_lock lock{mutex_};
        std::vector<AuditLogEntry> items;
        const auto capped = std::min(limit, items_.size());
        items.reserve(capped);
        for (auto it = items_.rbegin(); it != items_.rend() && items.size() < capped; ++it) {
            items.push_back(*it);
        }
        return items;
    }

    // 在内存版里直接复用现有容器做一次筛选计数，
    // 让调用方拿到和 MySQL 版一致的分页 total 语义。
    std::size_t count_filtered(
        const std::optional<std::string>& action_filter,
        const std::optional<std::string>& operator_filter,
        const std::optional<bool>& success_filter) const override {
        std::vector<AuditLogEntry> items;
        {
            std::scoped_lock lock{mutex_};
            items.reserve(items_.size());
            for (auto it = items_.rbegin(); it != items_.rend(); ++it) {
                items.push_back(*it);
            }
        }
        if (action_filter) {
            items.erase(
                std::remove_if(items.begin(), items.end(), [&](const AuditLogEntry& item) {
                    return item.action.find(*action_filter) == std::string::npos;
                }),
                items.end());
        }
        if (operator_filter) {
            items.erase(
                std::remove_if(items.begin(), items.end(), [&](const AuditLogEntry& item) {
                    return item.operator_id.find(*operator_filter) == std::string::npos;
                }),
                items.end());
        }
        if (success_filter) {
            items.erase(
                std::remove_if(items.begin(), items.end(), [&](const AuditLogEntry& item) {
                    return item.success != *success_filter;
                }),
                items.end());
        }
        return items.size();
    }

    AuditLogPage list_recent_filtered(
        std::size_t limit,
        const std::optional<AuditLogCursor>& cursor,
        const std::optional<std::string>& action_filter,
        const std::optional<std::string>& operator_filter,
        const std::optional<bool>& success_filter) const override {
        AuditLogPage page;
        const auto bounded_limit = std::clamp<std::size_t>(limit, 1, 500);
        std::vector<AuditLogEntry> items;
        {
            std::scoped_lock lock{mutex_};
            items.reserve(items_.size());
            for (auto it = items_.rbegin(); it != items_.rend(); ++it) {
                items.push_back(*it);
            }
        }
        if (action_filter) {
            items.erase(
                std::remove_if(items.begin(), items.end(), [&](const AuditLogEntry& item) {
                    return item.action.find(*action_filter) == std::string::npos;
                }),
                items.end());
        }
        if (operator_filter) {
            items.erase(
                std::remove_if(items.begin(), items.end(), [&](const AuditLogEntry& item) {
                    return item.operator_id.find(*operator_filter) == std::string::npos;
                }),
                items.end());
        }
        if (success_filter) {
            items.erase(
                std::remove_if(items.begin(), items.end(), [&](const AuditLogEntry& item) {
                    return item.success != *success_filter;
                }),
                items.end());
        }
        std::sort(items.begin(), items.end(), [](const AuditLogEntry& left, const AuditLogEntry& right) {
            const auto left_at = to_unix_seconds(left.created_at.value_or(std::chrono::system_clock::time_point{}));
            const auto right_at = to_unix_seconds(right.created_at.value_or(std::chrono::system_clock::time_point{}));
            if (left_at != right_at) {
                return left_at > right_at;
            }
            return left.audit_id > right.audit_id;
        });
        if (cursor) {
            items.erase(
                std::remove_if(items.begin(), items.end(), [&](const AuditLogEntry& item) {
                    const auto item_at = to_unix_seconds(item.created_at.value_or(std::chrono::system_clock::time_point{}));
                    if (item_at < cursor->last_created_at) {
                        return false;
                    }
                    if (item_at > cursor->last_created_at) {
                        return true;
                    }
                    return item.audit_id >= cursor->last_audit_id;
                }),
                items.end());
        }
        if (items.size() > bounded_limit) {
            items.resize(bounded_limit);
            page.has_more = true;
        }
        if (page.has_more && !items.empty()) {
            const auto& oldest = items.back();
            page.next_created_at = to_unix_seconds(oldest.created_at.value_or(std::chrono::system_clock::time_point{}));
            page.next_audit_id = oldest.audit_id;
        }
        page.items = std::move(items);
        return page;
    }

private:
    mutable std::mutex mutex_;
    std::size_t next_id_ = 1;
    std::vector<AuditLogEntry> items_;
};

class InMemoryAdminRepository final : public AdminRepository {
public:
    AdminPrincipal upsert_admin(
        const std::string& account_id,
        std::string operator_name,
        bool active,
        std::vector<std::string> permissions) override {
        std::scoped_lock lock{mutex_};
        auto& admin = admins_by_account_id_[account_id];
        admin.account_id = account_id;
        admin.operator_name = std::move(operator_name);
        admin.active = active;
        admin.permissions = std::move(permissions);
        admin.created_at = admin.created_at.value_or(std::chrono::system_clock::now());
        admin.updated_at = std::chrono::system_clock::now();
        return admin;
    }

    std::optional<AdminPrincipal> find_by_account_id(const std::string& account_id) const override {
        std::scoped_lock lock{mutex_};
        if (const auto it = admins_by_account_id_.find(account_id); it != admins_by_account_id_.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    std::vector<AdminPrincipal> list_all() const override {
        std::scoped_lock lock{mutex_};
        std::vector<AdminPrincipal> items;
        items.reserve(admins_by_account_id_.size());
        for (const auto& [_, admin] : admins_by_account_id_) {
            items.push_back(admin);
        }
        std::sort(
            items.begin(),
            items.end(),
            [](const AdminPrincipal& left, const AdminPrincipal& right) { return left.account_id < right.account_id; });
        return items;
    }

    // 内存版同样返回精确筛选总数，
    // 这样接口行为不会因为底层存储不同而变化。
    std::size_t count_filtered(
        const std::optional<std::string>& operator_name_filter,
        const std::optional<bool>& active_filter) const override {
        auto items = list_all();
        if (operator_name_filter) {
            items.erase(
                std::remove_if(items.begin(), items.end(), [&](const AdminPrincipal& item) {
                    return item.operator_name.find(*operator_name_filter) == std::string::npos;
                }),
                items.end());
        }
        if (active_filter) {
            items.erase(
                std::remove_if(items.begin(), items.end(), [&](const AdminPrincipal& item) {
                    return item.active != *active_filter;
                }),
                items.end());
        }
        return items.size();
    }

    std::vector<AdminPrincipal> list_filtered(
        std::size_t limit,
        std::size_t offset,
        const std::optional<std::string>& operator_name_filter,
        const std::optional<bool>& active_filter) const override {
        auto items = list_all();
        if (operator_name_filter) {
            items.erase(
                std::remove_if(items.begin(), items.end(), [&](const AdminPrincipal& item) {
                    return item.operator_name.find(*operator_name_filter) == std::string::npos;
                }),
                items.end());
        }
        if (active_filter) {
            items.erase(
                std::remove_if(items.begin(), items.end(), [&](const AdminPrincipal& item) {
                    return item.active != *active_filter;
                }),
                items.end());
        }
        if (offset >= items.size()) {
            return {};
        }
        const auto end = std::min(items.size(), offset + limit);
        return std::vector<AdminPrincipal>(
            items.begin() + static_cast<std::ptrdiff_t>(offset),
            items.begin() + static_cast<std::ptrdiff_t>(end));
    }

    bool has_permission(const std::string& account_id, const std::string& permission) const override {
        std::scoped_lock lock{mutex_};
        const auto it = admins_by_account_id_.find(account_id);
        if (it == admins_by_account_id_.end() || !it->second.active) {
            return false;
        }
        return std::find(it->second.permissions.begin(), it->second.permissions.end(), permission) !=
            it->second.permissions.end();
    }

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, AdminPrincipal> admins_by_account_id_;
};

class InMemoryAdminTaskRepository final : public AdminTaskRepository {
public:
    AdminTask save(AdminTask task) override {
        std::scoped_lock lock{mutex_};
        if (task.task_id.empty()) {
            task.task_id = "admin_task_" + std::to_string(next_id_++);
        }
        task.created_at = task.created_at.value_or(std::chrono::system_clock::now());
        task.updated_at = std::chrono::system_clock::now();
        tasks_by_id_[task.task_id] = task;
        return task;
    }

    AdminTaskPage list_recent(
        std::size_t limit,
        const std::optional<AdminTaskCursor>& cursor,
        const std::optional<AdminTaskKind>& kind,
        const std::optional<std::string>& status) const override {
        AdminTaskPage page;
        const auto bounded_limit = std::clamp<std::size_t>(limit, 1, 500);
        std::vector<AdminTask> items;
        {
            std::scoped_lock lock{mutex_};
            items.reserve(tasks_by_id_.size());
            for (const auto& [_, task] : tasks_by_id_) {
                if (kind && task.kind != *kind) {
                    continue;
                }
                if (status && task.status != *status) {
                    continue;
                }
                items.push_back(task);
            }
        }
        std::sort(items.begin(), items.end(), [](const AdminTask& left, const AdminTask& right) {
            const auto left_at = to_unix_seconds(left.created_at.value_or(std::chrono::system_clock::time_point{}));
            const auto right_at = to_unix_seconds(right.created_at.value_or(std::chrono::system_clock::time_point{}));
            if (left_at != right_at) {
                return left_at > right_at;
            }
            return left.task_id > right.task_id;
        });
        if (cursor) {
            items.erase(
                std::remove_if(items.begin(), items.end(), [&](const AdminTask& item) {
                    const auto item_at = to_unix_seconds(item.created_at.value_or(std::chrono::system_clock::time_point{}));
                    if (item_at < cursor->last_created_at) {
                        return false;
                    }
                    if (item_at > cursor->last_created_at) {
                        return true;
                    }
                    return item.task_id >= cursor->last_task_id;
                }),
                items.end());
        }
        if (items.size() > bounded_limit) {
            items.resize(bounded_limit);
            page.has_more = true;
        }
        if (page.has_more && !items.empty()) {
            const auto& oldest = items.back();
            page.next_created_at = to_unix_seconds(oldest.created_at.value_or(std::chrono::system_clock::time_point{}));
            page.next_task_id = oldest.task_id;
        }
        page.items = std::move(items);
        return page;
    }

    std::size_t count_filtered(
        const std::optional<AdminTaskKind>& kind,
        const std::optional<std::string>& status) const override {
        std::scoped_lock lock{mutex_};
        std::size_t count = 0;
        for (const auto& [_, task] : tasks_by_id_) {
            if (kind && task.kind != *kind) {
                continue;
            }
            if (status && task.status != *status) {
                continue;
            }
            ++count;
        }
        return count;
    }

    std::optional<AdminTask> find_by_id(const std::string& task_id) const override {
        std::scoped_lock lock{mutex_};
        const auto it = tasks_by_id_.find(task_id);
        if (it == tasks_by_id_.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    std::vector<AdminTask> claim_due_tasks(
        AdminTaskKind kind,
        std::chrono::system_clock::time_point now,
        std::size_t limit) override {
        std::scoped_lock lock{mutex_};
        std::vector<AdminTask> items;
        for (auto& [_, task] : tasks_by_id_) {
            if (task.kind != kind || task.status != "scheduled" || !task.execute_at || *task.execute_at > now) {
                continue;
            }
            task.status = "running";
            task.updated_at = now;
            items.push_back(task);
            if (items.size() >= limit) {
                break;
            }
        }
        return items;
    }

    std::optional<AdminTask> mark_completed(
        const std::string& task_id,
        std::string result_json,
        std::chrono::system_clock::time_point executed_at) override {
        std::scoped_lock lock{mutex_};
        const auto it = tasks_by_id_.find(task_id);
        if (it == tasks_by_id_.end()) {
            return std::nullopt;
        }
        it->second.status = "completed";
        it->second.result_json = std::move(result_json);
        it->second.executed_at = executed_at;
        it->second.updated_at = executed_at;
        return it->second;
    }

    std::optional<AdminTask> mark_failed(
        const std::string& task_id,
        std::string failure_reason,
        std::string result_json,
        std::chrono::system_clock::time_point executed_at) override {
        std::scoped_lock lock{mutex_};
        const auto it = tasks_by_id_.find(task_id);
        if (it == tasks_by_id_.end()) {
            return std::nullopt;
        }
        it->second.status = "failed";
        it->second.failure_reason = std::move(failure_reason);
        it->second.result_json = std::move(result_json);
        it->second.executed_at = executed_at;
        it->second.updated_at = executed_at;
        return it->second;
    }

private:
    mutable std::mutex mutex_;
    std::size_t next_id_ = 1;
    std::unordered_map<std::string, AdminTask> tasks_by_id_;
};

class InMemoryVoiceRepository final : public mf::voice::VoiceRepository {
public:
    mf::voice::VoiceRoomMember upsert_member(mf::voice::VoiceRoomMember member) override {
        std::scoped_lock lock{mutex_};
        const auto key = member.room_id + '|' + member.player_id;
        const auto now = std::chrono::system_clock::now();
        if (const auto it = members_.find(key); it != members_.end()) {
            member.server_muted = it->second.server_muted;
            member.banned = it->second.banned;
            member.joined_at = it->second.joined_at;
        } else {
            member.joined_at = now;
        }
        member.updated_at = now;
        members_[key] = member;
        return member;
    }

    std::optional<mf::voice::VoiceRoomMember> find_member(
        const std::string& room_id,
        const std::string& player_id) const override {
        std::scoped_lock lock{mutex_};
        if (const auto it = members_.find(room_id + '|' + player_id); it != members_.end()) return it->second;
        return std::nullopt;
    }

    std::vector<mf::voice::VoiceRoomMember> list_members(const std::string& room_id) const override {
        std::scoped_lock lock{mutex_};
        std::vector<mf::voice::VoiceRoomMember> values;
        for (const auto& [key, member] : members_) {
            (void)key;
            if (member.room_id == room_id) values.push_back(member);
        }
        return values;
    }

    bool remove_member(const std::string& room_id, const std::string& player_id) override {
        std::scoped_lock lock{mutex_};
        return members_.erase(room_id + '|' + player_id) > 0;
    }

    std::optional<mf::voice::VoiceRoomMember> set_moderation(
        const std::string& room_id,
        const std::string& player_id,
        bool server_muted,
        bool banned) override {
        std::scoped_lock lock{mutex_};
        const auto it = members_.find(room_id + '|' + player_id);
        if (it == members_.end()) return std::nullopt;
        it->second.server_muted = server_muted;
        it->second.banned = banned;
        it->second.updated_at = std::chrono::system_clock::now();
        return it->second;
    }

    mf::voice::VoiceModerationEvent append_moderation_event(mf::voice::VoiceModerationEvent event) override {
        std::scoped_lock lock{mutex_};
        event.event_id = "voice_event_" + std::to_string(next_event_id_++);
        event.created_at = std::chrono::system_clock::now();
        events_.push_back(event);
        return event;
    }

private:
    mutable std::mutex mutex_;
    std::size_t next_event_id_ = 1;
    std::unordered_map<std::string, mf::voice::VoiceRoomMember> members_;
    std::vector<mf::voice::VoiceModerationEvent> events_;
};

class InMemoryMultiplayerRoomRepository final : public mf::multiplayer::MultiplayerRoomRepository {
public:
    mf::multiplayer::RoomMutationResult create_room(
        mf::multiplayer::MultiplayerRoom room,
        mf::multiplayer::MultiplayerRoomMember owner) override {
        std::scoped_lock lock{mutex_};
        if (rooms_.contains(room.room_id)) {
            return {.status = mf::multiplayer::RoomMutationStatus::room_exists};
        }
        const auto now = std::chrono::system_clock::now();
        room.created_at = now;
        room.updated_at = now;
        owner.joined_at = now;
        rooms_.emplace(room.room_id, room);
        members_.emplace(member_key(room.room_id, owner.player_id), owner);
        return {
            .status = mf::multiplayer::RoomMutationStatus::success,
            .room = room,
            .member = owner,
        };
    }

    mf::multiplayer::RoomMutationResult join_room(
        const std::string& room_id,
        mf::multiplayer::MultiplayerRoomMember member) override {
        std::scoped_lock lock{mutex_};
        const auto room_it = rooms_.find(room_id);
        if (room_it == rooms_.end()) return {.status = mf::multiplayer::RoomMutationStatus::room_not_found};
        if (room_it->second.status != "open") return {.status = mf::multiplayer::RoomMutationStatus::room_closed};
        const auto key = member_key(room_id, member.player_id);
        if (members_.contains(key)) return {.status = mf::multiplayer::RoomMutationStatus::already_member};
        const auto member_count = static_cast<std::int64_t>(std::count_if(
            members_.begin(), members_.end(), [&](const auto& entry) { return entry.second.room_id == room_id; }));
        if (member_count >= room_it->second.max_players) {
            return {.status = mf::multiplayer::RoomMutationStatus::room_full};
        }
        member.joined_at = std::chrono::system_clock::now();
        members_.emplace(key, member);
        room_it->second.updated_at = std::chrono::system_clock::now();
        return {
            .status = mf::multiplayer::RoomMutationStatus::success,
            .room = room_it->second,
            .member = member,
        };
    }

    mf::multiplayer::RoomMutationResult leave_room(
        const std::string& room_id,
        const std::string& player_id) override {
        std::scoped_lock lock{mutex_};
        const auto room_it = rooms_.find(room_id);
        if (room_it == rooms_.end()) return {.status = mf::multiplayer::RoomMutationStatus::room_not_found};
        if (room_it->second.owner_player_id == player_id) {
            return {.status = mf::multiplayer::RoomMutationStatus::owner_must_close};
        }
        const auto member_it = members_.find(member_key(room_id, player_id));
        if (member_it == members_.end()) return {.status = mf::multiplayer::RoomMutationStatus::member_not_found};
        auto removed = member_it->second;
        members_.erase(member_it);
        room_it->second.updated_at = std::chrono::system_clock::now();
        return {
            .status = mf::multiplayer::RoomMutationStatus::success,
            .room = room_it->second,
            .member = removed,
            .affected_members = {removed},
        };
    }

    mf::multiplayer::RoomMutationResult close_room(
        const std::string& room_id,
        const std::string& requester_player_id) override {
        std::scoped_lock lock{mutex_};
        const auto room_it = rooms_.find(room_id);
        if (room_it == rooms_.end()) return {.status = mf::multiplayer::RoomMutationStatus::room_not_found};
        if (room_it->second.owner_player_id != requester_player_id) {
            return {.status = mf::multiplayer::RoomMutationStatus::permission_denied};
        }
        std::vector<mf::multiplayer::MultiplayerRoomMember> removed;
        for (auto it = members_.begin(); it != members_.end();) {
            if (it->second.room_id == room_id) {
                removed.push_back(it->second);
                it = members_.erase(it);
            } else {
                ++it;
            }
        }
        room_it->second.status = "closed";
        room_it->second.updated_at = std::chrono::system_clock::now();
        return {
            .status = mf::multiplayer::RoomMutationStatus::success,
            .room = room_it->second,
            .affected_members = std::move(removed),
        };
    }

    std::optional<mf::multiplayer::MultiplayerRoom> find_room(const std::string& room_id) const override {
        std::scoped_lock lock{mutex_};
        if (const auto it = rooms_.find(room_id); it != rooms_.end()) return it->second;
        return std::nullopt;
    }

    std::optional<mf::multiplayer::MultiplayerRoomMember> find_member(
        const std::string& room_id,
        const std::string& player_id) const override {
        std::scoped_lock lock{mutex_};
        if (const auto it = members_.find(member_key(room_id, player_id)); it != members_.end()) return it->second;
        return std::nullopt;
    }

    std::vector<mf::multiplayer::MultiplayerRoomMember> list_members(const std::string& room_id) const override {
        std::scoped_lock lock{mutex_};
        std::vector<mf::multiplayer::MultiplayerRoomMember> values;
        for (const auto& [key, member] : members_) {
            (void)key;
            if (member.room_id == room_id) values.push_back(member);
        }
        std::sort(values.begin(), values.end(), [](const auto& left, const auto& right) {
            if (left.role != right.role) return left.role == "leader";
            return left.player_id < right.player_id;
        });
        return values;
    }

private:
    static std::string member_key(const std::string& room_id, const std::string& player_id) {
        return room_id + '|' + player_id;
    }

    mutable std::mutex mutex_;
    std::unordered_map<std::string, mf::multiplayer::MultiplayerRoom> rooms_;
    std::unordered_map<std::string, mf::multiplayer::MultiplayerRoomMember> members_;
};

class InMemoryQuestRepository final : public QuestRepository {
public:
    InMemoryQuestRepository(
        std::shared_ptr<PlayerRepository> players,
        std::shared_ptr<RewardRepository> rewards)
        : players_(std::move(players)), rewards_(std::move(rewards)) {}

    QuestDefinition upsert_definition(QuestDefinition definition) override {
        std::scoped_lock lock{mutex_};
        const auto now = std::chrono::system_clock::now();
        if (!definition.created_at) definition.created_at = now;
        definition.updated_at = now;
        definitions_[definition.quest_id] = definition;
        return definition;
    }

    std::vector<QuestDefinition> list_definitions(bool include_disabled) const override {
        MF_PROFILE_SCOPE("Memory quest definitions");
        std::scoped_lock lock{mutex_};
        std::vector<QuestDefinition> values;
        for (const auto& [id, definition] : definitions_) {
            (void)id;
            if (include_disabled || definition.enabled) values.push_back(definition);
        }
        std::sort(values.begin(), values.end(), [](const auto& left, const auto& right) {
            return left.quest_id < right.quest_id;
        });
        return values;
    }

    std::optional<QuestDefinition> find_definition(const std::string& quest_id) const override {
        std::scoped_lock lock{mutex_};
        if (const auto it = definitions_.find(quest_id); it != definitions_.end()) return it->second;
        return std::nullopt;
    }

    QuestSnapshot get_snapshot(const std::string& player_id) const override {
        MF_PROFILE_SCOPE("Memory quest snapshot");
        std::scoped_lock lock{mutex_};
        return snapshot_unlocked(player_id);
    }

    std::optional<PlayerQuest> accept(
        const std::string& player_id,
        const std::string& quest_id,
        const std::string&,
        std::string& failure_code) override {
        std::scoped_lock lock{mutex_};
        const auto definition_it = definitions_.find(quest_id);
        if (definition_it == definitions_.end() || !definition_it->second.enabled) {
            failure_code = "quest_not_found";
            return std::nullopt;
        }
        const auto key = quest_key(player_id, quest_id);
        if (const auto existing = player_quests_.find(key); existing != player_quests_.end()) return existing->second;
        for (const auto& prerequisite : definition_it->second.pre_quest_ids) {
            const auto prerequisite_it = player_quests_.find(quest_key(player_id, prerequisite));
            if (prerequisite_it == player_quests_.end() || prerequisite_it->second.state != QuestState::claimed) {
                failure_code = "quest_condition_not_met";
                return std::nullopt;
            }
        }
        for (const auto& flag : definition_it->second.required_flags) {
            if (!story_flags_[player_id].contains(flag)) {
                failure_code = "quest_condition_not_met";
                return std::nullopt;
            }
        }
        PlayerQuest quest{
            .player_id = player_id,
            .quest_id = quest_id,
            .quest_version = definition_it->second.version,
            .accepted_at = std::chrono::system_clock::now(),
        };
        for (const auto& objective : definition_it->second.objectives) {
            quest.objectives.push_back({
                .objective_id = objective.objective_id,
                .target_value = objective.target_value,
            });
        }
        player_quests_[key] = quest;
        return quest;
    }

    QuestEventResult apply_event(
        const std::string& player_id,
        const std::string& event_id,
        const std::string& event_type,
        const std::string& target_id,
        std::int64_t amount,
        const std::string&,
        std::string& failure_code) override {
        std::scoped_lock lock{mutex_};
        QuestEventResult result;
        if (!event_ids_[player_id].insert(event_id).second) {
            result.duplicate_event = true;
            result.snapshot = snapshot_unlocked(player_id);
            return result;
        }
        bool matched = false;
        for (auto& [key, quest] : player_quests_) {
            (void)key;
            if (quest.player_id != player_id || quest.state != QuestState::accepted) continue;
            const auto definition_it = definitions_.find(quest.quest_id);
            if (definition_it == definitions_.end()) continue;
            bool quest_matched = false;
            for (const auto& definition_objective : definition_it->second.objectives) {
                if (definition_objective.event_type != event_type ||
                    (!definition_objective.target_id.empty() && definition_objective.target_id != target_id)) continue;
                const auto objective_it = std::find_if(quest.objectives.begin(), quest.objectives.end(), [&](const auto& value) {
                    return value.objective_id == definition_objective.objective_id;
                });
                if (objective_it == quest.objectives.end()) continue;
                objective_it->current_value = std::min(objective_it->target_value, objective_it->current_value + amount);
                matched = true;
                quest_matched = true;
            }
            if (quest_matched && std::all_of(quest.objectives.begin(), quest.objectives.end(), [](const auto& objective) {
                    return objective.current_value >= objective.target_value;
                })) {
                quest.state = QuestState::completed;
                quest.completed_at = std::chrono::system_clock::now();
                if (!definition_it->second.completion_flag.empty()) {
                    story_flags_[player_id].insert(definition_it->second.completion_flag);
                }
                if (definition_it->second.claim_mode == QuestClaimMode::automatic) {
                    auto automatic = claim_unlocked(player_id, definition_it->second, "auto:" + event_id, "");
                    if (automatic) {
                        result.automatic_grants.push_back(automatic->grant);
                        result.player = automatic->player;
                    }
                }
            }
            if (quest_matched) ++quest.progress_version;
        }
        if (!matched) {
            event_ids_[player_id].erase(event_id);
            failure_code = "quest_event_rejected";
            return result;
        }
        result.snapshot = snapshot_unlocked(player_id);
        return result;
    }

    std::optional<QuestClaimResult> claim(
        const std::string& player_id,
        const std::string& quest_id,
        const std::string& request_id,
        const std::string& npc_id,
        std::string& failure_code) override {
        std::scoped_lock lock{mutex_};
        const auto definition_it = definitions_.find(quest_id);
        if (definition_it == definitions_.end()) {
            failure_code = "quest_not_found";
            return std::nullopt;
        }
        const auto quest_it = player_quests_.find(quest_key(player_id, quest_id));
        if (quest_it == player_quests_.end()) {
            failure_code = "quest_not_accepted";
            return std::nullopt;
        }
        if (quest_it->second.state == QuestState::claimed) {
            auto repeated = claim_unlocked(player_id, definition_it->second, request_id, npc_id);
            if (repeated) repeated->already_claimed = true;
            return repeated;
        }
        if (quest_it->second.state != QuestState::completed) {
            failure_code = "quest_not_completed";
            return std::nullopt;
        }
        if (definition_it->second.claim_mode == QuestClaimMode::npc &&
            definition_it->second.turn_in_npc_id != npc_id) {
            failure_code = "quest_npc_mismatch";
            return std::nullopt;
        }
        return claim_unlocked(player_id, definition_it->second, request_id, npc_id);
    }

private:
    static std::string quest_key(const std::string& player_id, const std::string& quest_id) {
        return player_id + '|' + quest_id;
    }

    QuestSnapshot snapshot_unlocked(const std::string& player_id) const {
        QuestSnapshot snapshot;
        for (const auto& [key, quest] : player_quests_) {
            (void)key;
            if (quest.player_id == player_id) snapshot.quests.push_back(quest);
        }
        if (const auto flags = story_flags_.find(player_id); flags != story_flags_.end()) {
            snapshot.story_flags.assign(flags->second.begin(), flags->second.end());
        }
        return snapshot;
    }

    std::optional<QuestClaimResult> claim_unlocked(
        const std::string& player_id,
        const QuestDefinition& definition,
        const std::string&,
        const std::string&) {
        const auto grant_id = "quest:" + player_id + ':' + definition.quest_id;
        const auto existing = rewards_->find_by_grant_id(grant_id);
        const auto player = existing
                                ? players_->find_by_player_id(player_id)
                                : players_->apply_rewards(player_id, definition.rewards, grant_id);
        if (!player) return std::nullopt;
        const auto grant = existing.value_or(rewards_->record_grant(
            grant_id, player_id, "quest", definition.quest_id, definition.rewards));
        auto& quest = player_quests_.at(quest_key(player_id, definition.quest_id));
        if (!existing) {
            quest.state = QuestState::claimed;
            if (!quest.claimed_at) quest.claimed_at = std::chrono::system_clock::now();
            ++quest.progress_version;
        }
        return QuestClaimResult{
            .quest = quest,
            .grant = grant,
            .player = *player,
            .already_claimed = existing.has_value(),
        };
    }

    std::shared_ptr<PlayerRepository> players_;
    std::shared_ptr<RewardRepository> rewards_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, QuestDefinition> definitions_;
    std::unordered_map<std::string, PlayerQuest> player_quests_;
    std::unordered_map<std::string, std::unordered_set<std::string>> story_flags_;
    std::unordered_map<std::string, std::unordered_set<std::string>> event_ids_;
};

}  // 匿名命名空间

RepositoryBundle make_in_memory_repositories() {
    // 把仓储构造集中在一个地方，
    // 可以让应用以后更容易从内存演示模式切换到 SQL/Redis 后端。
    const auto players = std::make_shared<InMemoryPlayerRepository>();
    const auto rewards = std::make_shared<InMemoryRewardRepository>();
    return RepositoryBundle{
        .accounts = std::make_shared<InMemoryAccountRepository>(),
        .agent = std::make_shared<InMemoryAgentRepository>(),
        .players = players,
        .item_catalog = std::make_shared<InMemoryItemCatalogRepository>(),
        .gacha = std::make_shared<InMemoryGachaRepository>(),
        .gacha_transactions = nullptr,
        .shop = std::make_shared<InMemoryShopRepository>(),
        .loot = std::make_shared<InMemoryLootRepository>(),
        .quests = std::make_shared<InMemoryQuestRepository>(players, rewards),
        .multiplayer_rooms = std::make_shared<InMemoryMultiplayerRoomRepository>(),
        .voice = std::make_shared<InMemoryVoiceRepository>(),
        .friends = std::make_shared<InMemoryFriendRepository>(),
        .sessions = std::make_shared<InMemorySessionRepository>(),
        .chat = std::make_shared<InMemoryChatRepository>(),
        .chat_moderation = std::make_shared<InMemoryChatModerationRepository>(),
        .auth_security = std::make_shared<InMemoryAuthSecurityRepository>(),
        .rewards = rewards,
        .reward_templates = std::make_shared<InMemoryRewardTemplateRepository>(),
        .announcements = std::make_shared<InMemoryAnnouncementRepository>(),
        .mails = std::make_shared<InMemoryMailRepository>(),
        .audit_logs = std::make_shared<InMemoryAuditLogRepository>(),
        .admins = std::make_shared<InMemoryAdminRepository>(),
        .admin_tasks = std::make_shared<InMemoryAdminTaskRepository>(),
    };
}

}  // namespace mf::game 命名空间
