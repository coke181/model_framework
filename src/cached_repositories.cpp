#include "mf/storage/cached_repositories.hpp"

// 文件职责：
// 1. 实现读多写少仓储的进程内缓存包装。
// 2. 先覆盖管理员权限与奖励模板，减少后台鉴权和奖励模板展开时的数据库往返。
// 3. 写操作成功后会同步刷新或失效本地缓存，保证当前进程内读到的是新数据。

#include <algorithm>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mf::storage {
namespace {

template <class T>
struct OptionalCacheEntry {
    bool loaded = false;
    std::optional<T> value;
};

class CachedAdminRepository final : public mf::game::AdminRepository {
public:
    explicit CachedAdminRepository(std::shared_ptr<mf::game::AdminRepository> inner)
        : inner_(std::move(inner)) {}

    mf::game::AdminPrincipal upsert_admin(
        const std::string& account_id,
        std::string operator_name,
        bool active,
        std::vector<std::string> permissions) override {
        auto admin = inner_->upsert_admin(account_id, std::move(operator_name), active, std::move(permissions));
        std::scoped_lock lock{mutex_};
        by_account_id_[account_id] = OptionalCacheEntry<mf::game::AdminPrincipal>{
            .loaded = true,
            .value = admin,
        };
        list_all_loaded_ = false;
        return admin;
    }

    std::optional<mf::game::AdminPrincipal> find_by_account_id(const std::string& account_id) const override {
        {
            std::scoped_lock lock{mutex_};
            if (const auto it = by_account_id_.find(account_id); it != by_account_id_.end() && it->second.loaded) {
                return it->second.value;
            }
        }

        auto admin = inner_->find_by_account_id(account_id);
        std::scoped_lock lock{mutex_};
        by_account_id_[account_id] = OptionalCacheEntry<mf::game::AdminPrincipal>{
            .loaded = true,
            .value = admin,
        };
        return admin;
    }

    std::vector<mf::game::AdminPrincipal> list_all() const override {
        {
            std::scoped_lock lock{mutex_};
            if (list_all_loaded_) {
                return list_all_cache_;
            }
        }

        auto items = inner_->list_all();
        std::scoped_lock lock{mutex_};
        list_all_cache_ = items;
        list_all_loaded_ = true;
        for (const auto& item : items) {
            by_account_id_[item.account_id] = OptionalCacheEntry<mf::game::AdminPrincipal>{
                .loaded = true,
                .value = item,
            };
        }
        return items;
    }

    std::size_t count_filtered(
        const std::optional<std::string>& operator_name_filter,
        const std::optional<bool>& active_filter) const override {
        return inner_->count_filtered(operator_name_filter, active_filter);
    }

    std::vector<mf::game::AdminPrincipal> list_filtered(
        std::size_t limit,
        std::size_t offset,
        const std::optional<std::string>& operator_name_filter,
        const std::optional<bool>& active_filter) const override {
        return inner_->list_filtered(limit, offset, operator_name_filter, active_filter);
    }

    bool has_permission(const std::string& account_id, const std::string& permission) const override {
        if (const auto admin = find_by_account_id(account_id)) {
            if (!admin->active) {
                return false;
            }
            return std::find(admin->permissions.begin(), admin->permissions.end(), permission) !=
                admin->permissions.end();
        }
        return false;
    }

private:
    std::shared_ptr<mf::game::AdminRepository> inner_;
    mutable std::mutex mutex_;
    mutable std::unordered_map<std::string, OptionalCacheEntry<mf::game::AdminPrincipal>> by_account_id_;
    mutable bool list_all_loaded_ = false;
    mutable std::vector<mf::game::AdminPrincipal> list_all_cache_;
};

class CachedRewardTemplateRepository final : public mf::game::RewardTemplateRepository {
public:
    explicit CachedRewardTemplateRepository(std::shared_ptr<mf::game::RewardTemplateRepository> inner)
        : inner_(std::move(inner)) {}

    mf::game::RewardTemplate upsert_template(
        std::string template_id,
        std::string name,
        std::vector<mf::game::RewardEntry> rewards) override {
        auto item = inner_->upsert_template(std::move(template_id), std::move(name), std::move(rewards));
        std::scoped_lock lock{mutex_};
        by_template_id_[item.template_id] = OptionalCacheEntry<mf::game::RewardTemplate>{
            .loaded = true,
            .value = item,
        };
        list_all_loaded_ = false;
        return item;
    }

    std::optional<mf::game::RewardTemplate> find_by_template_id(const std::string& template_id) const override {
        {
            std::scoped_lock lock{mutex_};
            if (const auto it = by_template_id_.find(template_id);
                it != by_template_id_.end() && it->second.loaded) {
                return it->second.value;
            }
        }

        auto item = inner_->find_by_template_id(template_id);
        std::scoped_lock lock{mutex_};
        by_template_id_[template_id] = OptionalCacheEntry<mf::game::RewardTemplate>{
            .loaded = true,
            .value = item,
        };
        return item;
    }

    std::vector<mf::game::RewardTemplate> list_all() const override {
        {
            std::scoped_lock lock{mutex_};
            if (list_all_loaded_) {
                return list_all_cache_;
            }
        }

        auto items = inner_->list_all();
        std::scoped_lock lock{mutex_};
        list_all_cache_ = items;
        list_all_loaded_ = true;
        for (const auto& item : items) {
            by_template_id_[item.template_id] = OptionalCacheEntry<mf::game::RewardTemplate>{
                .loaded = true,
                .value = item,
            };
        }
        return items;
    }

private:
    std::shared_ptr<mf::game::RewardTemplateRepository> inner_;
    mutable std::mutex mutex_;
    mutable std::unordered_map<std::string, OptionalCacheEntry<mf::game::RewardTemplate>> by_template_id_;
    mutable bool list_all_loaded_ = false;
    mutable std::vector<mf::game::RewardTemplate> list_all_cache_;
};

}  // namespace

std::shared_ptr<mf::game::AdminRepository> make_cached_admin_repository(
    std::shared_ptr<mf::game::AdminRepository> inner) {
    return std::make_shared<CachedAdminRepository>(std::move(inner));
}

std::shared_ptr<mf::game::RewardTemplateRepository> make_cached_reward_template_repository(
    std::shared_ptr<mf::game::RewardTemplateRepository> inner) {
    return std::make_shared<CachedRewardTemplateRepository>(std::move(inner));
}

}  // namespace mf::storage
