#pragma once

#include "mf/auth/token_service.hpp"
#include "mf/chat/chat_service.hpp"
#include "mf/chat/state.hpp"
#include "mf/core/api.hpp"
#include "mf/core/tracing.hpp"
#include "mf/game/json.hpp"
#include "mf/game/mail_service.hpp"
#include "mf/game/repositories.hpp"
#include "mf/http/query_params.hpp"

#include <boost/json/object.hpp>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

const mf::game::Session* require_session(
    const mf::http::Request& request,
    const mf::auth::TokenService& token_service,
    mf::game::Session& storage,
    mf::core::ApiError& error);

std::string read_string_field(const boost::json::object& object, std::string_view key);
std::string read_optional_string_field(const boost::json::object& object, std::string_view key);
bool read_optional_bool_field(const boost::json::object& object, std::string_view key, bool default_value);
std::int64_t read_optional_i64_field(
    const boost::json::object& object,
    std::string_view key,
    std::int64_t default_value);
std::string client_ip_from_request(const mf::http::Request& request);
std::string user_agent_from_request(const mf::http::Request& request);
std::string trace_id_from_request(const mf::http::Request& request);
boost::json::object parse_json_or_empty_object(const std::string& payload);
mf::game::AuditLogEntry append_admin_audit(
    const std::shared_ptr<mf::game::AuditLogRepository>& audit_repository,
    const mf::http::Request& request,
    std::string operator_id,
    std::string action,
    std::string target_type,
    std::string target_id,
    std::string detail_json,
    bool success = true,
    std::string failure_reason = {},
    std::string before_json = {},
    std::string after_json = {},
    std::chrono::steady_clock::time_point started_at = std::chrono::steady_clock::now());
std::vector<std::string> resolve_target_players(
    const boost::json::object& object,
    const std::shared_ptr<mf::game::PlayerRepository>& player_repository);
std::optional<mf::game::AccountStatus> parse_account_status(std::string_view value);
boost::json::array build_friend_presence_array(
    const std::vector<mf::game::FriendLink>& friends,
    const std::vector<mf::game::FriendPresence>& presence_items);
boost::json::array build_friend_overview_array(
    const std::string& self_player_id,
    const std::vector<mf::game::FriendLink>& friends,
    const std::shared_ptr<mf::game::PlayerRepository>& player_repository,
    const std::shared_ptr<mf::chat::ChatService>& chat_service,
    const std::shared_ptr<mf::chat::ChatStateService>& chat_state_service,
    std::size_t limit,
    std::size_t offset);
std::optional<mf::game::ChatChannel> read_channel_from_object(const boost::json::object& object);
std::optional<mf::game::AdminTaskKind> parse_admin_task_kind_local(std::string_view value);
std::optional<std::vector<mf::game::RewardEntry>> read_rewards_array(
    const boost::json::object& object,
    std::string& error_message);
std::vector<std::string> read_string_array_field(const boost::json::object& object, std::string_view key);
bool require_bootstrap_token(
    const mf::http::Request& request,
    const std::string& bootstrap_token,
    mf::core::ApiError& error);
const mf::game::AdminPrincipal* require_admin_permission(
    const mf::http::Request& request,
    const mf::auth::TokenService& token_service,
    const std::shared_ptr<mf::game::AdminRepository>& admin_repository,
    const std::string& permission,
    mf::game::Session& session_storage,
    mf::game::AdminPrincipal& admin_storage,
    mf::core::ApiError& error);
