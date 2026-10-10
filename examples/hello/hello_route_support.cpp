#include "hello_route_support.hpp"

#include "mf/chat/channels.hpp"

#include <boost/json/parse.hpp>
#include <boost/json/value_to.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <random>
#include <sstream>
#include <unordered_map>

// 本文件职责：
// 1. 提供多个路由模块共享的辅助函数实现。
// 2. 覆盖 session/admin 鉴权、JSON 字段读取、审计追加、聊天频道解析、奖励数组解析等通用逻辑。
// 3. 让各个路由文件只关注业务分发，不重复堆样板代码。
//
// 文件调用流程：
// 1. 各个 hello_*_routes.cpp 通过 hello_route_support.hpp 引入这些声明。
// 2. 请求进入路由处理函数后，优先调用这里的 helper 做鉴权、参数解析和公共拼装。
// 3. helper 完成通用步骤后，再把控制流交回具体路由函数。
// 4. 路由函数继续调用服务层或仓储层完成业务处理。
// 5. 最终由路由模块返回统一 JSON 响应。

namespace {

[[noreturn]] void throw_field_error(
    std::string_view code,
    std::string_view message,
    std::string_view key,
    boost::json::object details = {}) {
    details["field"] = std::string(key);
    throw mf::core::ApiException(mf::core::ApiError{
        .status = boost::beast::http::status::bad_request,
        .code = std::string(code),
        .message = std::string(message),
        .details = std::move(details),
    });
}

const boost::json::value* find_required_field(const boost::json::object& object, std::string_view key) {
    if (const auto* value = object.if_contains(key)) {
        return value;
    }
    throw_field_error(
        "missing_required_field",
        "缺少必填字段",
        key);
}

std::string json_type_name(const boost::json::value& value) {
    switch (value.kind()) {
    case boost::json::kind::null:
        return "null";
    case boost::json::kind::bool_:
        return "bool";
    case boost::json::kind::int64:
        return "int64";
    case boost::json::kind::uint64:
        return "uint64";
    case boost::json::kind::double_:
        return "double";
    case boost::json::kind::string:
        return "string";
    case boost::json::kind::array:
        return "array";
    case boost::json::kind::object:
        return "object";
    }
    return "unknown";
}

void ensure_value_type(const boost::json::value& value, std::string_view key, boost::json::kind expected_kind) {
    if (value.kind() == expected_kind) {
        return;
    }
    const auto expected_type = expected_kind == boost::json::kind::bool_   ? "bool"
                             : expected_kind == boost::json::kind::int64 ? "int64"
                             : expected_kind == boost::json::kind::string ? "string"
                             : expected_kind == boost::json::kind::array  ? "array"
                                                                          : "value";
    const auto expected_type_label = expected_kind == boost::json::kind::bool_   ? "布尔值"
                                   : expected_kind == boost::json::kind::int64 ? "整数"
                                   : expected_kind == boost::json::kind::string ? "字符串"
                                   : expected_kind == boost::json::kind::array  ? "数组"
                                                                                : "值";
    throw_field_error(
        "invalid_field_type",
        "字段类型不正确",
        key,
        {
            {"expected_type", expected_type},
            {"expected_type_label", expected_type_label},
            {"actual_type", json_type_name(value)},
        });
}

std::optional<mf::game::RewardKind> parse_reward_kind(std::string_view value) {
    // 把请求里的字符串奖励类型解析成领域枚举，
    // 避免后面的业务层继续直接依赖裸字符串。
    if (value == "gold") {
        return mf::game::RewardKind::gold;
    }
    if (value == "currency") {
        return mf::game::RewardKind::currency;
    }
    if (value == "item") {
        return mf::game::RewardKind::item;
    }
    if (value == "attachment_template") {
        return mf::game::RewardKind::attachment_template;
    }
    return std::nullopt;
}

} // namespace

const mf::game::Session* require_session(
    const mf::http::Request& request,
    const mf::auth::TokenService& token_service,
    mf::game::Session& storage,
    mf::core::ApiError& error) {
    // 处理函数统一走这个辅助函数，把鉴权失败转换成一致的 API 错误结构，
    // 避免每个接口都重复解析请求头。
    const auto authorization = request[boost::beast::http::field::authorization];
    storage = token_service.authenticate_bearer(std::string{authorization}).value_or(mf::game::Session{});
    if (storage.token.empty()) {
        error = mf::core::ApiError{
            .status = boost::beast::http::status::unauthorized,
            .code = "unauthorized",
            .message = "缺少 Bearer Token，或 Token 无效",
        };
        return nullptr;
    }
    return &storage;
}

std::string read_string_field(const boost::json::object& object, std::string_view key) {
    const auto* value = find_required_field(object, key);
    ensure_value_type(*value, key, boost::json::kind::string);
    return boost::json::value_to<std::string>(*value);
}

std::string read_optional_string_field(const boost::json::object& object, std::string_view key) {
    if (const auto* value = object.if_contains(key)) {
        ensure_value_type(*value, key, boost::json::kind::string);
        return boost::json::value_to<std::string>(*value);
    }
    return {};
}

bool read_optional_bool_field(const boost::json::object& object, std::string_view key, bool default_value) {
    if (const auto* value = object.if_contains(key)) {
        ensure_value_type(*value, key, boost::json::kind::bool_);
        return value->as_bool();
    }
    return default_value;
}

std::int64_t read_optional_i64_field(
    const boost::json::object& object,
    std::string_view key,
    std::int64_t default_value) {
    if (const auto* value = object.if_contains(key)) {
        if (!value->is_int64() && !value->is_uint64()) {
            throw_field_error(
                "invalid_field_type",
                "字段类型不正确",
                key,
                {
                    {"expected_type", "int64"},
                    {"expected_type_label", "整数"},
                    {"actual_type", json_type_name(*value)},
                });
        }
        return value->is_int64() ? value->as_int64() : static_cast<std::int64_t>(value->as_uint64());
    }
    return default_value;
}

std::string client_ip_from_request(const mf::http::Request& request) {
    const auto forwarded = request["X-Forwarded-For"];
    if (!forwarded.empty()) {
        return std::string{forwarded};
    }
    const auto real_ip = request["X-Real-IP"];
    if (!real_ip.empty()) {
        return std::string{real_ip};
    }
    return "local";
}

std::string user_agent_from_request(const mf::http::Request& request) {
    return std::string{request[boost::beast::http::field::user_agent]};
}

std::string trace_id_from_request(const mf::http::Request& request) {
    if (const auto tracer = mf::core::global_tracer()) {
        if (const auto current = tracer->current_context()) {
            return current->trace_id;
        }
    }

    const auto traceparent = request["traceparent"];
    if (!traceparent.empty()) {
        if (const auto parsed = mf::core::TracerProvider::parse_traceparent(std::string_view{traceparent.data(), traceparent.size()})) {
            return parsed->trace_id;
        }
    }

    const auto provided = request["X-Trace-Id"];
    if (!provided.empty()) {
        return std::string{provided};
    }

    static std::mt19937_64 generator{std::random_device{}()};
    static std::mutex mutex;
    std::scoped_lock lock{mutex};
    std::ostringstream output;
    output << "trace_" << std::hex << generator() << generator();
    return output.str();
}

boost::json::object parse_json_or_empty_object(const std::string& payload) {
    if (payload.empty()) {
        return {};
    }
    const auto parsed = boost::json::parse(payload);
    if (!parsed.is_object()) {
        throw mf::core::ApiException(mf::core::ApiError{
            .status = boost::beast::http::status::bad_request,
            .code = "invalid_json_root_type",
            .message = "请求体顶层必须是 JSON 对象",
            .details = {{"expected_type", "object"}},
        });
    }
    return parsed.as_object();
}

mf::game::AuditLogEntry append_admin_audit(
    const std::shared_ptr<mf::game::AuditLogRepository>& audit_repository,
    const mf::http::Request& request,
    std::string operator_id,
    std::string action,
    std::string target_type,
    std::string target_id,
    std::string detail_json,
    bool success,
    std::string failure_reason,
    std::string before_json,
    std::string after_json,
    std::chrono::steady_clock::time_point started_at) {
    return audit_repository->append(mf::game::AuditLogWriteRequest{
        .operator_id = std::move(operator_id),
        .action = std::move(action),
        .target_type = std::move(target_type),
        .target_id = std::move(target_id),
        .request_ip = client_ip_from_request(request),
        .user_agent = user_agent_from_request(request),
        .trace_id = trace_id_from_request(request),
        .success = success,
        .failure_reason = std::move(failure_reason),
        .duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - started_at)
                           .count(),
        .before_json = std::move(before_json),
        .after_json = std::move(after_json),
        .detail_json = std::move(detail_json),
    });
}

std::vector<std::string> resolve_target_players(
    const boost::json::object& object,
    const std::shared_ptr<mf::game::PlayerRepository>& player_repository) {
    if (const auto* values = object.if_contains("player_ids")) {
        if (!values->is_array()) {
            throw_field_error(
                "invalid_field_type",
                "字段类型不正确",
                "player_ids",
                {
                    {"expected_type", "array"},
                    {"expected_type_label", "数组"},
                    {"actual_type", json_type_name(*values)},
                });
        }
        std::vector<std::string> player_ids;
        for (const auto& item : values->as_array()) {
            if (!item.is_string()) {
                throw mf::core::ApiException(mf::core::ApiError{
                    .status = boost::beast::http::status::bad_request,
                    .code = "invalid_array_item_type",
                    .message = "数组元素类型不正确",
                    .details =
                        {
                            {"field", "player_ids"},
                            {"expected_item_type", "string"},
                            {"expected_item_type_label", "字符串"},
                            {"actual_item_type", json_type_name(item)},
                        },
                });
            }
            player_ids.push_back(boost::json::value_to<std::string>(item));
        }
        return player_ids;
    }

    const auto all_players = player_repository->list_all();
    if (read_optional_bool_field(object, "all_players", false)) {
        std::vector<std::string> player_ids;
        player_ids.reserve(all_players.size());
        for (const auto& player : all_players) {
            player_ids.push_back(player.player_id);
        }
        return player_ids;
    }

    if (object.if_contains("rollout_mod") && object.if_contains("rollout_buckets")) {
        const auto mod = std::max<std::int64_t>(1, read_optional_i64_field(object, "rollout_mod", 1));
        const auto* rollout_buckets = find_required_field(object, "rollout_buckets");
        if (!rollout_buckets->is_array()) {
            throw_field_error(
                "invalid_field_type",
                "字段类型不正确",
                "rollout_buckets",
                {
                    {"expected_type", "array"},
                    {"expected_type_label", "数组"},
                    {"actual_type", json_type_name(*rollout_buckets)},
                });
        }
        std::vector<std::int64_t> buckets;
        for (const auto& item : rollout_buckets->as_array()) {
            if (!item.is_int64() && !item.is_uint64()) {
                throw mf::core::ApiException(mf::core::ApiError{
                    .status = boost::beast::http::status::bad_request,
                    .code = "invalid_array_item_type",
                    .message = "数组元素类型不正确",
                    .details =
                        {
                            {"field", "rollout_buckets"},
                            {"expected_item_type", "int64"},
                            {"expected_item_type_label", "整数"},
                            {"actual_item_type", json_type_name(item)},
                        },
                });
            }
            buckets.push_back(item.as_int64());
        }

        std::vector<std::string> player_ids;
        for (const auto& player : all_players) {
            const auto hash = static_cast<std::int64_t>(std::hash<std::string>{}(player.player_id) % mod);
            if (std::find(buckets.begin(), buckets.end(), hash) != buckets.end()) {
                player_ids.push_back(player.player_id);
            }
        }
        return player_ids;
    }

    return {};
}

std::optional<mf::game::AccountStatus> parse_account_status(std::string_view value) {
    if (value == "active") {
        return mf::game::AccountStatus::active;
    }
    if (value == "frozen") {
        return mf::game::AccountStatus::frozen;
    }
    if (value == "banned") {
        return mf::game::AccountStatus::banned;
    }
    return std::nullopt;
}

boost::json::array build_friend_presence_array(
    const std::vector<mf::game::FriendLink>& friends,
    const std::vector<mf::game::FriendPresence>& presence_items) {
    boost::json::array result;
    result.reserve(friends.size());

    std::unordered_map<std::string, mf::game::FriendPresence> presence_by_player_id;
    presence_by_player_id.reserve(presence_items.size());
    for (const auto& presence : presence_items) {
        presence_by_player_id.emplace(presence.player_id, presence);
    }

    for (const auto& friend_link : friends) {
        const auto it = presence_by_player_id.find(friend_link.friend_player_id);
        const auto online = it != presence_by_player_id.end() ? it->second.online : false;
        const auto connection_count = it != presence_by_player_id.end() ? it->second.connection_count : 0;

        result.push_back(boost::json::object{
            {"player_id", friend_link.player_id},
            {"friend_player_id", friend_link.friend_player_id},
            {"online", online},
            {"connection_count", connection_count},
        });
    }

    return result;
}

boost::json::array build_friend_overview_array(
    const std::string& self_player_id,
    const std::vector<mf::game::FriendLink>& friends,
    const std::shared_ptr<mf::game::PlayerRepository>& player_repository,
    const std::shared_ptr<mf::chat::ChatService>& chat_service,
    const std::shared_ptr<mf::chat::ChatStateService>& chat_state_service,
    std::size_t limit,
    std::size_t offset) {
    struct FriendOverviewItem {
        std::int64_t sort_sent_at = 0;
        std::size_t friend_index = 0;
        std::string friend_player_id;
    };

    std::vector<FriendOverviewItem> items;
    items.reserve(friends.size());

    std::vector<std::string> friend_channel_ids;
    std::vector<mf::game::ChatChannel> friend_channels;
    friend_channel_ids.reserve(friends.size());
    friend_channels.reserve(friends.size());
    for (const auto& friend_link : friends) {
        const auto channel = mf::chat::make_friend_channel(self_player_id, friend_link.friend_player_id);
        friend_channel_ids.push_back(channel.channel_id);
        friend_channels.push_back(std::move(channel));
    }
    std::unordered_map<std::string, mf::game::ChatMessage> latest_by_channel_id;
    latest_by_channel_id.reserve(friend_channel_ids.size());
    if (chat_service) {
        for (auto& message : chat_service->latest_messages(friend_channel_ids, 1)) {
            latest_by_channel_id.emplace(message.channel.channel_id, std::move(message));
        }
    }

    for (std::size_t index = 0; index < friends.size(); ++index) {
        const auto& friend_link = friends[index];
        const auto& channel = friend_channels[index];
        const auto latest_it = latest_by_channel_id.find(channel.channel_id);
        const auto message_sent_at =
            latest_it == latest_by_channel_id.end()
                ? 0
                : std::chrono::duration_cast<std::chrono::seconds>(
                      latest_it->second.sent_at.time_since_epoch())
                      .count();
        const auto last_sent_at =
            friend_link.last_sent_at > 0 ? friend_link.last_sent_at : message_sent_at;

        items.push_back(FriendOverviewItem{
            .sort_sent_at = last_sent_at,
            .friend_index = index,
            .friend_player_id = friend_link.friend_player_id,
        });
    }

    std::sort(
        items.begin(),
        items.end(),
        [](const FriendOverviewItem& left, const FriendOverviewItem& right) {
            if (left.sort_sent_at != right.sort_sent_at) {
                return left.sort_sent_at > right.sort_sent_at;
            }
            return left.friend_player_id < right.friend_player_id;
        });

    const auto page_begin = std::min(offset, items.size());
    const auto page_end = page_begin + std::min(limit, items.size() - page_begin);

    std::vector<std::string> page_friend_ids;
    std::vector<std::string> page_channel_ids;
    std::vector<std::string> nickname_fallback_ids;
    std::unordered_map<std::string, std::string> nickname_by_player_id;
    page_friend_ids.reserve(page_end - page_begin);
    page_channel_ids.reserve(page_end - page_begin);
    nickname_fallback_ids.reserve(page_end - page_begin);
    nickname_by_player_id.reserve(page_end - page_begin);
    for (std::size_t position = page_begin; position < page_end; ++position) {
        const auto& friend_link = friends[items[position].friend_index];
        page_friend_ids.push_back(friend_link.friend_player_id);
        page_channel_ids.push_back(friend_channels[items[position].friend_index].channel_id);
        if (friend_link.friend_nickname) {
            nickname_by_player_id.emplace(friend_link.friend_player_id, *friend_link.friend_nickname);
        } else {
            nickname_fallback_ids.push_back(friend_link.friend_player_id);
        }
    }

    if (player_repository && !nickname_fallback_ids.empty()) {
        for (const auto& summary : player_repository->find_summaries_by_player_ids(nickname_fallback_ids)) {
            nickname_by_player_id.emplace(summary.player_id, summary.nickname);
        }
    }

    std::unordered_map<std::string, mf::game::FriendPresence> presence_by_player_id;
    presence_by_player_id.reserve(page_friend_ids.size());
    std::unordered_map<std::string, std::int64_t> unread_by_channel_id;
    unread_by_channel_id.reserve(page_channel_ids.size());
    if (chat_state_service && !page_friend_ids.empty()) {
        const auto overview_state = chat_state_service->list_friend_overview_state(
            self_player_id, page_friend_ids, page_channel_ids);
        for (const auto& presence : overview_state.presence) {
            presence_by_player_id.emplace(presence.player_id, presence);
        }
        for (const auto& unread : overview_state.unread_counts) {
            unread_by_channel_id.emplace(unread.channel_id, unread.unread_count);
        }
    }

    boost::json::array result;
    result.reserve(page_end - page_begin);
    for (std::size_t position = page_begin; position < page_end; ++position) {
        const auto& item = items[position];
        const auto& friend_link = friends[item.friend_index];
        const auto& channel = friend_channels[item.friend_index];
        const auto presence_it = presence_by_player_id.find(friend_link.friend_player_id);
        const auto unread_it = unread_by_channel_id.find(channel.channel_id);
        const auto nickname_it = nickname_by_player_id.find(friend_link.friend_player_id);
        const auto latest_it = latest_by_channel_id.find(channel.channel_id);
        const auto has_latest_message = latest_it != latest_by_channel_id.end();

        // 这里刻意把好友关系、在线状态、未读数、最近一条消息聚合成一份结果，
        // 让前端做会话列表时不需要额外再打 presence 和 history 请求。
        result.push_back(boost::json::object{
            {"player_id", friend_link.player_id},
            {"friend_player_id", friend_link.friend_player_id},
            {"friend_nickname",
             nickname_it != nickname_by_player_id.end() ? nickname_it->second : std::string{}},
            {"channel", mf::game::to_json(channel)},
            {"online", presence_it != presence_by_player_id.end() ? presence_it->second.online : false},
            {"connection_count",
             presence_it != presence_by_player_id.end() ? presence_it->second.connection_count : 0},
            {"unread_count", unread_it != unread_by_channel_id.end() ? unread_it->second : 0},
            {"last_message",
             has_latest_message ? boost::json::value{mf::game::to_json(latest_it->second)} : boost::json::value{}},
        });
    }
    return result;
}

std::optional<mf::game::ChatChannel> read_channel_from_object(const boost::json::object& object) {
    if (!object.if_contains("channel_kind")) {
        return std::nullopt;
    }

    const auto kind = mf::chat::parse_channel_kind(read_string_field(object, "channel_kind"));
    if (!kind) {
        return std::nullopt;
    }

    switch (*kind) {
    case mf::game::ChatChannelKind::lobby: {
        const auto scope = object.if_contains("scope_id") ? read_string_field(object, "scope_id") : std::string{"global"};
        return mf::chat::make_lobby_channel(scope);
    }
    case mf::game::ChatChannelKind::room:
        if (!object.if_contains("scope_id")) {
            return std::nullopt;
        }
        return mf::chat::make_room_channel(read_string_field(object, "scope_id"));
    case mf::game::ChatChannelKind::friend_direct:
        if (!object.if_contains("player_a") || !object.if_contains("player_b")) {
            return std::nullopt;
        }
        return mf::chat::make_friend_channel(
            read_string_field(object, "player_a"),
            read_string_field(object, "player_b"));
    }
    return std::nullopt;
}

std::optional<mf::game::AdminTaskKind> parse_admin_task_kind_local(std::string_view value) {
    if (value == "announce") {
        return mf::game::AdminTaskKind::announce;
    }
    if (value == "batch_reward") {
        return mf::game::AdminTaskKind::batch_reward;
    }
    if (value == "batch_mail") {
        return mf::game::AdminTaskKind::batch_mail;
    }
    if (value == "batch_mute") {
        return mf::game::AdminTaskKind::batch_mute;
    }
    if (value == "batch_account_status") {
        return mf::game::AdminTaskKind::batch_account_status;
    }
    return std::nullopt;
}

std::optional<std::vector<mf::game::RewardEntry>> read_rewards_array(
    const boost::json::object& object,
    std::string& error_message) {
    // 统一解析 HTTP 请求里的 rewards 数组，
    // 这样发奖接口和发邮件接口可以复用同一套字段校验逻辑。
    std::vector<mf::game::RewardEntry> rewards;
    const auto* rewards_value = object.if_contains("rewards");
    if (!rewards_value) {
        return rewards;
    }
    if (!rewards_value->is_array()) {
        error_message = "rewards 必须是数组";
        return std::nullopt;
    }

    for (const auto& value : rewards_value->as_array()) {
        if (!value.is_object()) {
            error_message = "奖励项必须是对象";
            return std::nullopt;
        }
        const auto& reward_object = value.as_object();
        if (!reward_object.if_contains("kind") || !reward_object.if_contains("amount")) {
            error_message = "奖励项必须包含 kind 和 amount";
            return std::nullopt;
        }

        const auto kind = parse_reward_kind(read_string_field(reward_object, "kind"));
        if (!kind) {
            error_message = "reward.kind 只能是 gold、currency、item 或 attachment_template";
            return std::nullopt;
        }
        const auto resource_id = reward_object.if_contains("resource_id")
                                     ? read_string_field(reward_object, "resource_id")
                                     : std::string{};
        if ((*kind == mf::game::RewardKind::currency || *kind == mf::game::RewardKind::item ||
             *kind == mf::game::RewardKind::attachment_template) &&
            resource_id.empty()) {
            error_message = "currency、item、attachment_template 奖励必须填写 resource_id";
            return std::nullopt;
        }

        rewards.push_back(mf::game::RewardEntry{
            .kind = *kind,
            .resource_id = resource_id,
            .amount = read_optional_i64_field(reward_object, "amount", 0),
        });
    }

    return rewards;
}

std::vector<std::string> read_string_array_field(const boost::json::object& object, std::string_view key) {
    std::vector<std::string> values;
    if (const auto* value = object.if_contains(key)) {
        ensure_value_type(*value, key, boost::json::kind::array);
        for (const auto& item : value->as_array()) {
            if (!item.is_string()) {
                throw_field_error(
                    "invalid_array_item_type",
                    "数组元素类型不正确",
                    key,
                    {
                        {"expected_item_type", "string"},
                        {"expected_item_type_label", "字符串"},
                        {"actual_item_type", json_type_name(item)},
                    });
            }
            values.push_back(boost::json::value_to<std::string>(item));
        }
    }
    return values;
}

bool require_bootstrap_token(
    const mf::http::Request& request,
    const std::string& bootstrap_token,
    mf::core::ApiError& error) {
    if (bootstrap_token.empty()) {
        error = mf::core::ApiError{
            .status = boost::beast::http::status::service_unavailable,
            .code = "bootstrap_not_configured",
            .message = "未配置 bootstrap 鉴权",
        };
        return false;
    }

    const auto provided_token = request["X-GM-Token"];
    if (std::string_view{provided_token} != bootstrap_token) {
        error = mf::core::ApiError{
            .status = boost::beast::http::status::forbidden,
            .code = "bootstrap_forbidden",
            .message = "bootstrap token 缺失或无效",
        };
        return false;
    }
    return true;
}

const mf::game::AdminPrincipal* require_admin_permission(
    const mf::http::Request& request,
    const mf::auth::TokenService& token_service,
    const std::shared_ptr<mf::game::AdminRepository>& admin_repository,
    const std::string& permission,
    mf::game::Session& session_storage,
    mf::game::AdminPrincipal& admin_storage,
    mf::core::ApiError& error) {
    const auto* session = require_session(request, token_service, session_storage, error);
    if (!session) {
        return nullptr;
    }

    const auto admin = admin_repository->find_by_account_id(session->account_id);
    if (!admin || !admin->active) {
        error = mf::core::ApiError{
            .status = boost::beast::http::status::forbidden,
            .code = "admin_required",
            .message = "当前账号不是有效的管理员账号",
        };
        return nullptr;
    }

    if (std::find(admin->permissions.begin(), admin->permissions.end(), permission) == admin->permissions.end()) {
        error = mf::core::ApiError{
            .status = boost::beast::http::status::forbidden,
            .code = "admin_permission_denied",
            .message = "当前管理员缺少所需权限",
        };
        return nullptr;
    }

    admin_storage = *admin;
    return &admin_storage;
}
