#include "mf/storage/mysql_repositories.hpp"

// 文件职责：
// 实现基于 Boost.MySQL 的账号、玩家、好友、聊天仓储。
// 调用流程：
// mysql_redis 模式下，HTTP/WS 业务先调用对应 Repository 接口 ->
// Repository 再通过 Boost.MySQL 预处理语句访问 account / player_profile / friend_link / chat_message 表 ->
// 最终把持久化后的领域对象返回给上层服务。

#include "mf/chat/channels.hpp"
#include "mf/core/tracing.hpp"
#include "mf/core/profiling.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/json/array.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <boost/mysql/any_connection.hpp>
#include <boost/mysql/connect_params.hpp>
#include <boost/mysql/results.hpp>
#include <boost/mysql/row_view.hpp>
#include <boost/mysql/statement.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iomanip>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace mf::storage {
namespace mysql = boost::mysql;
namespace asio = boost::asio;

namespace {

struct MySqlObservabilityState {
    std::shared_ptr<mf::core::Logger> logger;
    std::shared_ptr<mf::core::MetricsRegistry> metrics;
    std::int64_t slow_operation_threshold_ms = 100;
};

std::mutex g_mysql_observability_mutex;
MySqlObservabilityState g_mysql_observability;

void observe_mysql_phase(std::string_view operation, std::string_view phase, double duration_ms) {
    std::scoped_lock lock{g_mysql_observability_mutex};
    if (g_mysql_observability.metrics) {
        g_mysql_observability.metrics->observe_histogram_ms(
            "mf_mysql_phase_duration_ms",
            duration_ms,
            {{"operation", std::string{operation}}, {"phase", std::string{phase}}});
    }
}

class MySqlClient;


class StatementCache {
public:
    static void clear_connection(mysql::any_connection& connection) {
        std::scoped_lock lock{cache_mutex()};
        connection_cache().erase(&connection);
    }

    static mysql::statement prepare(mysql::any_connection& connection, std::string_view sql) {
        std::scoped_lock lock{cache_mutex()};
        auto& store = connection_cache()[&connection];
        const auto key = std::string{sql};
        const auto it = store.statements.find(key);
        if (it != store.statements.end()) {
            return it->second;
        }
        auto [inserted, _] = store.statements.emplace(key, connection.prepare_statement(sql));
        return inserted->second;
    }

    const mysql::statement& account_quick_insert(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            account_quick_insert_stmt_,
            "INSERT INTO account("
            "account_id, login_name, display_name, email, phone_number, platform_channel, register_source, auth_mode) "
            "VALUES (?, ?, ?, '', '', 'quick', 'quick_login', 'quick')");
    }

    const mysql::statement& account_quick_update_display_name(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            account_quick_update_display_name_stmt_,
            "UPDATE account "
            "SET display_name = ?, updated_at = CURRENT_TIMESTAMP "
            "WHERE login_name = ? AND auth_mode = 'quick'");
    }

    const mysql::statement& account_password_insert(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            account_password_insert_stmt_,
            "INSERT IGNORE INTO account("
            "account_id, login_name, display_name, email, phone_number, platform_channel, register_source, "
            "auth_mode, password_hash, password_scheme, password_updated_at) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, 'password', ?, ?, CURRENT_TIMESTAMP)");
    }

    const mysql::statement& account_find_by_login(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            account_find_by_login_stmt_,
            "SELECT account_id, login_name, display_name, email, phone_number, platform_channel, register_source, "
            "auth_mode, status, status_reason, failed_login_attempts, "
            "UNIX_TIMESTAMP(locked_until), UNIX_TIMESTAMP(last_login_at), last_login_ip, "
            "UNIX_TIMESTAMP(created_at), UNIX_TIMESTAMP(updated_at), "
            "password_hash, password_scheme, reset_token, UNIX_TIMESTAMP(reset_token_expires_at) "
            "FROM account WHERE login_name = ? LIMIT 1");
    }

    const mysql::statement& account_find_password_by_login(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            account_find_password_by_login_stmt_,
            "SELECT account.account_id, account.login_name, account.display_name, account.email, "
            "account.phone_number, account.platform_channel, account.register_source, "
            "account.auth_mode, account.status, account.status_reason, account.failed_login_attempts, "
            "UNIX_TIMESTAMP(account.locked_until), UNIX_TIMESTAMP(account.last_login_at), account.last_login_ip, "
            "UNIX_TIMESTAMP(account.created_at), UNIX_TIMESTAMP(account.updated_at), "
            "account.password_hash, account.password_scheme, account.reset_token, "
            "UNIX_TIMESTAMP(account.reset_token_expires_at), "
            "player_profile.player_id "
            "FROM account LEFT JOIN player_profile ON player_profile.account_id = account.account_id "
            "WHERE account.login_name = ? AND account.auth_mode = 'password' LIMIT 1");
    }

    const mysql::statement& account_find_by_id(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            account_find_by_id_stmt_,
            "SELECT account_id, login_name, display_name, email, phone_number, platform_channel, register_source, "
            "auth_mode, status, status_reason, failed_login_attempts, "
            "UNIX_TIMESTAMP(locked_until), UNIX_TIMESTAMP(last_login_at), last_login_ip, "
            "UNIX_TIMESTAMP(created_at), UNIX_TIMESTAMP(updated_at), "
            "password_hash, password_scheme, reset_token, UNIX_TIMESTAMP(reset_token_expires_at) "
            "FROM account WHERE account_id = ?");
    }

    const mysql::statement& account_update_password(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            account_update_password_stmt_,
            "UPDATE account "
            "SET password_hash = ?, password_scheme = ?, password_updated_at = CURRENT_TIMESTAMP, "
            "failed_login_attempts = 0, locked_until = NULL, reset_token = NULL, reset_token_expires_at = NULL, "
            "updated_at = CURRENT_TIMESTAMP "
            "WHERE account_id = ?");
    }

    const mysql::statement& account_issue_reset_token(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            account_issue_reset_token_stmt_,
            "UPDATE account "
            "SET reset_token = ?, reset_token_expires_at = FROM_UNIXTIME(?), updated_at = CURRENT_TIMESTAMP "
            "WHERE login_name = ? AND auth_mode = 'password' AND (? = '' OR email = ?)");
    }

    const mysql::statement& account_find_by_reset_token(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            account_find_by_reset_token_stmt_,
            "SELECT account_id, login_name, display_name, email, phone_number, platform_channel, register_source, "
            "auth_mode, status, status_reason, failed_login_attempts, "
            "UNIX_TIMESTAMP(locked_until), UNIX_TIMESTAMP(last_login_at), last_login_ip, "
            "UNIX_TIMESTAMP(created_at), UNIX_TIMESTAMP(updated_at), "
            "password_hash, password_scheme, reset_token, UNIX_TIMESTAMP(reset_token_expires_at) "
            "FROM account WHERE reset_token = ? AND reset_token_expires_at IS NOT NULL "
            "AND reset_token_expires_at >= CURRENT_TIMESTAMP LIMIT 1");
    }

    const mysql::statement& account_set_status(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            account_set_status_stmt_,
            "UPDATE account SET status = ?, status_reason = ?, updated_at = CURRENT_TIMESTAMP WHERE account_id = ?");
    }

    const mysql::statement& account_record_login_success(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            account_record_login_success_stmt_,
            "UPDATE account "
            "SET failed_login_attempts = 0, locked_until = NULL, "
            "last_login_at = CURRENT_TIMESTAMP, last_login_ip = ?, updated_at = CURRENT_TIMESTAMP "
            "WHERE account_id = ?");
    }

    const mysql::statement& account_record_login_failure(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            account_record_login_failure_stmt_,
            "UPDATE account "
            "SET failed_login_attempts = failed_login_attempts + 1, "
            "locked_until = CASE "
            "WHEN failed_login_attempts + 1 >= ? THEN FROM_UNIXTIME(?) "
            "ELSE locked_until END, "
            "updated_at = CURRENT_TIMESTAMP "
            "WHERE account_id = ?");
    }

    const mysql::statement& player_upsert(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            player_upsert_stmt_,
            "INSERT INTO player_profile(player_id, account_id, nickname, gold, gem) "
            "VALUES (?, ?, ?, ?, ?) "
            "ON DUPLICATE KEY UPDATE nickname = VALUES(nickname)");
    }

    const mysql::statement& player_find_by_id(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            player_find_by_id_stmt_,
            "SELECT player_id, account_id, nickname, gold, gem "
            "FROM player_profile WHERE player_id = ?");
    }

    const mysql::statement& player_find_by_account_id(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            player_find_by_account_id_stmt_,
            "SELECT player_id, account_id, nickname, gold, gem "
            "FROM player_profile WHERE account_id = ? LIMIT 1");
    }

    const mysql::statement& player_list_all(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            player_list_all_stmt_,
            "SELECT player_id, account_id, nickname, gold, gem "
            "FROM player_profile ORDER BY created_at ASC");
    }

    const mysql::statement& player_list_filtered(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            player_list_filtered_stmt_,
            "SELECT player_id, account_id, nickname, gold, gem "
            "FROM player_profile "
            "WHERE (? = '' OR player_id LIKE CONCAT('%', ?, '%')) "
            "AND (? = '' OR account_id LIKE CONCAT('%', ?, '%')) "
            "AND (? = '' OR nickname LIKE CONCAT('%', ?, '%')) "
            "ORDER BY created_at ASC LIMIT ? OFFSET ?");
    }

    const mysql::statement& player_currency_spend(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            player_currency_spend_stmt_,
            "UPDATE player_currency SET amount = amount - ? "
            "WHERE player_id = ? AND currency_id = ? AND amount >= ?");
    }

    const mysql::statement& player_inventory_upsert(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            player_inventory_upsert_stmt_,
            "INSERT INTO player_inventory(player_id, item_id, instance_id, quantity) VALUES (?, ?, ?, ?) "
            "ON DUPLICATE KEY UPDATE quantity = VALUES(quantity)");
    }

    const mysql::statement& player_inventory_insert_ignore(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            player_inventory_insert_ignore_stmt_,
            "INSERT IGNORE INTO player_inventory(player_id, item_id, instance_id, quantity) VALUES (?, ?, ?, ?)");
    }

    const mysql::statement& player_inventory_delete(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            player_inventory_delete_stmt_,
            "DELETE FROM player_inventory WHERE player_id = ? AND item_id = ?");
    }

    const mysql::statement& player_spend_gold(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            player_spend_gold_stmt_,
            "UPDATE player_profile SET gold = gold - ?, updated_at = CURRENT_TIMESTAMP "
            "WHERE player_id = ? AND gold >= ?");
    }

    const mysql::statement& player_spend_gem(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            player_spend_gem_stmt_,
            "UPDATE player_profile SET gem = gem - ?, updated_at = CURRENT_TIMESTAMP "
            "WHERE player_id = ? AND gem >= ?");
    }

    const mysql::statement& player_count_filtered(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            player_count_filtered_stmt_,
            "SELECT COUNT(*) "
            "FROM player_profile "
            "WHERE (? = '' OR player_id LIKE CONCAT('%', ?, '%')) "
            "AND (? = '' OR account_id LIKE CONCAT('%', ?, '%')) "
            "AND (? = '' OR nickname LIKE CONCAT('%', ?, '%'))");
    }

    const mysql::statement& player_admin_list_filtered(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            player_admin_list_filtered_stmt_,
            "SELECT p.player_id, p.account_id, p.nickname, p.gold, p.gem, "
            "a.status, UNIX_TIMESTAMP(a.last_login_at), a.last_login_ip, "
            "COALESCE(m.channel_id, ''), UNIX_TIMESTAMP(m.muted_until), UNIX_TIMESTAMP(p.created_at) "
            "FROM player_profile p "
            "LEFT JOIN account a ON a.account_id = p.account_id "
            "LEFT JOIN chat_mute m ON m.channel_id = 'lobby:global' "
            "AND m.muted_player_id = p.player_id "
            "AND (m.muted_until IS NULL OR m.muted_until >= CURRENT_TIMESTAMP) "
            "WHERE (? = '' OR p.player_id LIKE CONCAT('%', ?, '%')) "
            "AND (? = '' OR p.account_id LIKE CONCAT('%', ?, '%')) "
            "AND (? = '' OR p.nickname LIKE CONCAT('%', ?, '%')) "
            "ORDER BY p.created_at ASC, p.player_id ASC LIMIT ?");
    }

    const mysql::statement& player_admin_list_filtered_cursor(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            player_admin_list_filtered_cursor_stmt_,
            "SELECT p.player_id, p.account_id, p.nickname, p.gold, p.gem, "
            "a.status, UNIX_TIMESTAMP(a.last_login_at), a.last_login_ip, "
            "COALESCE(m.channel_id, ''), UNIX_TIMESTAMP(m.muted_until), UNIX_TIMESTAMP(p.created_at) "
            "FROM player_profile p "
            "LEFT JOIN account a ON a.account_id = p.account_id "
            "LEFT JOIN chat_mute m ON m.channel_id = 'lobby:global' "
            "AND m.muted_player_id = p.player_id "
            "AND (m.muted_until IS NULL OR m.muted_until >= CURRENT_TIMESTAMP) "
            "WHERE (? = '' OR p.player_id LIKE CONCAT('%', ?, '%')) "
            "AND (? = '' OR p.account_id LIKE CONCAT('%', ?, '%')) "
            "AND (? = '' OR p.nickname LIKE CONCAT('%', ?, '%')) "
            "AND (p.created_at > FROM_UNIXTIME(?) OR (p.created_at = FROM_UNIXTIME(?) AND p.player_id > ?)) "
            "ORDER BY p.created_at ASC, p.player_id ASC LIMIT ?");
    }

    const mysql::statement& item_upsert(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            item_upsert_stmt_,
            "INSERT INTO item_definition(item_id, name, description, icon, rarity, max_stack, item_type, enabled) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?) "
            "ON DUPLICATE KEY UPDATE "
            "name = VALUES(name), description = VALUES(description), icon = VALUES(icon), rarity = VALUES(rarity), "
            "max_stack = VALUES(max_stack), item_type = VALUES(item_type), enabled = VALUES(enabled), "
            "updated_at = CURRENT_TIMESTAMP");
    }

    const mysql::statement& item_find_by_id(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            item_find_by_id_stmt_,
            "SELECT item_id, name, description, icon, rarity, max_stack, item_type, enabled, "
            "UNIX_TIMESTAMP(created_at), UNIX_TIMESTAMP(updated_at) "
            "FROM item_definition WHERE item_id = ? LIMIT 1");
    }

    const mysql::statement& item_list_filtered(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            item_list_filtered_stmt_,
            "SELECT item_id, name, description, icon, rarity, max_stack, item_type, enabled, "
            "UNIX_TIMESTAMP(created_at), UNIX_TIMESTAMP(updated_at) "
            "FROM item_definition "
            "WHERE (? = '' OR item_id LIKE CONCAT('%', ?, '%')) "
            "AND (? = '' OR name LIKE CONCAT('%', ?, '%')) "
            "AND (? = '' OR rarity = ?) "
            "AND (? = -1 OR enabled = ?) "
            "ORDER BY item_id ASC LIMIT ? OFFSET ?");
    }

    const mysql::statement& item_count_filtered(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            item_count_filtered_stmt_,
            "SELECT COUNT(*) FROM item_definition "
            "WHERE (? = '' OR item_id LIKE CONCAT('%', ?, '%')) "
            "AND (? = '' OR name LIKE CONCAT('%', ?, '%')) "
            "AND (? = '' OR rarity = ?) "
            "AND (? = -1 OR enabled = ?)");
    }

    const mysql::statement& friend_exists(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            friend_exists_stmt_,
            "SELECT player_id FROM friend_link "
            "WHERE ((player_id = ? AND friend_player_id = ?) OR (player_id = ? AND friend_player_id = ?)) "
            "AND status = 'accepted' LIMIT 1");
    }

    const mysql::statement& friend_request_exists(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            friend_request_exists_stmt_,
            "SELECT player_id FROM friend_link "
            "WHERE ((player_id = ? AND friend_player_id = ?) OR (player_id = ? AND friend_player_id = ?)) "
            "AND status IN ('accepted', 'pending_outgoing', 'pending_incoming') LIMIT 1");
    }

    const mysql::statement& friend_insert(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            friend_insert_stmt_,
            "INSERT INTO friend_link(player_id, friend_player_id, status) VALUES (?, ?, 'accepted')");
    }

    const mysql::statement& friend_request_insert(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            friend_request_insert_stmt_,
            "INSERT INTO friend_link(player_id, friend_player_id, status) VALUES (?, ?, ?)");
    }

    const mysql::statement& friend_list(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            friend_list_stmt_,
            "SELECT player_id, friend_player_id FROM friend_link "
            "WHERE player_id = ? AND status = 'accepted' ORDER BY created_at ASC");
    }

    const mysql::statement& friend_list_by_last_message(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            friend_list_by_last_message_stmt_,
            "SELECT f.player_id, f.friend_player_id, f.last_message_epoch AS last_sent_at, COALESCE(p.nickname, '') "
            "FROM friend_link f LEFT JOIN player_profile p ON p.player_id = f.friend_player_id "
            "WHERE f.player_id = ? AND f.status = 'accepted' "
            "ORDER BY f.last_message_epoch DESC, f.friend_player_id ASC "
            "LIMIT ? OFFSET ?");
    }

    const mysql::statement& friend_list_by_last_message_cursor(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            friend_list_by_last_message_cursor_stmt_,
            "SELECT f.player_id, f.friend_player_id, f.last_message_epoch AS last_sent_at, COALESCE(p.nickname, '') "
            "FROM friend_link f LEFT JOIN player_profile p ON p.player_id = f.friend_player_id "
            "WHERE f.player_id = ? AND f.status = 'accepted' "
            "  AND (f.last_message_epoch < ? "
            "    OR (f.last_message_epoch = ? AND f.friend_player_id > ?)) "
            "ORDER BY f.last_message_epoch DESC, f.friend_player_id ASC "
            "LIMIT ?");
    }

    const mysql::statement& friend_request_list_incoming(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            friend_request_list_incoming_stmt_,
            "SELECT friend_player_id, player_id, status, UNIX_TIMESTAMP(created_at) "
            "FROM friend_link WHERE player_id = ? AND status = 'pending_incoming' ORDER BY created_at ASC");
    }

    const mysql::statement& friend_request_list_outgoing(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            friend_request_list_outgoing_stmt_,
            "SELECT player_id, friend_player_id, status, UNIX_TIMESTAMP(created_at) "
            "FROM friend_link WHERE player_id = ? AND status = 'pending_outgoing' ORDER BY created_at ASC");
    }

    const mysql::statement& friend_request_update_status(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            friend_request_update_status_stmt_,
            "UPDATE friend_link SET status = ? "
            "WHERE (player_id = ? AND friend_player_id = ?) OR (player_id = ? AND friend_player_id = ?)");
    }

    const mysql::statement& friend_request_delete_pair(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            friend_request_delete_pair_stmt_,
            "DELETE FROM friend_link "
            "WHERE (player_id = ? AND friend_player_id = ?) OR (player_id = ? AND friend_player_id = ?)");
    }

    const mysql::statement& friend_last_message_touch(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            friend_last_message_touch_stmt_,
            "UPDATE friend_link SET last_message_epoch = GREATEST(COALESCE(last_message_epoch, 0), ?), "
            "last_message_at = GREATEST(COALESCE(last_message_at, FROM_UNIXTIME(0)), FROM_UNIXTIME(?)) "
            "WHERE status = 'accepted' AND ("
            "(player_id = ? AND friend_player_id = ?) OR "
            "(player_id = ? AND friend_player_id = ?))");
    }

    const mysql::statement& chat_block_upsert(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            chat_block_upsert_stmt_,
            "INSERT INTO chat_block(player_id, blocked_player_id) VALUES (?, ?) "
            "ON DUPLICATE KEY UPDATE created_at = CURRENT_TIMESTAMP");
    }

    const mysql::statement& chat_block_delete(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            chat_block_delete_stmt_,
            "DELETE FROM chat_block WHERE player_id = ? AND blocked_player_id = ?");
    }

    const mysql::statement& chat_block_exists(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            chat_block_exists_stmt_,
            "SELECT player_id FROM chat_block WHERE player_id = ? AND blocked_player_id = ? LIMIT 1");
    }

    const mysql::statement& chat_block_list(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            chat_block_list_stmt_,
            "SELECT player_id, blocked_player_id, UNIX_TIMESTAMP(created_at) "
            "FROM chat_block WHERE player_id = ? ORDER BY created_at ASC");
    }

    const mysql::statement& chat_insert(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            chat_insert_stmt_,
            "INSERT INTO chat_message("
            "message_id, channel_id, channel_kind, scope_id, message_kind, "
            "sender_player_id, sender_nickname, text_body, sent_at) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, FROM_UNIXTIME(?)) "
            "ON DUPLICATE KEY UPDATE message_id = VALUES(message_id)");
    }

    const mysql::statement& chat_outbox_insert(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            chat_outbox_insert_stmt_,
            "INSERT INTO chat_message_outbox("
            "message_id, channel_id, channel_kind, scope_id, message_kind, "
            "sender_player_id, sender_nickname, text_body, sent_at) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, FROM_UNIXTIME(?)) "
            "ON DUPLICATE KEY UPDATE message_id = VALUES(message_id)");
    }

    const mysql::statement& chat_outbox_list(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            chat_outbox_list_stmt_,
            "SELECT message_id, channel_id, channel_kind, scope_id, message_kind, "
            "sender_player_id, sender_nickname, text_body, 0, NULL, NULL, "
            "UNIX_TIMESTAMP(sent_at) FROM chat_message_outbox "
            "WHERE status = 'pending' AND next_attempt_at <= CURRENT_TIMESTAMP "
            "ORDER BY outbox_id ASC LIMIT ? FOR UPDATE SKIP LOCKED");
    }

    const mysql::statement& chat_outbox_delete(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            chat_outbox_delete_stmt_,
            "DELETE FROM chat_message_outbox WHERE message_id = ?");
    }

    const mysql::statement& chat_outbox_health(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            chat_outbox_health_stmt_,
            "SELECT "
            "CAST(COALESCE(SUM(status = 'pending'), 0) AS SIGNED), "
            "CAST(COALESCE(SUM(status = 'failed'), 0) AS SIGNED), "
            "UNIX_TIMESTAMP(MIN(CASE WHEN status = 'pending' THEN created_at END)) "
            "FROM chat_message_outbox");
    }

    const mysql::statement& chat_outbox_fail(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            chat_outbox_fail_stmt_,
            "UPDATE chat_message_outbox SET attempts = attempts + 1, "
            "next_attempt_at = DATE_ADD(CURRENT_TIMESTAMP, INTERVAL LEAST(300, POW(2, attempts)) SECOND), "
            "status = IF(attempts + 1 >= 10, 'failed', 'pending'), "
            "last_error = ? WHERE message_id = ?");
    }

    const mysql::statement& chat_outbox_retry_failed(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            chat_outbox_retry_failed_stmt_,
            "UPDATE chat_message_outbox SET status = 'pending', attempts = 0, "
            "next_attempt_at = CURRENT_TIMESTAMP, last_error = NULL "
            "WHERE status = 'failed' ORDER BY outbox_id ASC LIMIT ?");
    }

    const mysql::statement& chat_recent(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            chat_recent_stmt_,
            "SELECT message_id, channel_id, channel_kind, scope_id, message_kind, "
            "sender_player_id, sender_nickname, text_body, recalled, recalled_by_player_id, "
            "UNIX_TIMESTAMP(recalled_at), UNIX_TIMESTAMP(sent_at) "
            "FROM chat_message WHERE channel_id = ? ORDER BY sent_at DESC LIMIT ?");
    }

    const mysql::statement& chat_history_page_first(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            chat_history_page_first_stmt_,
            "SELECT message_id, channel_id, channel_kind, scope_id, message_kind, "
            "sender_player_id, sender_nickname, text_body, recalled, recalled_by_player_id, "
            "UNIX_TIMESTAMP(recalled_at), UNIX_TIMESTAMP(sent_at) "
            "FROM chat_message WHERE channel_id = ? "
            "ORDER BY sent_at DESC, message_id DESC LIMIT ?");
    }

    const mysql::statement& chat_history_page_next(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            chat_history_page_next_stmt_,
            "SELECT message_id, channel_id, channel_kind, scope_id, message_kind, "
            "sender_player_id, sender_nickname, text_body, recalled, recalled_by_player_id, "
            "UNIX_TIMESTAMP(recalled_at), UNIX_TIMESTAMP(sent_at) "
            "FROM chat_message WHERE channel_id = ? "
            "  AND (sent_at < FROM_UNIXTIME(?) "
            "    OR (sent_at = FROM_UNIXTIME(?) AND message_id < ?)) "
            "ORDER BY sent_at DESC, message_id DESC LIMIT ?");
    }

    const mysql::statement& chat_recall(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            chat_recall_stmt_,
            "UPDATE chat_message SET recalled = 1, recalled_by_player_id = ?, "
            "recalled_at = CURRENT_TIMESTAMP, text_body = '[消息已撤回]' "
            "WHERE channel_id = ? AND message_id = ? AND sender_player_id = ? AND recalled = 0");
    }

    const mysql::statement& chat_find_by_id(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            chat_find_by_id_stmt_,
            "SELECT message_id, channel_id, channel_kind, scope_id, message_kind, "
            "sender_player_id, sender_nickname, text_body, recalled, recalled_by_player_id, "
            "UNIX_TIMESTAMP(recalled_at), UNIX_TIMESTAMP(sent_at) "
            "FROM chat_message WHERE channel_id = ? AND message_id = ? LIMIT 1");
    }

    const mysql::statement& chat_mute_upsert(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            chat_mute_upsert_stmt_,
            "INSERT INTO chat_mute(channel_id, muted_player_id, muted_by_player_id, reason, muted_until) "
            "VALUES (?, ?, ?, ?, FROM_UNIXTIME(?)) "
            "ON DUPLICATE KEY UPDATE muted_by_player_id = VALUES(muted_by_player_id), "
            "reason = VALUES(reason), muted_until = VALUES(muted_until), updated_at = CURRENT_TIMESTAMP");
    }

    const mysql::statement& chat_mute_clear(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            chat_mute_clear_stmt_,
            "DELETE FROM chat_mute WHERE channel_id = ? AND muted_player_id = ?");
    }

    const mysql::statement& chat_mute_find_active(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            chat_mute_find_active_stmt_,
            "SELECT channel_id, muted_player_id, muted_by_player_id, reason, UNIX_TIMESTAMP(muted_until) "
            "FROM chat_mute WHERE channel_id = ? AND muted_player_id = ? "
            "AND (muted_until IS NULL OR muted_until >= CURRENT_TIMESTAMP) LIMIT 1");
    }

    const mysql::statement& reward_grant_find_by_id(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            reward_grant_find_by_id_stmt_,
            "SELECT grant_id, player_id, source_type, source_id, rewards_json, UNIX_TIMESTAMP(granted_at) "
            "FROM reward_grant WHERE grant_id = ? LIMIT 1");
    }

    const mysql::statement& reward_grant_insert(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            reward_grant_insert_stmt_,
            "INSERT INTO reward_grant(grant_id, player_id, source_type, source_id, rewards_json, granted_at) "
            "VALUES (?, ?, ?, ?, CAST(? AS JSON), FROM_UNIXTIME(?))");
    }

    const mysql::statement& reward_grant_list_by_player(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            reward_grant_list_by_player_stmt_,
            "SELECT grant_id, player_id, source_type, source_id, rewards_json, UNIX_TIMESTAMP(granted_at) "
            "FROM reward_grant WHERE player_id = ? ORDER BY granted_at DESC LIMIT ?");
    }

    const mysql::statement& reward_grant_list_filtered(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            reward_grant_list_filtered_stmt_,
            "SELECT grant_id, player_id, source_type, source_id, rewards_json, UNIX_TIMESTAMP(granted_at) "
            "FROM reward_grant "
            "WHERE player_id = ? "
            "AND (? = '' OR source_type = ?) "
            "AND (? = '' OR source_id LIKE CONCAT('%', ?, '%')) "
            "ORDER BY granted_at DESC LIMIT ? OFFSET ?");
    }

    const mysql::statement& reward_grant_count_filtered(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            reward_grant_count_filtered_stmt_,
            "SELECT COUNT(*) "
            "FROM reward_grant "
            "WHERE player_id = ? "
            "AND (? = '' OR source_type = ?) "
            "AND (? = '' OR source_id LIKE CONCAT('%', ?, '%'))");
    }

    const mysql::statement& reward_template_upsert(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            reward_template_upsert_stmt_,
            "INSERT INTO reward_template(template_id, template_name, rewards_json) "
            "VALUES (?, ?, CAST(? AS JSON)) "
            "ON DUPLICATE KEY UPDATE template_name = VALUES(template_name), rewards_json = VALUES(rewards_json), "
            "updated_at = CURRENT_TIMESTAMP");
    }

    const mysql::statement& reward_template_find_by_id(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            reward_template_find_by_id_stmt_,
            "SELECT template_id, template_name, rewards_json, UNIX_TIMESTAMP(created_at), UNIX_TIMESTAMP(updated_at) "
            "FROM reward_template WHERE template_id = ? LIMIT 1");
    }

    const mysql::statement& reward_template_list_all(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            reward_template_list_all_stmt_,
            "SELECT template_id, template_name, rewards_json, UNIX_TIMESTAMP(created_at), UNIX_TIMESTAMP(updated_at) "
            "FROM reward_template ORDER BY template_id ASC");
    }

    const mysql::statement& announcement_insert(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            announcement_insert_stmt_,
            "INSERT INTO announcement("
            "announcement_id, channel_id, channel_kind, scope_id, display_name, operator_id, title, body_text, "
            "published_at, expires_at) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, FROM_UNIXTIME(?), FROM_UNIXTIME(?))");
    }

    const mysql::statement& announcement_list_recent(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            announcement_list_recent_stmt_,
            "SELECT announcement_id, channel_id, channel_kind, scope_id, display_name, operator_id, title, body_text, "
            "UNIX_TIMESTAMP(published_at), UNIX_TIMESTAMP(expires_at) "
            "FROM announcement "
            "WHERE (? = '' OR channel_kind = ?) "
            "AND (? = '' OR scope_id = ?) "
            "AND (expires_at IS NULL OR expires_at >= FROM_UNIXTIME(?)) "
            "ORDER BY published_at DESC LIMIT ?");
    }

    const mysql::statement& mail_insert(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            mail_insert_stmt_,
            "INSERT INTO mail_message("
            "mail_id, player_id, sender_name, title, body_text, rewards_json, "
            "is_read, is_claimed, is_deleted, claim_grant_id, sent_at, expires_at, read_at, claimed_at, deleted_at) "
            "VALUES (?, ?, ?, ?, ?, CAST(? AS JSON), ?, ?, ?, ?, FROM_UNIXTIME(?), FROM_UNIXTIME(?), "
            "FROM_UNIXTIME(?), FROM_UNIXTIME(?), FROM_UNIXTIME(?))");
    }

    const mysql::statement& mail_list_inbox(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            mail_list_inbox_stmt_,
            "SELECT mail_id, player_id, sender_name, title, body_text, rewards_json, "
            "is_read, is_claimed, is_deleted, claim_grant_id, "
            "UNIX_TIMESTAMP(sent_at), UNIX_TIMESTAMP(expires_at), UNIX_TIMESTAMP(read_at), "
            "UNIX_TIMESTAMP(claimed_at), UNIX_TIMESTAMP(deleted_at) "
            "FROM mail_message WHERE player_id = ? AND mail_id <> '' AND (? = 1 OR is_deleted = 0) "
            "ORDER BY sent_at DESC");
    }

    const mysql::statement& mail_list_inbox_cursor(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            mail_list_inbox_cursor_stmt_,
            "SELECT mail_id, player_id, sender_name, title, body_text, rewards_json, "
            "is_read, is_claimed, is_deleted, claim_grant_id, "
            "UNIX_TIMESTAMP(sent_at), UNIX_TIMESTAMP(expires_at), UNIX_TIMESTAMP(read_at), "
            "UNIX_TIMESTAMP(claimed_at), UNIX_TIMESTAMP(deleted_at) "
            "FROM mail_message WHERE player_id = ? AND mail_id <> '' "
            "AND (? = 1 OR is_deleted = 0) "
            "AND (sent_at < FROM_UNIXTIME(?) "
            "OR (sent_at = FROM_UNIXTIME(?) AND mail_id < ?)) "
            "ORDER BY sent_at DESC, mail_id DESC LIMIT ?");
    }

    const mysql::statement& mail_list_filtered(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            mail_list_filtered_stmt_,
            "SELECT mail_id, player_id, sender_name, title, body_text, rewards_json, "
            "is_read, is_claimed, is_deleted, claim_grant_id, "
            "UNIX_TIMESTAMP(sent_at), UNIX_TIMESTAMP(expires_at), UNIX_TIMESTAMP(read_at), "
            "UNIX_TIMESTAMP(claimed_at), UNIX_TIMESTAMP(deleted_at) "
            "FROM mail_message "
            "WHERE player_id = ? "
            "AND mail_id <> '' "
            "AND (? = 1 OR is_deleted = 0) "
            "AND (? = 0 OR is_claimed = ?) "
            "AND (? = 0 OR is_read = ?) "
            "ORDER BY sent_at DESC LIMIT ? OFFSET ?");
    }

    const mysql::statement& mail_count_filtered(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            mail_count_filtered_stmt_,
            "SELECT COUNT(*) "
            "FROM mail_message "
            "WHERE player_id = ? "
            "AND mail_id <> '' "
            "AND (? = 1 OR is_deleted = 0) "
            "AND (? = 0 OR is_claimed = ?) "
            "AND (? = 0 OR is_read = ?)");
    }

    const mysql::statement& mail_find_by_id(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            mail_find_by_id_stmt_,
            "SELECT mail_id, player_id, sender_name, title, body_text, rewards_json, "
            "is_read, is_claimed, is_deleted, claim_grant_id, "
            "UNIX_TIMESTAMP(sent_at), UNIX_TIMESTAMP(expires_at), UNIX_TIMESTAMP(read_at), "
            "UNIX_TIMESTAMP(claimed_at), UNIX_TIMESTAMP(deleted_at) "
            "FROM mail_message WHERE player_id = ? AND mail_id = ? LIMIT 1");
    }

    const mysql::statement& mail_mark_read(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            mail_mark_read_stmt_,
            "UPDATE mail_message SET is_read = 1, read_at = FROM_UNIXTIME(?), updated_at = CURRENT_TIMESTAMP "
            "WHERE player_id = ? AND mail_id = ? AND is_deleted = 0");
    }

    const mysql::statement& mail_mark_claimed(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            mail_mark_claimed_stmt_,
            "UPDATE mail_message SET is_read = 1, is_claimed = 1, claim_grant_id = ?, "
            "read_at = COALESCE(read_at, FROM_UNIXTIME(?)), claimed_at = FROM_UNIXTIME(?), "
            "updated_at = CURRENT_TIMESTAMP "
            "WHERE player_id = ? AND mail_id = ? AND is_deleted = 0");
    }

    const mysql::statement& mail_mark_deleted(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            mail_mark_deleted_stmt_,
            "UPDATE mail_message SET is_deleted = 1, deleted_at = FROM_UNIXTIME(?), updated_at = CURRENT_TIMESTAMP "
            "WHERE player_id = ? AND mail_id = ?");
    }

    const mysql::statement& gm_audit_insert(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            gm_audit_insert_stmt_,
            "INSERT INTO gm_audit_log("
            "audit_id, operator_id, action_name, target_type, target_id, request_ip, user_agent, trace_id, "
            "success_flag, failure_reason, duration_ms, before_json, after_json, detail_json, created_at) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, CAST(? AS JSON), CAST(? AS JSON), CAST(? AS JSON), "
            "FROM_UNIXTIME(?))");
    }

    const mysql::statement& gm_audit_list_recent(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            gm_audit_list_recent_stmt_,
            "SELECT audit_id, operator_id, action_name, target_type, target_id, request_ip, user_agent, trace_id, "
            "success_flag, failure_reason, duration_ms, before_json, after_json, detail_json, UNIX_TIMESTAMP(created_at) "
            "FROM gm_audit_log ORDER BY created_at DESC LIMIT ?");
    }

    const mysql::statement& gm_audit_list_filtered(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            gm_audit_list_filtered_stmt_,
            "SELECT audit_id, operator_id, action_name, target_type, target_id, request_ip, user_agent, trace_id, "
            "success_flag, failure_reason, duration_ms, before_json, after_json, detail_json, UNIX_TIMESTAMP(created_at) "
            "FROM gm_audit_log "
            "WHERE (? = '' OR action_name LIKE CONCAT('%', ?, '%')) "
            "AND (? = '' OR operator_id LIKE CONCAT('%', ?, '%')) "
            "AND (? = 0 OR success_flag = ?) "
            "ORDER BY created_at DESC, audit_id DESC LIMIT ?");
    }

    const mysql::statement& gm_audit_list_filtered_cursor(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            gm_audit_list_filtered_cursor_stmt_,
            "SELECT audit_id, operator_id, action_name, target_type, target_id, request_ip, user_agent, trace_id, "
            "success_flag, failure_reason, duration_ms, before_json, after_json, detail_json, UNIX_TIMESTAMP(created_at) "
            "FROM gm_audit_log "
            "WHERE (? = '' OR action_name LIKE CONCAT('%', ?, '%')) "
            "AND (? = '' OR operator_id LIKE CONCAT('%', ?, '%')) "
            "AND (? = 0 OR success_flag = ?) "
            "AND (created_at < FROM_UNIXTIME(?) OR (created_at = FROM_UNIXTIME(?) AND audit_id < ?)) "
            "ORDER BY created_at DESC, audit_id DESC LIMIT ?");
    }

    const mysql::statement& gm_audit_count_filtered(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            gm_audit_count_filtered_stmt_,
            "SELECT COUNT(*) "
            "FROM gm_audit_log "
            "WHERE (? = '' OR action_name LIKE CONCAT('%', ?, '%')) "
            "AND (? = '' OR operator_id LIKE CONCAT('%', ?, '%')) "
            "AND (? = 0 OR success_flag = ?)");
    }

    const mysql::statement& admin_upsert(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            admin_upsert_stmt_,
            "INSERT INTO admin_account(account_id, operator_name, is_active) VALUES (?, ?, ?) "
            "ON DUPLICATE KEY UPDATE operator_name = VALUES(operator_name), is_active = VALUES(is_active), "
            "updated_at = CURRENT_TIMESTAMP");
    }

    const mysql::statement& admin_delete_permissions(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            admin_delete_permissions_stmt_,
            "DELETE FROM admin_permission WHERE account_id = ?");
    }

    const mysql::statement& admin_insert_permission(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            admin_insert_permission_stmt_,
            "INSERT INTO admin_permission(account_id, permission_code) VALUES (?, ?)");
    }

    const mysql::statement& admin_find_by_account_id(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            admin_find_by_account_id_stmt_,
            "SELECT account_id, operator_name, is_active, UNIX_TIMESTAMP(created_at), UNIX_TIMESTAMP(updated_at) "
            "FROM admin_account WHERE account_id = ? LIMIT 1");
    }

    const mysql::statement& admin_list_permissions(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            admin_list_permissions_stmt_,
            "SELECT permission_code FROM admin_permission WHERE account_id = ? ORDER BY permission_code ASC");
    }

    const mysql::statement& admin_has_permission(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            admin_has_permission_stmt_,
            "SELECT permission_code FROM admin_permission WHERE account_id = ? AND permission_code = ? LIMIT 1");
    }

    const mysql::statement& admin_list_all(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            admin_list_all_stmt_,
            "SELECT account_id, operator_name, is_active, UNIX_TIMESTAMP(created_at), UNIX_TIMESTAMP(updated_at) "
            "FROM admin_account ORDER BY account_id ASC");
    }

    const mysql::statement& admin_list_filtered(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            admin_list_filtered_stmt_,
            "SELECT account_id, operator_name, is_active, UNIX_TIMESTAMP(created_at), UNIX_TIMESTAMP(updated_at) "
            "FROM admin_account "
            "WHERE (? = '' OR operator_name LIKE CONCAT('%', ?, '%')) "
            "AND (? = 0 OR is_active = ?) "
            "ORDER BY account_id ASC LIMIT ? OFFSET ?");
    }

    const mysql::statement& admin_count_filtered(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            admin_count_filtered_stmt_,
            "SELECT COUNT(*) "
            "FROM admin_account "
            "WHERE (? = '' OR operator_name LIKE CONCAT('%', ?, '%')) "
            "AND (? = 0 OR is_active = ?)");
    }

    const mysql::statement& admin_task_upsert(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            admin_task_upsert_stmt_,
            "INSERT INTO admin_task("
            "task_id, task_kind, status, operator_id, target_type, target_id, request_ip, user_agent, trace_id, "
            "request_json, result_json, failure_reason, execute_at, executed_at) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, CAST(? AS JSON), CAST(? AS JSON), ?, FROM_UNIXTIME(?), FROM_UNIXTIME(?)) "
            "ON DUPLICATE KEY UPDATE status = VALUES(status), result_json = VALUES(result_json), "
            "failure_reason = VALUES(failure_reason), execute_at = VALUES(execute_at), executed_at = VALUES(executed_at), "
            "updated_at = CURRENT_TIMESTAMP");
    }

    const mysql::statement& admin_task_list_recent(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            admin_task_list_recent_stmt_,
            "SELECT task_id, task_kind, status, operator_id, target_type, target_id, request_ip, user_agent, trace_id, "
            "request_json, result_json, failure_reason, UNIX_TIMESTAMP(execute_at), UNIX_TIMESTAMP(executed_at), "
            "UNIX_TIMESTAMP(created_at), UNIX_TIMESTAMP(updated_at) "
            "FROM admin_task ORDER BY created_at DESC LIMIT ? OFFSET ?");
    }

    const mysql::statement& admin_task_list_filtered(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            admin_task_list_filtered_stmt_,
            "SELECT task_id, task_kind, status, operator_id, target_type, target_id, request_ip, user_agent, trace_id, "
            "request_json, result_json, failure_reason, UNIX_TIMESTAMP(execute_at), UNIX_TIMESTAMP(executed_at), "
            "UNIX_TIMESTAMP(created_at), UNIX_TIMESTAMP(updated_at) "
            "FROM admin_task "
            "WHERE (? = '' OR task_kind = ?) "
            "AND (? = '' OR status = ?) "
            "ORDER BY created_at DESC, task_id DESC LIMIT ?");
    }

    const mysql::statement& admin_task_list_filtered_cursor(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            admin_task_list_filtered_cursor_stmt_,
            "SELECT task_id, task_kind, status, operator_id, target_type, target_id, request_ip, user_agent, trace_id, "
            "request_json, result_json, failure_reason, UNIX_TIMESTAMP(execute_at), UNIX_TIMESTAMP(executed_at), "
            "UNIX_TIMESTAMP(created_at), UNIX_TIMESTAMP(updated_at) "
            "FROM admin_task "
            "WHERE (? = '' OR task_kind = ?) "
            "AND (? = '' OR status = ?) "
            "AND (created_at < FROM_UNIXTIME(?) OR (created_at = FROM_UNIXTIME(?) AND task_id < ?)) "
            "ORDER BY created_at DESC, task_id DESC LIMIT ?");
    }

    const mysql::statement& admin_task_count_filtered(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            admin_task_count_filtered_stmt_,
            "SELECT COUNT(*) "
            "FROM admin_task "
            "WHERE (? = '' OR task_kind = ?) "
            "AND (? = '' OR status = ?)");
    }

    const mysql::statement& admin_task_claim_due(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            admin_task_claim_due_stmt_,
            "SELECT task_id, task_kind, status, operator_id, target_type, target_id, request_ip, user_agent, trace_id, "
            "request_json, result_json, failure_reason, UNIX_TIMESTAMP(execute_at), UNIX_TIMESTAMP(executed_at), "
            "UNIX_TIMESTAMP(created_at), UNIX_TIMESTAMP(updated_at) "
            "FROM admin_task WHERE task_kind = ? AND status = 'scheduled' AND execute_at IS NOT NULL "
            "AND execute_at <= FROM_UNIXTIME(?) ORDER BY execute_at ASC LIMIT ?");
    }

    const mysql::statement& admin_task_mark_running(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            admin_task_mark_running_stmt_,
            "UPDATE admin_task SET status = 'running', updated_at = CURRENT_TIMESTAMP WHERE task_id = ?");
    }

    const mysql::statement& admin_task_mark_completed(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            admin_task_mark_completed_stmt_,
            "UPDATE admin_task SET status = 'completed', result_json = CAST(? AS JSON), executed_at = FROM_UNIXTIME(?), "
            "updated_at = CURRENT_TIMESTAMP WHERE task_id = ?");
    }

    const mysql::statement& admin_task_mark_failed(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            admin_task_mark_failed_stmt_,
            "UPDATE admin_task SET status = 'failed', failure_reason = ?, result_json = CAST(? AS JSON), "
            "executed_at = FROM_UNIXTIME(?), updated_at = CURRENT_TIMESTAMP WHERE task_id = ?");
    }

    const mysql::statement& admin_task_find_by_id(mysql::any_connection& connection) {
        return get_or_prepare(
            connection,
            admin_task_find_by_id_stmt_,
            "SELECT task_id, task_kind, status, operator_id, target_type, target_id, request_ip, user_agent, trace_id, "
            "request_json, result_json, failure_reason, UNIX_TIMESTAMP(execute_at), UNIX_TIMESTAMP(executed_at), "
            "UNIX_TIMESTAMP(created_at), UNIX_TIMESTAMP(updated_at) "
            "FROM admin_task WHERE task_id = ? LIMIT 1");
    }

private:
    struct PreparedStatementStore {
        std::unordered_map<std::string, mysql::statement> statements;
    };

    const mysql::statement& get_or_prepare(
        mysql::any_connection& connection,
        std::optional<mysql::statement>& /*slot*/,
        std::string_view sql) {
        std::scoped_lock lock{cache_mutex()};
        auto& store = connection_cache()[&connection];
        const auto key = std::string{sql};
        const auto it = store.statements.find(key);
        if (it != store.statements.end()) {
            return it->second;
        }
        auto [inserted, _] = store.statements.emplace(key, connection.prepare_statement(sql));
        return inserted->second;
    }

    static std::unordered_map<mysql::any_connection*, PreparedStatementStore>& connection_cache() {
        static std::unordered_map<mysql::any_connection*, PreparedStatementStore> cache;
        return cache;
    }

    static std::mutex& cache_mutex() {
        static std::mutex mutex;
        return mutex;
    }

    std::optional<mysql::statement> account_quick_insert_stmt_;
    std::optional<mysql::statement> account_quick_update_display_name_stmt_;
    std::optional<mysql::statement> account_password_insert_stmt_;
    std::optional<mysql::statement> account_find_by_login_stmt_;
    std::optional<mysql::statement> account_find_password_by_login_stmt_;
    std::optional<mysql::statement> account_find_by_id_stmt_;
    std::optional<mysql::statement> account_update_password_stmt_;
    std::optional<mysql::statement> account_issue_reset_token_stmt_;
    std::optional<mysql::statement> account_find_by_reset_token_stmt_;
    std::optional<mysql::statement> account_set_status_stmt_;
    std::optional<mysql::statement> account_record_login_success_stmt_;
    std::optional<mysql::statement> account_record_login_failure_stmt_;
    std::optional<mysql::statement> player_upsert_stmt_;
    std::optional<mysql::statement> player_find_by_id_stmt_;
    std::optional<mysql::statement> player_find_by_account_id_stmt_;
    std::optional<mysql::statement> player_list_all_stmt_;
    std::optional<mysql::statement> player_list_filtered_stmt_;
    std::optional<mysql::statement> player_count_filtered_stmt_;
    std::optional<mysql::statement> player_admin_list_filtered_stmt_;
    std::optional<mysql::statement> player_admin_list_filtered_cursor_stmt_;
    std::optional<mysql::statement> player_currency_spend_stmt_;
    std::optional<mysql::statement> player_inventory_upsert_stmt_;
    std::optional<mysql::statement> player_inventory_insert_ignore_stmt_;
    std::optional<mysql::statement> player_inventory_delete_stmt_;
    std::optional<mysql::statement> player_spend_gold_stmt_;
    std::optional<mysql::statement> player_spend_gem_stmt_;
    std::optional<mysql::statement> item_upsert_stmt_;
    std::optional<mysql::statement> item_find_by_id_stmt_;
    std::optional<mysql::statement> item_list_filtered_stmt_;
    std::optional<mysql::statement> item_count_filtered_stmt_;
    std::optional<mysql::statement> friend_exists_stmt_;
    std::optional<mysql::statement> friend_request_exists_stmt_;
    std::optional<mysql::statement> friend_insert_stmt_;
    std::optional<mysql::statement> friend_request_insert_stmt_;
    std::optional<mysql::statement> friend_list_stmt_;
    std::optional<mysql::statement> friend_list_by_last_message_stmt_;
    std::optional<mysql::statement> friend_list_by_last_message_cursor_stmt_;
    std::optional<mysql::statement> friend_request_list_incoming_stmt_;
    std::optional<mysql::statement> friend_request_list_outgoing_stmt_;
    std::optional<mysql::statement> friend_request_update_status_stmt_;
    std::optional<mysql::statement> friend_request_delete_pair_stmt_;
    std::optional<mysql::statement> friend_last_message_touch_stmt_;
    std::optional<mysql::statement> chat_block_upsert_stmt_;
    std::optional<mysql::statement> chat_block_delete_stmt_;
    std::optional<mysql::statement> chat_block_exists_stmt_;
    std::optional<mysql::statement> chat_block_list_stmt_;
    std::optional<mysql::statement> chat_insert_stmt_;
    std::optional<mysql::statement> chat_outbox_insert_stmt_;
    std::optional<mysql::statement> chat_outbox_list_stmt_;
    std::optional<mysql::statement> chat_outbox_delete_stmt_;
    std::optional<mysql::statement> chat_outbox_health_stmt_;
    std::optional<mysql::statement> chat_outbox_fail_stmt_;
    std::optional<mysql::statement> chat_outbox_retry_failed_stmt_;
    std::optional<mysql::statement> chat_recent_stmt_;
    std::optional<mysql::statement> chat_history_page_first_stmt_;
    std::optional<mysql::statement> chat_history_page_next_stmt_;
    std::optional<mysql::statement> chat_recall_stmt_;
    std::optional<mysql::statement> chat_find_by_id_stmt_;
    std::optional<mysql::statement> chat_mute_upsert_stmt_;
    std::optional<mysql::statement> chat_mute_clear_stmt_;
    std::optional<mysql::statement> chat_mute_find_active_stmt_;
    std::optional<mysql::statement> reward_grant_find_by_id_stmt_;
    std::optional<mysql::statement> reward_grant_insert_stmt_;
    std::optional<mysql::statement> reward_grant_list_by_player_stmt_;
    std::optional<mysql::statement> reward_grant_list_filtered_stmt_;
    std::optional<mysql::statement> reward_grant_count_filtered_stmt_;
    std::optional<mysql::statement> reward_template_upsert_stmt_;
    std::optional<mysql::statement> reward_template_find_by_id_stmt_;
    std::optional<mysql::statement> reward_template_list_all_stmt_;
    std::optional<mysql::statement> announcement_insert_stmt_;
    std::optional<mysql::statement> announcement_list_recent_stmt_;
    std::optional<mysql::statement> mail_insert_stmt_;
    std::optional<mysql::statement> mail_list_inbox_stmt_;
    std::optional<mysql::statement> mail_list_inbox_cursor_stmt_;
    std::optional<mysql::statement> mail_list_filtered_stmt_;
    std::optional<mysql::statement> mail_count_filtered_stmt_;
    std::optional<mysql::statement> mail_find_by_id_stmt_;
    std::optional<mysql::statement> mail_mark_read_stmt_;
    std::optional<mysql::statement> mail_mark_claimed_stmt_;
    std::optional<mysql::statement> mail_mark_deleted_stmt_;
    std::optional<mysql::statement> gm_audit_insert_stmt_;
    std::optional<mysql::statement> gm_audit_list_recent_stmt_;
    std::optional<mysql::statement> gm_audit_list_filtered_stmt_;
    std::optional<mysql::statement> gm_audit_list_filtered_cursor_stmt_;
    std::optional<mysql::statement> gm_audit_count_filtered_stmt_;
    std::optional<mysql::statement> admin_upsert_stmt_;
    std::optional<mysql::statement> admin_delete_permissions_stmt_;
    std::optional<mysql::statement> admin_insert_permission_stmt_;
    std::optional<mysql::statement> admin_find_by_account_id_stmt_;
    std::optional<mysql::statement> admin_list_permissions_stmt_;
    std::optional<mysql::statement> admin_has_permission_stmt_;
    std::optional<mysql::statement> admin_list_all_stmt_;
    std::optional<mysql::statement> admin_list_filtered_stmt_;
    std::optional<mysql::statement> admin_count_filtered_stmt_;
    std::optional<mysql::statement> admin_task_upsert_stmt_;
    std::optional<mysql::statement> admin_task_list_recent_stmt_;
    std::optional<mysql::statement> admin_task_list_filtered_stmt_;
    std::optional<mysql::statement> admin_task_list_filtered_cursor_stmt_;
    std::optional<mysql::statement> admin_task_count_filtered_stmt_;
    std::optional<mysql::statement> admin_task_claim_due_stmt_;
    std::optional<mysql::statement> admin_task_mark_running_stmt_;
    std::optional<mysql::statement> admin_task_mark_completed_stmt_;
    std::optional<mysql::statement> admin_task_mark_failed_stmt_;
    std::optional<mysql::statement> admin_task_find_by_id_stmt_;
};

// MySQL 连接池职责：
// 1. 预先维护一组可复用连接，避免高并发下每次请求都重新建连。
// 2. 用条件变量控制“借连接/还连接”，把最大并发 DB 连接数收敛到 pool_size。
// 3. 当连接异常时标记为 broken，下次借出时自动重连并清理该连接上的预处理语句缓存。
class MySqlConnectionPool {
public:
    struct Lease {
        Lease() = default;
        Lease(MySqlConnectionPool* pool, std::size_t index)
            : pool_(pool),
              index_(index) {}

        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;

        Lease(Lease&& other) noexcept
            : pool_(std::exchange(other.pool_, nullptr)),
              index_(std::exchange(other.index_, 0)) {}

        Lease& operator=(Lease&& other) noexcept {
            if (this != &other) {
                release();
                pool_ = std::exchange(other.pool_, nullptr);
                index_ = std::exchange(other.index_, 0);
            }
            return *this;
        }

        ~Lease() {
            release();
        }

        mysql::any_connection& connection() const {
            return pool_->entries_.at(index_)->connection;
        }

        void mark_broken() const {
            auto& entry = *pool_->entries_.at(index_);
            StatementCache::clear_connection(entry.connection);
            entry.connected = false;
        }

    private:
        void release() {
            if (pool_ == nullptr) {
                return;
            }
            pool_->release(index_);
            pool_ = nullptr;
        }

        MySqlConnectionPool* pool_ = nullptr;
        std::size_t index_ = 0;
    };

    explicit MySqlConnectionPool(MySqlOptions options)
        : options_(std::move(options)) {
        const auto pool_size = std::max<std::size_t>(1, options_.pool_size);
        entries_.reserve(pool_size);
        for (std::size_t index = 0; index < pool_size; ++index) {
            entries_.push_back(std::make_unique<Entry>());
        }
        set_pool_gauges();
    }

    Lease acquire() {
        const auto started_at = std::chrono::steady_clock::now();
        std::unique_lock lock{mutex_};
        const auto ready = [this] {
            return try_find_idle_index().has_value();
        };

        if (!ready()) {
            observe_pool_wait("wait");
        }

        const auto timeout = std::chrono::milliseconds{std::max<std::uint32_t>(1, options_.acquire_timeout_ms)};
        if (!condition_.wait_for(lock, timeout, ready)) {
            observe_pool_acquire(
                "timeout",
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started_at).count());
            throw std::runtime_error("mysql connection pool acquire timeout");
        }

        const auto index = *try_find_idle_index();
        auto& entry = *entries_.at(index);
        entry.in_use = true;
        ++in_use_count_;
        const auto waited_ms = std::chrono::duration<double, std::milli>(
                                   std::chrono::steady_clock::now() - started_at)
                                   .count();
        observe_pool_acquire("ok", waited_ms);
        set_pool_gauges();
        lock.unlock();
        try {
            const auto was_connected = entry.connected;
            ensure_connected(entry);
            observe_pool_connection(was_connected ? "reused" : "created");
        } catch (...) {
            observe_pool_connection("error");
            release(index);
            throw;
        }
        return Lease{this, index};
    }

private:
    struct Entry {
        Entry()
            : context(1),
              connection(context) {}

        asio::io_context context;
        mysql::any_connection connection;
        bool connected = false;
        bool in_use = false;
    };

    std::optional<std::size_t> try_find_idle_index() const {
        for (std::size_t index = 0; index < entries_.size(); ++index) {
            if (!entries_[index]->in_use) {
                return index;
            }
        }
        return std::nullopt;
    }

    void ensure_connected(Entry& entry) {
        if (entry.connected) {
            return;
        }

        mysql::connect_params params;
        params.server_address.emplace_host_and_port(options_.host, options_.port);
        params.username = options_.username;
        params.password = options_.password;
        params.database = options_.database;
        entry.connection.connect(params);
        entry.connected = true;
        StatementCache::clear_connection(entry.connection);
    }

    void release(std::size_t index) {
        std::scoped_lock lock{mutex_};
        entries_.at(index)->in_use = false;
        if (in_use_count_ > 0) {
            --in_use_count_;
        }
        set_pool_gauges();
        condition_.notify_one();
    }

    void set_pool_gauges() const {
        std::shared_ptr<mf::core::MetricsRegistry> metrics;
        {
            std::scoped_lock observability_lock{g_mysql_observability_mutex};
            metrics = g_mysql_observability.metrics;
        }
        if (metrics) {
            const mf::core::MetricLabels labels{{"pool", options_.database}};
            metrics->set_gauge("mf_mysql_pool_size", static_cast<double>(entries_.size()), labels);
            metrics->set_gauge("mf_mysql_pool_in_use", static_cast<double>(in_use_count_), labels);
        }
    }

    void observe_pool_wait(std::string_view state) const {
        std::scoped_lock observability_lock{g_mysql_observability_mutex};
        if (g_mysql_observability.metrics) {
            g_mysql_observability.metrics->increment_counter(
                "mf_mysql_pool_wait_total",
                1.0,
                {
                    {"state", std::string{state}},
                });
        }
    }

    void observe_pool_acquire(std::string_view status, double waited_ms) const {
        std::scoped_lock observability_lock{g_mysql_observability_mutex};
        if (g_mysql_observability.metrics) {
            g_mysql_observability.metrics->increment_counter(
                "mf_mysql_pool_acquire_total",
                1.0,
                {
                    {"status", std::string{status}},
                });
            g_mysql_observability.metrics->observe_histogram_ms(
                "mf_mysql_pool_wait_duration_ms",
                waited_ms,
                {
                    {"status", std::string{status}},
                });
        }
    }

    void observe_pool_connection(std::string_view status) const {
        std::scoped_lock observability_lock{g_mysql_observability_mutex};
        if (g_mysql_observability.metrics) {
            g_mysql_observability.metrics->increment_counter(
                "mf_mysql_pool_connection_events_total",
                1.0,
                {{"status", std::string{status}}});
        }
    }

    MySqlOptions options_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::vector<std::unique_ptr<Entry>> entries_;
    std::size_t in_use_count_ = 0;
};

// MySqlClient 是仓储层统一入口：
// 上层 Repository 只管声明“这次要跑什么 SQL”，
// 它负责从连接池借连接、挂 tracing/metrics、执行完成后归还连接。
class MySqlClient {
public:
    explicit MySqlClient(MySqlOptions options)
        : options_(std::move(options)),
          pool_(std::make_shared<MySqlConnectionPool>(options_)) {}

    template <class Fn>
    decltype(auto) with_connection(std::string_view operation_name, std::string_view statement_name, Fn&& fn) {
        MF_PROFILE_SCOPE("MySQL operation");
        auto lease = pool_->acquire();
        const auto started_at = std::chrono::steady_clock::now();
        auto tracer = mf::core::global_tracer();
        auto span = tracer ? tracer->start_span(
                                 "mysql." + std::string{operation_name},
                                 {
                                     {"db.system", "mysql"},
                                     {"db.operation", std::string{operation_name}},
                                     {"db.name", options_.database},
                                     {"db.statement.name", std::string{statement_name}},
                                 })
                           : mf::core::SpanScope{};
        try {
            MF_PROFILE_SCOPE("MySQL query and decode");
            if constexpr (std::is_void_v<std::invoke_result_t<Fn, mysql::any_connection&>>) {
                fn(lease.connection());
                if (span.valid()) {
                    span.set_status("ok");
                }
                observe_operation(operation_name, started_at, "ok");
            } else {
                auto value = fn(lease.connection());
                if (span.valid()) {
                    span.set_status("ok");
                }
                observe_operation(operation_name, started_at, "ok");
                return value;
            }
        } catch (...) {
            lease.mark_broken();
            if (span.valid()) {
                span.set_status("error");
            }
            observe_operation(operation_name, started_at, "error");
            throw;
        }
    }

    template <class Fn>
    decltype(auto) with_connection(std::string_view operation_name, Fn&& fn) {
        return with_connection(operation_name, operation_name, std::forward<Fn>(fn));
    }

private:
    void observe_operation(
        std::string_view operation_name,
        std::chrono::steady_clock::time_point started_at,
        std::string_view status) const {
        const auto duration_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started_at)
                                     .count();
        std::scoped_lock observability_lock{g_mysql_observability_mutex};
        if (g_mysql_observability.metrics) {
            g_mysql_observability.metrics->increment_counter(
                "mf_mysql_operations_total",
                1.0,
                {
                    {"operation", std::string{operation_name}},
                    {"status", std::string{status}},
                });
            g_mysql_observability.metrics->observe_histogram_ms(
                "mf_mysql_operation_duration_ms",
                static_cast<double>(duration_ms),
                {
                    {"operation", std::string{operation_name}},
                });
            if (duration_ms >= static_cast<double>(g_mysql_observability.slow_operation_threshold_ms)) {
                g_mysql_observability.metrics->increment_counter(
                    "mf_alert_events_total",
                    1.0,
                    {
                        {"category", "slow_mysql_operation"},
                        {"operation", std::string{operation_name}},
                    });
            }
        }
        if (g_mysql_observability.logger && duration_ms >= static_cast<double>(g_mysql_observability.slow_operation_threshold_ms)) {
            g_mysql_observability.logger->log(
                mf::core::LogLevel::warn,
                "slow_mysql_operation operation=" + std::string{operation_name} + " status=" + std::string{status} +
                    " duration_ms=" + std::to_string(duration_ms));
        }
    }

    MySqlOptions options_;
    std::shared_ptr<MySqlConnectionPool> pool_;
};

std::string mysql_client_cache_key(const MySqlOptions& options) {
    std::ostringstream stream;
    stream << options.host << ':' << options.port << '|'
           << options.username << '|' << options.password << '|'
           << options.database << '|' << options.pool_size << '|'
           << options.acquire_timeout_ms;
    return stream.str();
}

std::shared_ptr<MySqlClient> shared_mysql_client(const MySqlOptions& options) {
    static std::mutex mutex;
    static std::unordered_map<std::string, std::weak_ptr<MySqlClient>> cache;

    const auto key = mysql_client_cache_key(options);
    std::scoped_lock lock{mutex};
    if (const auto existing = cache[key].lock()) {
        return existing;
    }

    auto created = std::make_shared<MySqlClient>(options);
    cache[key] = created;
    return created;
}

std::string make_hash_hex(std::string_view source) {
    constexpr std::uint64_t offset_basis = 14695981039346656037ull;
    constexpr std::uint64_t prime = 1099511628211ull;

    std::uint64_t hash = offset_basis;
    for (const unsigned char ch : source) {
        hash ^= ch;
        hash *= prime;
    }

    std::ostringstream output;
    output << std::hex << std::nouppercase << std::setw(16) << std::setfill('0') << hash;
    return output.str();
}

std::string make_stable_id(std::string_view prefix, std::string_view source) {
    return std::string(prefix) + make_hash_hex(source);
}

std::uint64_t make_hash_u64(std::string_view source) {
    constexpr std::uint64_t offset_basis = 14695981039346656037ull;
    constexpr std::uint64_t prime = 1099511628211ull;

    std::uint64_t hash = offset_basis;
    for (const unsigned char ch : source) {
        hash ^= ch;
        hash *= prime;
    }
    return hash;
}

std::string make_numeric_player_id(std::string_view source, std::uint32_t offset = 0) {
    constexpr std::uint32_t min_player_id = 10000000;
    constexpr std::uint32_t player_id_span = 90000000;

    const auto base = static_cast<std::uint32_t>(make_hash_u64(source) % player_id_span);
    const auto value = min_player_id + ((base + offset) % player_id_span);

    std::ostringstream output;
    output << std::setw(8) << std::setfill('0') << value;
    return output.str();
}

std::string make_chat_message_id(const mf::game::ChatMessage& message) {
    static std::atomic<std::uint64_t> sequence = 1;

    const auto tick = std::chrono::duration_cast<std::chrono::microseconds>(
                          message.sent_at.time_since_epoch())
                          .count();
    std::ostringstream source;
    source << message.channel.channel_id << '|'
           << message.sender_player_id << '|'
           << message.sender_nickname << '|'
           << message.text << '|'
           << tick << '|'
           << sequence.fetch_add(1, std::memory_order_relaxed);
    return "msg_" + make_hash_hex(source.str());
}

std::string make_mail_id(const mf::game::MailMessage& mail) {
    static std::atomic<std::uint64_t> sequence = 1;

    const auto tick =
        std::chrono::duration_cast<std::chrono::microseconds>(
            mail.sent_at.value_or(std::chrono::system_clock::now()).time_since_epoch())
            .count();
    std::ostringstream source;
    source << mail.player_id << '|'
           << mail.sender_name << '|'
           << mail.title << '|'
           << tick << '|'
           << sequence.fetch_add(1, std::memory_order_relaxed);
    return "mail_" + make_hash_hex(source.str());
}

std::string make_audit_id(std::string_view action, std::string_view target_id) {
    static std::atomic<std::uint64_t> sequence = 1;
    const auto tick = std::chrono::duration_cast<std::chrono::microseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
    std::ostringstream source;
    source << action << '|'
           << target_id << '|'
           << tick << '|'
           << sequence.fetch_add(1, std::memory_order_relaxed);
    return "audit_" + make_hash_hex(source.str());
}

std::string make_inventory_instance_id() {
    // 背包实例 ID 必须走统一生成入口。
    // 否则默认背包和后续发奖各自维护独立序列时，会出现 instance_id 冲突。
    static std::atomic<std::uint64_t> sequence = 1;
    return "item_inst_" + std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
}

std::vector<mf::game::InventoryItem> default_inventory() {
    return {
        mf::game::InventoryItem{
            .instance_id = make_inventory_instance_id(),
            .item_id = "starter_sword",
            .quantity = 1,
        },
        mf::game::InventoryItem{
            .instance_id = make_inventory_instance_id(),
            .item_id = "starter_potion",
            .quantity = 1,
        },
    };
}

std::string reward_kind_to_string(mf::game::RewardKind kind) {
    switch (kind) {
    case mf::game::RewardKind::gold:
        return "gold";
    case mf::game::RewardKind::currency:
        return "currency";
    case mf::game::RewardKind::item:
        return "item";
    case mf::game::RewardKind::attachment_template:
        return "attachment_template";
    }
    return "gold";
}

mf::game::RewardKind parse_reward_kind(std::string_view value) {
    if (value == "currency") {
        return mf::game::RewardKind::currency;
    }
    if (value == "item") {
        return mf::game::RewardKind::item;
    }
    if (value == "attachment_template") {
        return mf::game::RewardKind::attachment_template;
    }
    return mf::game::RewardKind::gold;
}

std::string encode_rewards_json(const std::vector<mf::game::RewardEntry>& rewards) {
    boost::json::array items;
    items.reserve(rewards.size());
    for (const auto& reward : rewards) {
        items.emplace_back(boost::json::object{
            {"kind", reward_kind_to_string(reward.kind)},
            {"resource_id", reward.resource_id},
            {"amount", reward.amount},
        });
    }
    return boost::json::serialize(items);
}

std::vector<mf::game::RewardEntry> decode_rewards_json(std::string_view payload) {
    if (payload.empty()) {
        return {};
    }

    std::vector<mf::game::RewardEntry> rewards;
    const auto parsed = boost::json::parse(payload);
    const auto& array = parsed.as_array();
    rewards.reserve(array.size());
    for (const auto& value : array) {
        const auto& object = value.as_object();
        rewards.push_back(mf::game::RewardEntry{
            .kind = parse_reward_kind(boost::json::value_to<std::string>(object.at("kind"))),
            .resource_id =
                object.if_contains("resource_id")
                    ? boost::json::value_to<std::string>(object.at("resource_id"))
                    : std::string{},
            .amount = object.at("amount").as_int64(),
        });
    }
    return rewards;
}

std::string column_string(const mysql::row_view& row, std::size_t index) {
    return std::string(row.at(index).as_string());
}

std::int64_t column_i64(const mysql::row_view& row, std::size_t index) {
    return row.at(index).as_int64();
}

std::optional<std::string> column_optional_string(const mysql::row_view& row, std::size_t index) {
    if (row.at(index).is_null()) {
        return std::nullopt;
    }
    return column_string(row, index);
}

std::optional<std::int64_t> column_optional_i64(const mysql::row_view& row, std::size_t index) {
    if (row.at(index).is_null()) {
        return std::nullopt;
    }
    return column_i64(row, index);
}

std::chrono::system_clock::time_point unix_seconds_to_time_point(std::int64_t unix_seconds) {
    return std::chrono::system_clock::time_point{std::chrono::seconds{unix_seconds}};
}

std::int64_t to_unix_seconds(std::chrono::system_clock::time_point time_point) {
    return std::chrono::duration_cast<std::chrono::seconds>(time_point.time_since_epoch()).count();
}

mf::game::AccountAuthMode parse_account_auth_mode(std::string_view value) {
    if (value == "password") {
        return mf::game::AccountAuthMode::password;
    }
    return mf::game::AccountAuthMode::quick;
}

mf::game::AccountStatus parse_account_status(std::string_view value) {
    if (value == "frozen") {
        return mf::game::AccountStatus::frozen;
    }
    if (value == "banned") {
        return mf::game::AccountStatus::banned;
    }
    return mf::game::AccountStatus::active;
}

mf::game::FriendRequestStatus parse_friend_request_status(std::string_view value) {
    if (value == "pending_outgoing") {
        return mf::game::FriendRequestStatus::pending_outgoing;
    }
    if (value == "accepted") {
        return mf::game::FriendRequestStatus::accepted;
    }
    return mf::game::FriendRequestStatus::pending_incoming;
}

std::optional<std::chrono::system_clock::time_point> optional_time_point_from_row(
    const mysql::row_view& row,
    std::size_t index) {
    if (const auto value = column_optional_i64(row, index)) {
        return unix_seconds_to_time_point(*value);
    }
    return std::nullopt;
}

mf::game::Account row_to_account(const mysql::row_view& row) {
    return mf::game::Account{
        .account_id = column_string(row, 0),
        .login_name = column_string(row, 1),
        .display_name = column_string(row, 2),
        .email = column_string(row, 3),
        .phone_number = column_string(row, 4),
        .platform_channel = column_string(row, 5),
        .register_source = column_string(row, 6),
        .auth_mode = parse_account_auth_mode(column_string(row, 7)),
        .status = parse_account_status(column_string(row, 8)),
        .status_reason = column_string(row, 9),
        .failed_login_attempts = column_i64(row, 10),
        .locked_until = optional_time_point_from_row(row, 11),
        .last_login_at = optional_time_point_from_row(row, 12),
        .last_login_ip = column_string(row, 13),
        .created_at = optional_time_point_from_row(row, 14),
        .updated_at = optional_time_point_from_row(row, 15),
    };
}

std::vector<mf::game::CurrencyBalance> load_player_currencies(
    mysql::any_connection& connection,
    const std::string& player_id) {
    mysql::results result;
    const auto statement = StatementCache::prepare(connection,
        "SELECT currency_id, amount FROM player_currency WHERE player_id = ? ORDER BY currency_id ASC");
    connection.execute(statement.bind(player_id), result);
    std::vector<mf::game::CurrencyBalance> currencies;
    currencies.reserve(result.rows().size());
    for (const auto row : result.rows()) {
        currencies.push_back(mf::game::CurrencyBalance{
            .currency_id = column_string(row, 0),
            .amount = column_i64(row, 1),
        });
    }
    return currencies;
}

std::vector<mf::game::InventoryItem> load_player_inventory(
    mysql::any_connection& connection,
    const std::string& player_id) {
    mysql::results result;
    const auto statement = StatementCache::prepare(connection,
        "SELECT item_id, instance_id, quantity FROM player_inventory WHERE player_id = ? ORDER BY item_id ASC");
    connection.execute(statement.bind(player_id), result);
    std::vector<mf::game::InventoryItem> inventory;
    inventory.reserve(result.rows().size());
    for (const auto row : result.rows()) {
        inventory.push_back(mf::game::InventoryItem{
            .instance_id = column_string(row, 1),
            .item_id = column_string(row, 0),
            .quantity = column_i64(row, 2),
        });
    }
    return inventory;
}

// 玩家主行只保留 player_id / account_id / nickname / gold / gem 五个标量列；
// 货币和背包在独立行表里按行原子更新，避免同一玩家写入在 player_profile 行上串行化。
mf::game::PlayerProfile row_to_profile(mysql::any_connection& connection, const mysql::row_view& row) {
    const auto gold = column_i64(row, 3);
    const auto gem = column_i64(row, 4);
    mf::game::PlayerProfile profile{
        .player_id = column_string(row, 0),
        .account_id = column_string(row, 1),
        .nickname = column_string(row, 2),
        .gold = gold,
        .gem = gem,
        .currencies = {
            mf::game::CurrencyBalance{.currency_id = "gold", .amount = gold},
            mf::game::CurrencyBalance{.currency_id = "gem", .amount = gem},
        },
    };
    auto currencies = load_player_currencies(connection, profile.player_id);
    for (auto& currency : currencies) {
        if (currency.currency_id == "gold" || currency.currency_id == "gem") {
            continue;
        }
        profile.currencies.push_back(std::move(currency));
    }
    profile.inventory = load_player_inventory(connection, profile.player_id);
    return profile;
}

// /players/me 等单资料读取走单条 SQL，把主行 + 货币 + 背包一次带回；
// 明细用 JSON_ARRAYAGG 聚合到两列，避免原来的 1+2 次往返。排序与分表读取一致，
// 写路径同样复用，返回结果无陈旧数据。
std::optional<mf::game::PlayerProfile> load_player_profile_single_query(
    mysql::any_connection& connection,
    std::string_view lookup_column,
    const std::string& lookup_value) {
    mysql::results result;
    const auto statement = StatementCache::prepare(connection,
        "SELECT p.player_id, p.account_id, p.nickname, p.gold, p.gem, "
        "  CAST(COALESCE((SELECT JSON_ARRAYAGG(obj) FROM ("
        "    SELECT JSON_OBJECT('currency_id', c.currency_id, 'amount', c.amount) AS obj "
        "    FROM player_currency c WHERE c.player_id = p.player_id ORDER BY c.currency_id"
        "  ) AS ordered_currencies), CAST('[]' AS JSON)) AS CHAR) AS currencies_json, "
        "  CAST(COALESCE((SELECT JSON_ARRAYAGG(obj) FROM ("
        "    SELECT JSON_OBJECT('item_id', i.item_id, 'instance_id', i.instance_id, 'quantity', i.quantity) AS obj "
        "    FROM player_inventory i WHERE i.player_id = p.player_id ORDER BY i.item_id"
        "  ) AS ordered_inventory), CAST('[]' AS JSON)) AS CHAR) AS inventory_json "
        "FROM player_profile p WHERE p." + std::string{lookup_column} + " = ?");
    connection.execute(statement.bind(lookup_value), result);
    if (result.rows().empty()) {
        return std::nullopt;
    }

    const auto row = result.rows().at(0);
    const auto gold = column_i64(row, 3);
    const auto gem = column_i64(row, 4);
    mf::game::PlayerProfile profile{
        .player_id = column_string(row, 0),
        .account_id = column_string(row, 1),
        .nickname = column_string(row, 2),
        .gold = gold,
        .gem = gem,
        .currencies = {
            mf::game::CurrencyBalance{.currency_id = "gold", .amount = gold},
            mf::game::CurrencyBalance{.currency_id = "gem", .amount = gem},
        },
    };

    const auto currencies_json = column_string(row, 5);
    if (!currencies_json.empty()) {
        const auto currencies = boost::json::parse(currencies_json).as_array();
        profile.currencies.reserve(profile.currencies.size() + currencies.size());
        for (const auto& value : currencies) {
            const auto& item = value.as_object();
            const auto currency_id = boost::json::value_to<std::string>(item.at("currency_id"));
            if (currency_id == "gold" || currency_id == "gem") {
                continue;
            }
            profile.currencies.push_back(mf::game::CurrencyBalance{
                .currency_id = currency_id,
                .amount = item.at("amount").as_int64(),
            });
        }
    }

    const auto inventory_json = column_string(row, 6);
    if (!inventory_json.empty()) {
        const auto inventory = boost::json::parse(inventory_json).as_array();
        profile.inventory.reserve(inventory.size());
        for (const auto& value : inventory) {
            const auto& item = value.as_object();
            profile.inventory.push_back(mf::game::InventoryItem{
                .instance_id = boost::json::value_to<std::string>(item.at("instance_id")),
                .item_id = boost::json::value_to<std::string>(item.at("item_id")),
                .quantity = item.at("quantity").as_int64(),
            });
        }
    }
    return profile;
}

// 把一组 player_id 编码成 JSON 数组，配合 MySQL JSON_TABLE 一次查出多人的明细行，
// 避免“每个玩家再单独查一次货币/背包”的 N+1 往返。JSON 数组只用一个绑定参数，
// 预处理语句不随列表长度变化，避免重新引入 prepared statement 数量增长。
std::string encode_player_id_json_array(const std::vector<std::string>& player_ids) {
    boost::json::array ids;
    ids.reserve(player_ids.size());
    for (const auto& player_id : player_ids) {
        ids.emplace_back(player_id);
    }
    return boost::json::serialize(ids);
}

using CurrencyByPlayer = std::unordered_map<std::string, std::vector<mf::game::CurrencyBalance>>;
using InventoryByPlayer = std::unordered_map<std::string, std::vector<mf::game::InventoryItem>>;

CurrencyByPlayer load_currencies_batch(
    mysql::any_connection& connection,
    const std::string& player_ids_json) {
    CurrencyByPlayer by_player;
    mysql::results result;
    const auto statement = StatementCache::prepare(connection,
        "SELECT c.player_id, c.currency_id, c.amount "
        "FROM player_currency c "
        "JOIN JSON_TABLE(?, '$[*]' COLUMNS(player_id VARCHAR(64) PATH '$')) AS ids "
        "ON ids.player_id = c.player_id COLLATE utf8mb4_0900_ai_ci "
        "ORDER BY c.player_id, c.currency_id");
    connection.execute(statement.bind(player_ids_json), result);
    for (const auto row : result.rows()) {
        by_player[column_string(row, 0)].push_back(mf::game::CurrencyBalance{
            .currency_id = column_string(row, 1),
            .amount = column_i64(row, 2),
        });
    }
    return by_player;
}

InventoryByPlayer load_inventory_batch(
    mysql::any_connection& connection,
    const std::string& player_ids_json) {
    InventoryByPlayer by_player;
    mysql::results result;
    const auto statement = StatementCache::prepare(connection,
        "SELECT i.player_id, i.item_id, i.instance_id, i.quantity "
        "FROM player_inventory i "
        "JOIN JSON_TABLE(?, '$[*]' COLUMNS(player_id VARCHAR(64) PATH '$')) AS ids "
        "ON ids.player_id = i.player_id COLLATE utf8mb4_0900_ai_ci "
        "ORDER BY i.player_id, i.item_id");
    connection.execute(statement.bind(player_ids_json), result);
    for (const auto row : result.rows()) {
        by_player[column_string(row, 0)].push_back(mf::game::InventoryItem{
            .instance_id = column_string(row, 2),
            .item_id = column_string(row, 1),
            .quantity = column_i64(row, 3),
        });
    }
    return by_player;
}

mf::game::PlayerProfile profile_from_row_and_maps(
    const mysql::row_view& row,
    const CurrencyByPlayer& currencies_by_player,
    const InventoryByPlayer& inventory_by_player) {
    const auto gold = column_i64(row, 3);
    const auto gem = column_i64(row, 4);
    mf::game::PlayerProfile profile{
        .player_id = column_string(row, 0),
        .account_id = column_string(row, 1),
        .nickname = column_string(row, 2),
        .gold = gold,
        .gem = gem,
        .currencies = {
            mf::game::CurrencyBalance{.currency_id = "gold", .amount = gold},
            mf::game::CurrencyBalance{.currency_id = "gem", .amount = gem},
        },
    };

    if (const auto it = currencies_by_player.find(profile.player_id); it != currencies_by_player.end()) {
        for (const auto& currency : it->second) {
            if (currency.currency_id == "gold" || currency.currency_id == "gem") {
                continue;
            }
            profile.currencies.push_back(currency);
        }
    }
    if (const auto it = inventory_by_player.find(profile.player_id); it != inventory_by_player.end()) {
        profile.inventory = it->second;
    }
    return profile;
}

void add_gold_on_connection(
    mysql::any_connection& connection,
    const std::string& player_id,
    std::int64_t amount) {
    mysql::results result;
    const auto statement = StatementCache::prepare(connection,
        "UPDATE player_profile SET gold = gold + ?, updated_at = CURRENT_TIMESTAMP WHERE player_id = ?");
    connection.execute(statement.bind(amount, player_id), result);
}

void add_gem_on_connection(
    mysql::any_connection& connection,
    const std::string& player_id,
    std::int64_t amount) {
    mysql::results result;
    const auto statement = StatementCache::prepare(connection,
        "UPDATE player_profile SET gem = gem + ?, updated_at = CURRENT_TIMESTAMP WHERE player_id = ?");
    connection.execute(statement.bind(amount, player_id), result);
}

void add_currency_on_connection(
    mysql::any_connection& connection,
    const std::string& player_id,
    const std::string& currency_id,
    std::int64_t amount) {
    mysql::results result;
    const auto statement = StatementCache::prepare(connection,
        "INSERT INTO player_currency(player_id, currency_id, amount) VALUES (?, ?, ?) "
        "ON DUPLICATE KEY UPDATE amount = amount + VALUES(amount)");
    connection.execute(statement.bind(player_id, currency_id, amount), result);
}

void add_inventory_on_connection(
    mysql::any_connection& connection,
    const std::string& player_id,
    const std::string& item_id,
    std::int64_t amount) {
    mysql::results result;
    const auto statement = StatementCache::prepare(connection,
        "INSERT INTO player_inventory(player_id, item_id, instance_id, quantity) VALUES (?, ?, ?, ?) "
        "ON DUPLICATE KEY UPDATE quantity = quantity + VALUES(quantity)");
    connection.execute(statement.bind(player_id, item_id, make_inventory_instance_id(), amount), result);
}

// 调用方必须已经开启事务；本函数用原子行更新发奖，不再对 player_profile 整行加锁。
std::optional<mf::game::PlayerProfile> apply_rewards_in_transaction(
    mysql::any_connection& connection,
    const std::string& player_id,
    const std::vector<mf::game::RewardEntry>& rewards) {
    mysql::results player_result;
    const auto find_statement = StatementCache::prepare(connection,
        "SELECT player_id, account_id, nickname, gold, gem FROM player_profile WHERE player_id = ?");
    connection.execute(find_statement.bind(player_id), player_result);
    if (player_result.rows().empty()) {
        return std::nullopt;
    }

    for (const auto& reward : rewards) {
        if (reward.amount <= 0) {
            continue;
        }
        switch (reward.kind) {
        case mf::game::RewardKind::gold:
            add_gold_on_connection(connection, player_id, reward.amount);
            break;
        case mf::game::RewardKind::currency:
            if (reward.resource_id == "gold") {
                add_gold_on_connection(connection, player_id, reward.amount);
            } else if (reward.resource_id == "gem") {
                add_gem_on_connection(connection, player_id, reward.amount);
            } else {
                add_currency_on_connection(connection, player_id, reward.resource_id, reward.amount);
            }
            break;
        case mf::game::RewardKind::item:
            add_inventory_on_connection(connection, player_id, reward.resource_id, reward.amount);
            break;
        case mf::game::RewardKind::attachment_template:
            break;
        }
    }

    // 奖励通过原子 SQL 直接落库，gold/gem 列会变化；重新读取主行后再拼装完整资料，
    // 避免返回旧值（货币和背包会随本次行读取拿到最新值）。
    mysql::results refreshed_result;
    connection.execute(find_statement.bind(player_id), refreshed_result);
    if (refreshed_result.rows().empty()) {
        return std::nullopt;
    }
    return row_to_profile(connection, refreshed_result.rows().at(0));
}

std::string encode_string_array(const std::vector<std::string>& values) {
    boost::json::array array;
    for (const auto& value : values) {
        array.emplace_back(value);
    }
    return boost::json::serialize(array);
}

std::vector<std::string> decode_string_array(std::string_view payload) {
    std::vector<std::string> values;
    if (payload.empty()) {
        return values;
    }
    for (const auto& value : boost::json::parse(payload).as_array()) {
        values.push_back(boost::json::value_to<std::string>(value));
    }
    return values;
}

std::string encode_quest_objectives(const std::vector<mf::game::QuestObjectiveDefinition>& objectives) {
    boost::json::array array;
    for (const auto& objective : objectives) {
        array.emplace_back(boost::json::object{
            {"objective_id", objective.objective_id},
            {"event_type", objective.event_type},
            {"target_id", objective.target_id},
            {"target_value", objective.target_value},
        });
    }
    return boost::json::serialize(array);
}

std::vector<mf::game::QuestObjectiveDefinition> decode_quest_objectives(std::string_view payload) {
    std::vector<mf::game::QuestObjectiveDefinition> objectives;
    if (payload.empty()) {
        return objectives;
    }
    for (const auto& value : boost::json::parse(payload).as_array()) {
        const auto& object = value.as_object();
        objectives.push_back({
            .objective_id = boost::json::value_to<std::string>(object.at("objective_id")),
            .event_type = boost::json::value_to<std::string>(object.at("event_type")),
            .target_id = object.if_contains("target_id")
                             ? boost::json::value_to<std::string>(object.at("target_id"))
                             : std::string{},
            .target_value = object.at("target_value").as_int64(),
        });
    }
    return objectives;
}

std::string quest_claim_mode_to_string(mf::game::QuestClaimMode mode) {
    switch (mode) {
    case mf::game::QuestClaimMode::automatic: return "auto";
    case mf::game::QuestClaimMode::npc: return "npc";
    case mf::game::QuestClaimMode::manual: return "manual";
    }
    return "manual";
}

mf::game::QuestClaimMode parse_quest_claim_mode(std::string_view value) {
    if (value == "auto") return mf::game::QuestClaimMode::automatic;
    if (value == "npc") return mf::game::QuestClaimMode::npc;
    return mf::game::QuestClaimMode::manual;
}

std::string quest_state_to_string(mf::game::QuestState state) {
    switch (state) {
    case mf::game::QuestState::accepted: return "accepted";
    case mf::game::QuestState::completed: return "completed";
    case mf::game::QuestState::claimed: return "claimed";
    }
    return "accepted";
}

mf::game::QuestState parse_quest_state(std::string_view value) {
    if (value == "completed") return mf::game::QuestState::completed;
    if (value == "claimed") return mf::game::QuestState::claimed;
    return mf::game::QuestState::accepted;
}

mf::game::QuestDefinition row_to_quest_definition(const mysql::row_view& row) {
    return {
        .quest_id = column_string(row, 0),
        .version = column_i64(row, 1),
        .title = column_string(row, 2),
        .description = column_string(row, 3),
        .claim_mode = parse_quest_claim_mode(column_string(row, 4)),
        .turn_in_npc_id = column_string(row, 5),
        .completion_flag = column_string(row, 6),
        .pre_quest_ids = decode_string_array(column_string(row, 7)),
        .required_flags = decode_string_array(column_string(row, 8)),
        .objectives = decode_quest_objectives(column_string(row, 9)),
        .rewards = decode_rewards_json(column_string(row, 10)),
        .enabled = column_i64(row, 11) != 0,
        .created_at = optional_time_point_from_row(row, 12),
        .updated_at = optional_time_point_from_row(row, 13),
    };
}

mf::game::PlayerAdminRow row_to_player_admin_row(const mysql::row_view& row) {
    return mf::game::PlayerAdminRow{
        .player_id = column_string(row, 0),
        .account_id = column_string(row, 1),
        .nickname = column_string(row, 2),
        .gold = column_i64(row, 3),
        .gem = column_i64(row, 4),
        .status = parse_account_status(column_string(row, 5)),
        .mute_channel_id = column_string(row, 8),
        .muted_until = optional_time_point_from_row(row, 9),
        .last_login_at = optional_time_point_from_row(row, 6),
        .last_login_ip = column_string(row, 7),
    };
}

mf::game::ItemDefinition row_to_item_definition(const mysql::row_view& row) {
    return mf::game::ItemDefinition{
        .item_id = column_string(row, 0),
        .name = column_string(row, 1),
        .description = column_string(row, 2),
        .icon = column_string(row, 3),
        .rarity = column_string(row, 4),
        .max_stack = column_i64(row, 5),
        .item_type = column_string(row, 6),
        .enabled = column_i64(row, 7) != 0,
        .created_at = optional_time_point_from_row(row, 8),
        .updated_at = optional_time_point_from_row(row, 9),
    };
}

mf::game::GachaPool row_to_gacha_pool(const mysql::row_view& row) {
    return mf::game::GachaPool{
        .pool_id = column_string(row, 0),
        .pool_type = column_string(row, 1),
        .name = column_string(row, 2),
        .description = column_string(row, 3),
        .banner_icon = column_string(row, 4),
        .cost_currency_id = column_string(row, 5),
        .cost_amount_per_draw = column_i64(row, 6),
        .pity_5 = column_i64(row, 7),
        .pity_4 = column_i64(row, 8),
        .enabled = column_i64(row, 9) != 0,
        .starts_at = optional_time_point_from_row(row, 10),
        .ends_at = optional_time_point_from_row(row, 11),
        .created_at = optional_time_point_from_row(row, 12),
        .updated_at = optional_time_point_from_row(row, 13),
    };
}

mf::game::GachaPoolItem row_to_gacha_pool_item(const mysql::row_view& row) {
    return mf::game::GachaPoolItem{
        .pool_id = column_string(row, 0),
        .item_key = column_string(row, 1),
        .rarity = column_i64(row, 2),
        .weight = column_i64(row, 3),
        .is_up = column_i64(row, 4) != 0,
        .reward = mf::game::RewardEntry{
            .kind = parse_reward_kind(column_string(row, 5)),
            .resource_id = column_string(row, 6),
            .amount = column_i64(row, 7),
        },
    };
}

mf::game::GachaPlayerState row_to_gacha_player_state(const mysql::row_view& row) {
    return mf::game::GachaPlayerState{
        .player_id = column_string(row, 0),
        .pool_id = column_string(row, 1),
        .pity_5_count = column_i64(row, 2),
        .pity_4_count = column_i64(row, 3),
        .total_draws = column_i64(row, 4),
        .updated_at = optional_time_point_from_row(row, 5),
    };
}

mf::game::GachaHistoryEntry row_to_gacha_history(const mysql::row_view& row) {
    return mf::game::GachaHistoryEntry{
        .history_id = column_string(row, 0),
        .player_id = column_string(row, 1),
        .pool_id = column_string(row, 2),
        .item_key = column_string(row, 3),
        .rarity = column_i64(row, 4),
        .is_up = column_i64(row, 5) != 0,
        .reward = mf::game::RewardEntry{
            .kind = parse_reward_kind(column_string(row, 6)),
            .resource_id = column_string(row, 7),
            .amount = column_i64(row, 8),
        },
        .pity_5_before = column_i64(row, 9),
        .pity_4_before = column_i64(row, 10),
        .drawn_at = optional_time_point_from_row(row, 11),
    };
}

mf::game::ShopProduct row_to_shop_product(const mysql::row_view& row) {
    return mf::game::ShopProduct{
        .product_id = column_string(row, 0),
        .category = column_string(row, 1),
        .name = column_string(row, 2),
        .description = column_string(row, 3),
        .icon = column_string(row, 4),
        .price_currency_id = column_string(row, 5),
        .price_amount = column_i64(row, 6),
        .rewards = decode_rewards_json(column_string(row, 7)),
        .purchase_limit = column_i64(row, 8),
        .enabled = column_i64(row, 9) != 0,
        .starts_at = optional_time_point_from_row(row, 10),
        .ends_at = optional_time_point_from_row(row, 11),
        .created_at = optional_time_point_from_row(row, 12),
        .updated_at = optional_time_point_from_row(row, 13),
    };
}

mf::game::ShopPurchaseRecord row_to_shop_purchase(const mysql::row_view& row) {
    return mf::game::ShopPurchaseRecord{
        .purchase_id = column_string(row, 0),
        .player_id = column_string(row, 1),
        .product_id = column_string(row, 2),
        .price_currency_id = column_string(row, 3),
        .price_amount = column_i64(row, 4),
        .rewards = decode_rewards_json(column_string(row, 5)),
        .purchased_at = optional_time_point_from_row(row, 6),
    };
}

mf::game::LootDrop row_to_loot_drop(const mysql::row_view& row) {
    return mf::game::LootDrop{
        .drop_id = column_string(row, 0),
        .owner_player_id = column_string(row, 1),
        .scene_id = column_string(row, 2),
        .rewards = decode_rewards_json(column_string(row, 3)),
        .picked = column_i64(row, 4) != 0,
        .picked_by_player_id = column_string(row, 5),
        .spawned_at = optional_time_point_from_row(row, 6),
        .expires_at = optional_time_point_from_row(row, 7),
        .picked_at = optional_time_point_from_row(row, 8),
    };
}

mf::game::ChatChannel row_to_channel(const mysql::row_view& row) {
    const auto kind_text = column_string(row, 2);
    const auto kind = mf::chat::parse_channel_kind(kind_text).value_or(mf::game::ChatChannelKind::lobby);

    return mf::game::ChatChannel{
        .channel_id = column_string(row, 1),
        .kind = kind,
        .scope_id = column_string(row, 3),
        .display_name = kind_text,
    };
}

mf::game::ChatMessage row_to_message(const mysql::row_view& row) {
    const auto kind_text = column_string(row, 4);
    const auto kind = mf::chat::parse_message_kind(kind_text).value_or(mf::game::ChatMessageKind::text);

    return mf::game::ChatMessage{
        .message_id = column_string(row, 0),
        .channel = row_to_channel(row),
        .kind = kind,
        .sender_player_id = column_string(row, 5),
        .sender_nickname = column_string(row, 6),
        .text = column_string(row, 7),
        .recalled = column_i64(row, 8) > 0,
        .recalled_by_player_id = column_optional_string(row, 9).value_or(std::string{}),
        .recalled_at = optional_time_point_from_row(row, 10),
        .sent_at = unix_seconds_to_time_point(column_i64(row, 11)),
    };
}

mf::game::FriendRequest row_to_friend_request(const mysql::row_view& row) {
    return mf::game::FriendRequest{
        .requester_player_id = column_string(row, 0),
        .target_player_id = column_string(row, 1),
        .status = parse_friend_request_status(column_string(row, 2)),
        .created_at = optional_time_point_from_row(row, 3),
    };
}

mf::game::ChatBlockEntry row_to_chat_block(const mysql::row_view& row) {
    return mf::game::ChatBlockEntry{
        .player_id = column_string(row, 0),
        .blocked_player_id = column_string(row, 1),
        .created_at = optional_time_point_from_row(row, 2),
    };
}

mf::game::ChatMuteEntry row_to_chat_mute(const mysql::row_view& row) {
    return mf::game::ChatMuteEntry{
        .channel_id = column_string(row, 0),
        .muted_player_id = column_string(row, 1),
        .muted_by_player_id = column_string(row, 2),
        .reason = column_string(row, 3),
        .muted_until = optional_time_point_from_row(row, 4),
    };
}

mf::game::RewardGrantRecord row_to_reward_grant(const mysql::row_view& row) {
    return mf::game::RewardGrantRecord{
        .grant_id = column_string(row, 0),
        .player_id = column_string(row, 1),
        .source_type = column_string(row, 2),
        .source_id = column_string(row, 3),
        .rewards = decode_rewards_json(column_string(row, 4)),
        .granted_at = optional_time_point_from_row(row, 5),
    };
}

mf::game::RewardTemplate row_to_reward_template(const mysql::row_view& row) {
    return mf::game::RewardTemplate{
        .template_id = column_string(row, 0),
        .name = column_string(row, 1),
        .rewards = decode_rewards_json(column_string(row, 2)),
        .created_at = optional_time_point_from_row(row, 3),
        .updated_at = optional_time_point_from_row(row, 4),
    };
}

mf::game::AnnouncementEntry row_to_announcement(const mysql::row_view& row) {
    return mf::game::AnnouncementEntry{
        .announcement_id = column_string(row, 0),
        .channel =
            mf::game::ChatChannel{
                .channel_id = column_string(row, 1),
                // 公告表里只存 channel_kind 字符串，这里统一走聊天频道解析函数还原领域枚举。
                .kind = mf::chat::parse_channel_kind(column_string(row, 2)).value_or(mf::game::ChatChannelKind::lobby),
                .scope_id = column_string(row, 3),
                .display_name = column_string(row, 4),
            },
        .operator_id = column_string(row, 5),
        .title = column_string(row, 6),
        .body = column_string(row, 7),
        .published_at = optional_time_point_from_row(row, 8),
        .expires_at = optional_time_point_from_row(row, 9),
    };
}

mf::game::MailMessage row_to_mail_message(const mysql::row_view& row) {
    return mf::game::MailMessage{
        .mail_id = column_string(row, 0),
        .player_id = column_string(row, 1),
        .sender_name = column_string(row, 2),
        .title = column_string(row, 3),
        .body = column_string(row, 4),
        .rewards = decode_rewards_json(column_string(row, 5)),
        .read = column_i64(row, 6) > 0,
        .claimed = column_i64(row, 7) > 0,
        .deleted = column_i64(row, 8) > 0,
        .claim_grant_id = column_string(row, 9),
        .sent_at = optional_time_point_from_row(row, 10),
        .expires_at = optional_time_point_from_row(row, 11),
        .read_at = optional_time_point_from_row(row, 12),
        .claimed_at = optional_time_point_from_row(row, 13),
        .deleted_at = optional_time_point_from_row(row, 14),
    };
}

mf::game::AuditLogEntry row_to_audit_log(const mysql::row_view& row) {
    return mf::game::AuditLogEntry{
        .audit_id = column_string(row, 0),
        .operator_id = column_string(row, 1),
        .action = column_string(row, 2),
        .target_type = column_string(row, 3),
        .target_id = column_string(row, 4),
        .request_ip = column_string(row, 5),
        .user_agent = column_string(row, 6),
        .trace_id = column_string(row, 7),
        .success = column_i64(row, 8) != 0,
        .failure_reason = column_string(row, 9),
        .duration_ms = column_i64(row, 10),
        .before_json = column_optional_string(row, 11).value_or(std::string{}),
        .after_json = column_optional_string(row, 12).value_or(std::string{}),
        .detail_json = column_string(row, 13),
        .created_at = optional_time_point_from_row(row, 14),
    };
}

mf::game::AdminPrincipal row_to_admin_principal(const mysql::row_view& row, std::vector<std::string> permissions) {
    return mf::game::AdminPrincipal{
        .account_id = column_string(row, 0),
        .operator_name = column_string(row, 1),
        .active = column_i64(row, 2) > 0,
        .permissions = std::move(permissions),
        .created_at = optional_time_point_from_row(row, 3),
        .updated_at = optional_time_point_from_row(row, 4),
    };
}

std::string admin_task_kind_to_string(mf::game::AdminTaskKind kind) {
    switch (kind) {
    case mf::game::AdminTaskKind::announce:
        return "announce";
    case mf::game::AdminTaskKind::batch_reward:
        return "batch_reward";
    case mf::game::AdminTaskKind::batch_mail:
        return "batch_mail";
    case mf::game::AdminTaskKind::batch_mute:
        return "batch_mute";
    case mf::game::AdminTaskKind::batch_account_status:
        return "batch_account_status";
    }
    return "announce";
}

mf::game::AdminTaskKind parse_admin_task_kind(std::string_view value) {
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
    return mf::game::AdminTaskKind::announce;
}

mf::game::AdminTask row_to_admin_task(const mysql::row_view& row) {
    return mf::game::AdminTask{
        .task_id = column_string(row, 0),
        .kind = parse_admin_task_kind(column_string(row, 1)),
        .status = column_string(row, 2),
        .operator_id = column_string(row, 3),
        .target_type = column_string(row, 4),
        .target_id = column_string(row, 5),
        .request_ip = column_string(row, 6),
        .user_agent = column_string(row, 7),
        .trace_id = column_string(row, 8),
        .request_json = column_string(row, 9),
        .result_json = column_optional_string(row, 10).value_or(std::string{}),
        .failure_reason = column_string(row, 11),
        .execute_at = optional_time_point_from_row(row, 12),
        .executed_at = optional_time_point_from_row(row, 13),
        .created_at = optional_time_point_from_row(row, 14),
        .updated_at = optional_time_point_from_row(row, 15),
    };
}

class MySqlAgentRepository final : public mf::agent::AgentRepository {
public:
    explicit MySqlAgentRepository(std::shared_ptr<MySqlClient> client)
        : client_(std::move(client)) {}

    mf::agent::AgentConversation save_conversation(mf::agent::AgentConversation conversation) override {
        client_->with_connection(__func__, "agent.conversation_save", [&](mysql::any_connection& connection) {
            const auto statement = StatementCache::prepare(connection, 
                "INSERT INTO agent_conversation(conversation_id, player_id, scene_name, title, status_name) "
                "VALUES (?, ?, ?, ?, ?) ON DUPLICATE KEY UPDATE "
                "scene_name=VALUES(scene_name), title=VALUES(title), status_name=VALUES(status_name), updated_at=CURRENT_TIMESTAMP");
            mysql::results result;
            connection.execute(statement.bind(
                conversation.conversation_id,
                conversation.player_id,
                conversation.scene,
                conversation.title,
                conversation.status), result);
        });
        return find_conversation(conversation.conversation_id, conversation.player_id).value_or(std::move(conversation));
    }

    std::optional<mf::agent::AgentConversation> find_conversation(
        const std::string& conversation_id,
        const std::string& player_id) const override {
        std::optional<mf::agent::AgentConversation> conversation;
        client_->with_connection(__func__, "agent.conversation_find", [&](mysql::any_connection& connection) {
            const auto statement = StatementCache::prepare(connection, 
                "SELECT conversation_id, player_id, scene_name, title, status_name, "
                "UNIX_TIMESTAMP(created_at), UNIX_TIMESTAMP(updated_at) "
                "FROM agent_conversation WHERE conversation_id=? AND player_id=? AND status_name='active'");
            mysql::results result;
            connection.execute(statement.bind(conversation_id, player_id), result);
            if (!result.rows().empty()) conversation = row_to_conversation(result.rows().at(0));
        });
        return conversation;
    }

    bool delete_conversation(const std::string& conversation_id, const std::string& player_id) override {
        bool deleted = false;
        client_->with_connection(__func__, "agent.conversation_delete", [&](mysql::any_connection& connection) {
            const auto statement = StatementCache::prepare(connection, 
                "UPDATE agent_conversation SET status_name='deleted', updated_at=CURRENT_TIMESTAMP "
                "WHERE conversation_id=? AND player_id=? AND status_name<>'deleted'");
            mysql::results result;
            connection.execute(statement.bind(conversation_id, player_id), result);
            deleted = result.affected_rows() > 0;
        });
        return deleted;
    }

    mf::agent::AgentMessage append_message(mf::agent::AgentMessage message) override {
        client_->with_connection(__func__, "agent.message_append", [&](mysql::any_connection& connection) {
            const auto statement = StatementCache::prepare(connection, 
                "INSERT INTO agent_message(message_id, conversation_id, player_id, client_request_id, role_name, "
                "content_text, model_name, prompt_tokens, completion_tokens) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)");
            mysql::results result;
            connection.execute(statement.bind(
                message.message_id,
                message.conversation_id,
                message.player_id,
                message.client_request_id,
                message.role,
                message.content,
                message.model,
                message.prompt_tokens,
                message.completion_tokens), result);
        });
        return find_message(message.message_id).value_or(std::move(message));
    }

    std::vector<mf::agent::AgentMessage> list_messages(
        const std::string& conversation_id,
        const std::string& player_id,
        std::size_t limit) const override {
        std::vector<mf::agent::AgentMessage> messages;
        client_->with_connection(__func__, "agent.message_list", [&](mysql::any_connection& connection) {
            const auto statement = StatementCache::prepare(connection, 
                "SELECT message_id, conversation_id, player_id, client_request_id, role_name, content_text, "
                "model_name, prompt_tokens, completion_tokens, UNIX_TIMESTAMP(created_at) "
                "FROM agent_message WHERE conversation_id=? AND player_id=? ORDER BY created_at DESC, message_id DESC LIMIT ?");
            mysql::results result;
            connection.execute(statement.bind(conversation_id, player_id, static_cast<std::uint64_t>(limit)), result);
            for (const auto row : result.rows()) messages.push_back(row_to_message(row));
        });
        std::reverse(messages.begin(), messages.end());
        return messages;
    }

    std::optional<mf::agent::AgentMessage> find_assistant_message_by_request(
        const std::string& player_id,
        const std::string& client_request_id) const override {
        std::optional<mf::agent::AgentMessage> message;
        client_->with_connection(__func__, "agent.message_find_request", [&](mysql::any_connection& connection) {
            const auto statement = StatementCache::prepare(connection, 
                "SELECT m.message_id, m.conversation_id, m.player_id, m.client_request_id, m.role_name, m.content_text, "
                "m.model_name, m.prompt_tokens, m.completion_tokens, UNIX_TIMESTAMP(m.created_at) "
                "FROM agent_message m INNER JOIN agent_conversation c ON c.conversation_id=m.conversation_id "
                "WHERE m.player_id=? AND m.client_request_id=? AND m.role_name='assistant' AND c.status_name='active' "
                "ORDER BY m.created_at DESC LIMIT 1");
            mysql::results result;
            connection.execute(statement.bind(player_id, client_request_id), result);
            if (!result.rows().empty()) message = row_to_message(result.rows().at(0));
        });
        return message;
    }

    mf::agent::AgentToolCall save_tool_call(mf::agent::AgentToolCall call) override {
        client_->with_connection(__func__, "agent.tool_save", [&](mysql::any_connection& connection) {
            const auto statement = StatementCache::prepare(connection, 
                "INSERT INTO agent_tool_call(call_id, conversation_id, message_id, player_id, tool_name, arguments_json, "
                "risk_level, status_name, display_text, result_json, error_code, idempotency_key, confirmed_at, finished_at) "
                "VALUES (?, ?, ?, ?, ?, CAST(? AS JSON), ?, ?, ?, CAST(? AS JSON), ?, ?, "
                "IF(?=0,NULL,FROM_UNIXTIME(?)), IF(?=0,NULL,FROM_UNIXTIME(?))) "
                "ON DUPLICATE KEY UPDATE status_name=VALUES(status_name), display_text=VALUES(display_text), "
                "result_json=VALUES(result_json), error_code=VALUES(error_code), confirmed_at=VALUES(confirmed_at), "
                "finished_at=VALUES(finished_at)");
            const auto confirmed = call.confirmed_at ? to_unix_seconds(*call.confirmed_at) : 0;
            const auto finished = call.finished_at ? to_unix_seconds(*call.finished_at) : 0;
            const auto result_json = call.result_json.empty() ? "{}" : call.result_json;
            mysql::results result;
            connection.execute(statement.bind(
                call.call_id,
                call.conversation_id,
                call.message_id,
                call.player_id,
                call.tool_name,
                call.arguments_json.empty() ? "{}" : call.arguments_json,
                call.risk_level,
                call.status,
                call.display_text,
                result_json,
                call.error_code,
                call.idempotency_key,
                confirmed,
                confirmed,
                finished,
                finished), result);
        });
        return find_tool_call(call.call_id, call.conversation_id, call.player_id).value_or(std::move(call));
    }

    std::optional<mf::agent::AgentToolCall> find_tool_call(
        const std::string& call_id,
        const std::string& conversation_id,
        const std::string& player_id) const override {
        std::optional<mf::agent::AgentToolCall> call;
        client_->with_connection(__func__, "agent.tool_find", [&](mysql::any_connection& connection) {
            const auto statement = StatementCache::prepare(connection, 
                "SELECT t.call_id, t.conversation_id, t.message_id, t.player_id, t.tool_name, CAST(t.arguments_json AS CHAR), "
                "t.risk_level, t.status_name, t.display_text, CAST(t.result_json AS CHAR), t.error_code, t.idempotency_key, "
                "UNIX_TIMESTAMP(t.created_at), UNIX_TIMESTAMP(t.confirmed_at), UNIX_TIMESTAMP(t.finished_at) "
                "FROM agent_tool_call t INNER JOIN agent_conversation c ON c.conversation_id=t.conversation_id "
                "WHERE t.call_id=? AND t.conversation_id=? AND t.player_id=? AND c.status_name='active'");
            mysql::results result;
            connection.execute(statement.bind(call_id, conversation_id, player_id), result);
            if (!result.rows().empty()) call = row_to_tool_call(result.rows().at(0));
        });
        return call;
    }

    std::vector<mf::agent::AgentToolCall> list_tool_calls_for_message(const std::string& message_id) const override {
        std::vector<mf::agent::AgentToolCall> calls;
        client_->with_connection(__func__, "agent.tool_list_message", [&](mysql::any_connection& connection) {
            const auto statement = StatementCache::prepare(connection, 
                "SELECT call_id, conversation_id, message_id, player_id, tool_name, CAST(arguments_json AS CHAR), "
                "risk_level, status_name, display_text, CAST(result_json AS CHAR), error_code, idempotency_key, "
                "UNIX_TIMESTAMP(created_at), UNIX_TIMESTAMP(confirmed_at), UNIX_TIMESTAMP(finished_at) "
                "FROM agent_tool_call WHERE message_id=? ORDER BY created_at, call_id");
            mysql::results result;
            connection.execute(statement.bind(message_id), result);
            for (const auto row : result.rows()) calls.push_back(row_to_tool_call(row));
        });
        return calls;
    }

    mf::agent::AgentUsageDaily get_daily_usage(
        const std::string& player_id,
        const std::string& usage_date) const override {
        mf::agent::AgentUsageDaily usage{.player_id = player_id, .usage_date = usage_date};
        client_->with_connection(__func__, "agent.usage_get", [&](mysql::any_connection& connection) {
            const auto statement = StatementCache::prepare(connection, 
                "SELECT player_id, DATE_FORMAT(usage_date, '%Y-%m-%d'), request_count, prompt_tokens, "
                "completion_tokens, failed_count FROM agent_usage_daily WHERE player_id=? AND usage_date=?");
            mysql::results result;
            connection.execute(statement.bind(player_id, usage_date), result);
            if (!result.rows().empty()) usage = row_to_usage(result.rows().at(0));
        });
        return usage;
    }

    mf::agent::AgentUsageDaily record_usage(
        const std::string& player_id,
        const std::string& usage_date,
        std::int64_t prompt_tokens,
        std::int64_t completion_tokens,
        bool failed) override {
        client_->with_connection(__func__, "agent.usage_record", [&](mysql::any_connection& connection) {
            const auto statement = StatementCache::prepare(connection, 
                "INSERT INTO agent_usage_daily(player_id, usage_date, request_count, prompt_tokens, completion_tokens, failed_count) "
                "VALUES (?, ?, 1, ?, ?, ?) ON DUPLICATE KEY UPDATE request_count=request_count+1, "
                "prompt_tokens=prompt_tokens+VALUES(prompt_tokens), completion_tokens=completion_tokens+VALUES(completion_tokens), "
                "failed_count=failed_count+VALUES(failed_count)");
            mysql::results result;
            connection.execute(statement.bind(player_id, usage_date, prompt_tokens, completion_tokens, failed ? 1 : 0), result);
        });
        return get_daily_usage(player_id, usage_date);
    }

private:
    std::optional<mf::agent::AgentMessage> find_message(const std::string& message_id) const {
        std::optional<mf::agent::AgentMessage> message;
        client_->with_connection(__func__, "agent.message_find", [&](mysql::any_connection& connection) {
            const auto statement = StatementCache::prepare(connection, 
                "SELECT message_id, conversation_id, player_id, client_request_id, role_name, content_text, "
                "model_name, prompt_tokens, completion_tokens, UNIX_TIMESTAMP(created_at) "
                "FROM agent_message WHERE message_id=?");
            mysql::results result;
            connection.execute(statement.bind(message_id), result);
            if (!result.rows().empty()) message = row_to_message(result.rows().at(0));
        });
        return message;
    }

    static mf::agent::AgentConversation row_to_conversation(const mysql::row_view& row) {
        return {
            .conversation_id = column_string(row, 0), .player_id = column_string(row, 1),
            .scene = column_string(row, 2), .title = column_string(row, 3), .status = column_string(row, 4),
            .created_at = optional_time_point_from_row(row, 5), .updated_at = optional_time_point_from_row(row, 6),
        };
    }

    static mf::agent::AgentMessage row_to_message(const mysql::row_view& row) {
        return {
            .message_id = column_string(row, 0), .conversation_id = column_string(row, 1),
            .player_id = column_string(row, 2), .client_request_id = column_string(row, 3),
            .role = column_string(row, 4), .content = column_string(row, 5), .model = column_string(row, 6),
            .prompt_tokens = column_i64(row, 7), .completion_tokens = column_i64(row, 8),
            .created_at = optional_time_point_from_row(row, 9),
        };
    }

    static mf::agent::AgentToolCall row_to_tool_call(const mysql::row_view& row) {
        return {
            .call_id = column_string(row, 0), .conversation_id = column_string(row, 1),
            .message_id = column_string(row, 2), .player_id = column_string(row, 3),
            .tool_name = column_string(row, 4), .arguments_json = column_string(row, 5),
            .risk_level = column_string(row, 6), .status = column_string(row, 7),
            .display_text = column_string(row, 8), .result_json = column_string(row, 9),
            .error_code = column_string(row, 10), .idempotency_key = column_string(row, 11),
            .created_at = optional_time_point_from_row(row, 12),
            .confirmed_at = optional_time_point_from_row(row, 13),
            .finished_at = optional_time_point_from_row(row, 14),
        };
    }

    static mf::agent::AgentUsageDaily row_to_usage(const mysql::row_view& row) {
        return {
            .player_id = column_string(row, 0), .usage_date = column_string(row, 1),
            .request_count = column_i64(row, 2), .prompt_tokens = column_i64(row, 3),
            .completion_tokens = column_i64(row, 4), .failed_count = column_i64(row, 5),
        };
    }

    std::shared_ptr<MySqlClient> client_;
};

class MySqlAccountRepository final : public mf::game::AccountRepository {
public:
    explicit MySqlAccountRepository(std::shared_ptr<MySqlClient> client)
        : client_(std::move(client)) {}

    std::optional<mf::game::Account> upsert_quick_account(
        std::string login_name,
        std::string display_name) override {
        std::optional<mf::game::Account> existing;

        client_->with_connection(__func__, "account.find_by_login", [&](mysql::any_connection& connection) {
            mysql::results result;
            connection.execute(statements_.account_find_by_login(connection).bind(login_name), result);
            if (!result.rows().empty()) {
                existing = row_to_account(result.rows().at(0));
                return;
            }

            const auto account_id = make_stable_id("acc_", login_name);
            mysql::results insert_result;
            connection.execute(
                statements_.account_quick_insert(connection).bind(account_id, login_name, display_name),
                insert_result);
            existing = mf::game::Account{
                .account_id = account_id,
                .login_name = login_name,
                .display_name = display_name,
                .auth_mode = mf::game::AccountAuthMode::quick,
            };
        });

        if (!existing) {
            return std::nullopt;
        }

        if (existing->auth_mode != mf::game::AccountAuthMode::quick) {
            return std::nullopt;
        }

        client_->with_connection(__func__, "account.quick_update_display_name", [&](mysql::any_connection& connection) {
            mysql::results result;
            connection.execute(
                statements_.account_quick_update_display_name(connection).bind(display_name, login_name),
                result);
        });

        existing->display_name = std::move(display_name);
        return existing;
    }

    std::optional<mf::game::Account> create_password_account(
        std::string login_name,
        std::string display_name,
        std::string email,
        std::string phone_number,
        std::string platform_channel,
        std::string register_source,
        std::string password_hash,
        std::string password_scheme) override {
        const auto account_id = make_stable_id("acc_", login_name);
        bool inserted = false;
        client_->with_connection(__func__, "account.password_insert", [&](mysql::any_connection& connection) {
            mysql::results result;
            connection.execute(
                statements_.account_password_insert(connection).bind(
                    account_id,
                    login_name,
                    display_name,
                    email,
                    phone_number,
                    platform_channel,
                    register_source,
                    password_hash,
                    password_scheme),
                result);
            if (result.affected_rows() == 0) {
                // login_name 的唯一索引负责并发判重，避免注册前再做一次 SELECT。
                return;
            }
            inserted = true;
        });

        // INSERT IGNORE 遇到重复登录名时不会抛异常，而是 affected_rows=0。
        if (!inserted) {
            return std::nullopt;
        }

        return mf::game::Account{
            .account_id = account_id,
            .login_name = std::move(login_name),
            .display_name = std::move(display_name),
            .email = std::move(email),
            .phone_number = std::move(phone_number),
            .platform_channel = std::move(platform_channel),
            .register_source = std::move(register_source),
            .auth_mode = mf::game::AccountAuthMode::password,
            .status = mf::game::AccountStatus::active,
        };
    }

    std::optional<mf::game::PasswordAccountRecord> find_password_account_by_login(
        const std::string& login_name) const override {
        std::optional<mf::game::PasswordAccountRecord> account;

        client_->with_connection(__func__, "account.find_password_by_login", [&](mysql::any_connection& connection) {
            mysql::results result;
            connection.execute(
                statements_.account_find_password_by_login(connection).bind(login_name),
                result);
            if (result.rows().empty()) {
                return;
            }

            const auto row = result.rows().at(0);
            account = mf::game::PasswordAccountRecord{
                .account = row_to_account(row),
                .password_hash = column_string(row, 16),
                .password_scheme = column_string(row, 17),
                .reset_token = column_optional_string(row, 18).value_or(std::string{}),
                .reset_token_expires_at = optional_time_point_from_row(row, 19),
                .player_id = column_optional_string(row, 20),
            };
        });

        return account;
    }

    std::optional<mf::game::Account> find_by_login_name(const std::string& login_name) const override {
        return find_account_by_login(login_name);
    }

    std::optional<mf::game::Account> update_password_credentials(
        const std::string& account_id,
        std::string password_hash,
        std::string password_scheme) override {
        client_->with_connection(__func__, "account.update_password", [&](mysql::any_connection& connection) {
            mysql::results result;
            connection.execute(
                statements_.account_update_password(connection).bind(
                    password_hash,
                    password_scheme,
                    account_id),
                result);
        });
        return find_by_account_id(account_id);
    }

    std::optional<mf::game::PasswordResetTicket> create_password_reset_ticket(
        const std::string& login_name,
        const std::string& email,
        std::string reset_token,
        std::chrono::system_clock::time_point expires_at) override {
        client_->with_connection(__func__, "account.issue_reset_token", [&](mysql::any_connection& connection) {
            mysql::results result;
            connection.execute(
                statements_.account_issue_reset_token(connection).bind(
                    reset_token,
                    to_unix_seconds(expires_at),
                    login_name,
                    email,
                    email),
                result);
        });

        const auto account = find_password_account_by_login(login_name);
        if (!account || account->reset_token != reset_token) {
            return std::nullopt;
        }

        return mf::game::PasswordResetTicket{
            .account_id = account->account.account_id,
            .login_name = account->account.login_name,
            .reset_token = std::move(reset_token),
            .expires_at = expires_at,
        };
    }

    std::optional<mf::game::Account> reset_password_with_ticket(
        const std::string& reset_token,
        std::string password_hash,
        std::string password_scheme) override {
        const auto record = find_password_account_by_reset_token(reset_token);
        if (!record) {
            return std::nullopt;
        }
        client_->with_connection(__func__, "account.update_password", [&](mysql::any_connection& connection) {
            mysql::results result;
            connection.execute(
                statements_.account_update_password(connection).bind(
                    password_hash,
                    password_scheme,
                    record->account.account_id),
                result);
        });
        return find_by_account_id(record->account.account_id);
    }

    std::optional<mf::game::Account> set_account_status(
        const std::string& account_id,
        mf::game::AccountStatus status,
        std::string reason) override {
        client_->with_connection(__func__, "account.set_status", [&](mysql::any_connection& connection) {
            mysql::results result;
            connection.execute(
                statements_.account_set_status(connection).bind(
                    std::string(to_status_text(status)),
                    reason,
                    account_id),
                result);
        });
        return find_by_account_id(account_id);
    }

    std::optional<mf::game::Account> record_login_success(
        const std::string& account_id,
        const std::string& client_ip) override {
        client_->with_connection(__func__, "account.record_login_success", [&](mysql::any_connection& connection) {
            mysql::results result;
            connection.execute(
                statements_.account_record_login_success(connection).bind(client_ip, account_id),
                result);
        });
        return find_by_account_id(account_id);
    }

    // 登录成功只更新审计字段；调用方已经持有完整账号记录，
    // 因此这里不再额外 SELECT，减少一次 MySQL 往返和连接占用时间。
    void record_login_success_without_fetch(
        const std::string& account_id,
        const std::string& client_ip) override {
        client_->with_connection(
            __func__,
            "account.record_login_success_without_fetch",
            [&](mysql::any_connection& connection) {
                mysql::results result;
                connection.execute(
                    statements_.account_record_login_success(connection).bind(client_ip, account_id),
                    result);
            });
    }

    std::optional<mf::game::Account> record_login_failure(
        const std::string& account_id,
        std::int64_t lock_threshold,
        std::chrono::seconds lock_duration) override {
        const auto locked_until = std::chrono::system_clock::now() + lock_duration;
        client_->with_connection(__func__, "account.record_login_failure", [&](mysql::any_connection& connection) {
            mysql::results result;
            connection.execute(
                statements_.account_record_login_failure(connection).bind(
                    lock_threshold,
                    to_unix_seconds(locked_until),
                    account_id),
                result);
        });
        return find_by_account_id(account_id);
    }

    std::optional<mf::game::Account> find_by_account_id(const std::string& account_id) const override {
        std::optional<mf::game::Account> account;

        client_->with_connection(__func__, "account.find_by_id", [&](mysql::any_connection& connection) {
            mysql::results result;
            connection.execute(statements_.account_find_by_id(connection).bind(account_id), result);
            if (result.rows().empty()) {
                return;
            }
            account = row_to_account(result.rows().at(0));
        });

        return account;
    }

private:
    static std::string_view to_status_text(mf::game::AccountStatus status) {
        switch (status) {
        case mf::game::AccountStatus::active:
            return "active";
        case mf::game::AccountStatus::frozen:
            return "frozen";
        case mf::game::AccountStatus::banned:
            return "banned";
        }
        return "active";
    }

    std::optional<mf::game::Account> find_account_by_login(const std::string& login_name) const {
        std::optional<mf::game::Account> account;

        client_->with_connection(__func__, "account.find_by_login", [&](mysql::any_connection& connection) {
            mysql::results result;
            connection.execute(statements_.account_find_by_login(connection).bind(login_name), result);
            if (result.rows().empty()) {
                return;
            }
            account = row_to_account(result.rows().at(0));
        });

        return account;
    }

    std::optional<mf::game::PasswordAccountRecord> find_password_account_by_reset_token(
        const std::string& reset_token) const {
        std::optional<mf::game::PasswordAccountRecord> account;

        client_->with_connection(__func__, "account.find_by_reset_token", [&](mysql::any_connection& connection) {
            mysql::results result;
            connection.execute(
                statements_.account_find_by_reset_token(connection).bind(reset_token),
                result);
            if (result.rows().empty()) {
                return;
            }

            const auto row = result.rows().at(0);
            account = mf::game::PasswordAccountRecord{
                .account = row_to_account(row),
                .password_hash = column_string(row, 16),
                .password_scheme = column_string(row, 17),
                .reset_token = column_optional_string(row, 18).value_or(std::string{}),
                .reset_token_expires_at = optional_time_point_from_row(row, 19),
            };
        });

        return account;
    }

    std::shared_ptr<MySqlClient> client_;
    mutable StatementCache statements_;
};

class MySqlPlayerRepository final : public mf::game::PlayerRepository {
public:
    explicit MySqlPlayerRepository(std::shared_ptr<MySqlClient> client)
        : client_(std::move(client)) {}

    mf::game::PlayerProfile create_for_registration(const mf::game::Account& account) override {
        const auto player_id = allocate_player_id(account.account_id);
        const auto inventory = default_inventory();
        client_->with_connection(__func__, "player.create_for_registration", [&](mysql::any_connection& connection) {
            mysql::results result;
            connection.execute(
                statements_.player_upsert(connection).bind(
                    player_id, account.account_id, account.display_name, 1000, 0),
                result);
            for (const auto& item : inventory) {
                mysql::results item_result;
                connection.execute(
                    statements_.player_inventory_insert_ignore(connection).bind(
                        player_id, item.item_id, item.instance_id, item.quantity),
                    item_result);
            }
        });

        // 注册路径已经知道是新角色，直接返回默认资料，避免再查主表、货币表和背包表。
        return mf::game::PlayerProfile{
            .player_id = player_id,
            .account_id = account.account_id,
            .nickname = account.display_name,
            .gold = 1000,
            .gem = 0,
            .currencies = {
                mf::game::CurrencyBalance{.currency_id = "gold", .amount = 1000},
                mf::game::CurrencyBalance{.currency_id = "gem", .amount = 0},
            },
            .inventory = inventory,
        };
    }

    mf::game::PlayerProfile upsert_for_login(const mf::game::Account& account) override {
        {
            std::scoped_lock lock{login_identity_cache_mutex_};
            const auto it = login_identity_cache_.find(account.account_id);
            if (it != login_identity_cache_.end() &&
                it->second.expires_at > std::chrono::steady_clock::now() &&
                it->second.profile.nickname == account.display_name) {
                return it->second.profile;
            }
            if (it != login_identity_cache_.end()) {
                login_identity_cache_.erase(it);
            }
        }

        // 进程冷启动时只合并同一账号的回源请求。
        // 分片锁避免不同账号因为共享一把全局锁而串行访问 MySQL。
        const auto lock_shard = std::hash<std::string>{}(account.account_id) %
                                k_login_identity_lock_shards;
        std::scoped_lock fill_lock{login_identity_fill_mutexes_[lock_shard]};
        {
            std::scoped_lock lock{login_identity_cache_mutex_};
            const auto it = login_identity_cache_.find(account.account_id);
            if (it != login_identity_cache_.end() &&
                it->second.expires_at > std::chrono::steady_clock::now() &&
                it->second.profile.nickname == account.display_name) {
                return it->second.profile;
            }
        }

        std::optional<mf::game::PlayerProfile> profile;
        client_->with_connection(__func__, "player.upsert_for_login", [&](mysql::any_connection& connection) {
            mysql::results result;
            connection.execute(
                statements_.player_find_by_account_id(connection).bind(account.account_id),
                result);
            if (result.rows().empty()) {
                return;
            }

            const auto row = result.rows().at(0);
            const auto player_id = column_string(row, 0);
            const auto gold = column_i64(row, 3);
            const auto gem = column_i64(row, 4);
            if (column_string(row, 2) != account.display_name) {
                mysql::results update_result;
                connection.execute(
                    statements_.player_upsert(connection).bind(
                        player_id,
                        account.account_id,
                        account.display_name,
                        gold,
                        gem),
                    update_result);
            }
            profile = mf::game::PlayerProfile{
                .player_id = player_id,
                .account_id = account.account_id,
                .nickname = account.display_name,
                .gold = gold,
                .gem = gem,
                .currencies = {
                    mf::game::CurrencyBalance{.currency_id = "gold", .amount = gold},
                    mf::game::CurrencyBalance{.currency_id = "gem", .amount = gem},
                },
            };
        });

        // 新建角色只在首次登录触发，仍通过完整初始化逻辑创建默认资产和背包。
        if (profile) {
            {
                std::scoped_lock lock{login_identity_cache_mutex_};
                login_identity_cache_[account.account_id] = LoginIdentityCacheEntry{
                    .profile = *profile,
                    .expires_at = std::chrono::steady_clock::now() + std::chrono::seconds{30},
                };
            }
            return *profile;
        }
        auto created = upsert_for_account(account);
        {
            std::scoped_lock lock{login_identity_cache_mutex_};
            login_identity_cache_[account.account_id] = LoginIdentityCacheEntry{
                .profile = created,
                .expires_at = std::chrono::steady_clock::now() + std::chrono::seconds{30},
            };
        }
        return created;
    }

    mf::game::PlayerProfile upsert_for_account(const mf::game::Account& account) override {
        // 一次聚合查询同时拿到主资料、货币和背包，避免原来的“主表查询 +
        // 货币查询 + 背包查询”多次占用连接。登录高峰中资料未变化时直接返回，
        // 不再执行无意义的 UPDATE。
        const std::optional<mf::game::PlayerProfile> existing_by_account = find_by_account_id(account.account_id);
        if (existing_by_account && existing_by_account->nickname == account.display_name) {
            return *existing_by_account;
        }

        // 先按 account_id 查旧档，避免历史账号在改规则后被重复建出新角色。
        const auto player_id = existing_by_account ? existing_by_account->player_id : allocate_player_id(account.account_id);
        const std::optional<mf::game::PlayerProfile> existing =
            existing_by_account ? existing_by_account : find_by_player_id(player_id);

        // 这里保持和内存实现同样的契约：
        // 第一次建号给默认金币和默认背包，之后重复登录只同步昵称。
        const auto gold = existing ? existing->gold : 1000;
        const auto gem = existing ? existing->gem : 0;
        const auto currencies =
            existing ? existing->currencies
                     : std::vector<mf::game::CurrencyBalance>{
                           mf::game::CurrencyBalance{.currency_id = "gold", .amount = gold},
                           mf::game::CurrencyBalance{.currency_id = "gem", .amount = gem},
                       };
        const auto inventory = existing ? existing->inventory : default_inventory();

        client_->with_connection(__func__, "player.upsert", [&](mysql::any_connection& connection) {
            mysql::results result;
            connection.execute(
                statements_.player_upsert(connection).bind(
                    player_id,
                    account.account_id,
                    account.display_name,
                    gold,
                    gem),
                result);

            if (!existing) {
                for (const auto& item : inventory) {
                    mysql::results seed_result;
                    connection.execute(
                        statements_.player_inventory_insert_ignore(connection).bind(
                            player_id, item.item_id, item.instance_id, item.quantity),
                        seed_result);
                }
            }
        });

        return mf::game::PlayerProfile{
            .player_id = player_id,
            .account_id = account.account_id,
            .nickname = account.display_name,
            .gold = gold,
            .gem = gem,
            .currencies = currencies,
            .inventory = inventory,
        };
    }

    std::optional<mf::game::PlayerProfile> find_by_player_id(const std::string& player_id) const override {
        std::optional<mf::game::PlayerProfile> profile;

        client_->with_connection(__func__, "player.find_by_id", [&](mysql::any_connection& connection) {
            profile = load_player_profile_single_query(connection, "player_id", player_id);
        });

        return profile;
    }

    std::vector<mf::game::PlayerSummary> find_summaries_by_player_ids(
        const std::vector<std::string>& player_ids) const override {
        std::vector<mf::game::PlayerSummary> summaries;
        if (player_ids.empty()) {
            return summaries;
        }

        boost::json::array wanted_ids;
        wanted_ids.reserve(player_ids.size());
        for (const auto& player_id : player_ids) {
            wanted_ids.emplace_back(player_id);
        }
        const auto wanted_ids_json = boost::json::serialize(wanted_ids);

        client_->with_connection(__func__, "player.find_summaries_by_ids", [&](mysql::any_connection& connection) {
            mysql::results result;
            const auto statement = StatementCache::prepare(
                connection,
                "SELECT p.player_id, p.account_id, p.nickname "
                "FROM player_profile p "
                "JOIN JSON_TABLE(?, '$[*]' COLUMNS(player_id VARCHAR(64) PATH '$')) AS ids "
                "ON ids.player_id = p.player_id COLLATE utf8mb4_0900_ai_ci");
            connection.execute(statement.bind(wanted_ids_json), result);
            summaries.reserve(result.rows().size());
            for (const auto row : result.rows()) {
                summaries.push_back(mf::game::PlayerSummary{
                    .player_id = column_string(row, 0),
                    .account_id = column_string(row, 1),
                    .nickname = column_string(row, 2),
                });
            }
        });

        return summaries;
    }

    std::vector<mf::game::PlayerProfile> list_all() const override {
        std::vector<mf::game::PlayerProfile> players;

        client_->with_connection(__func__, "player.list_all", [&](mysql::any_connection& connection) {
            mysql::results result;
            // 这里直接走原始 SQL，
            // 避免无参 statement 在某些执行路径下再次 bind() 导致后台玩家列表 500。
            connection.execute(
                "SELECT player_id, account_id, nickname, gold, gem "
                "FROM player_profile ORDER BY created_at ASC",
                result);
            if (result.rows().empty()) {
                return;
            }

            std::vector<std::string> player_ids;
            player_ids.reserve(result.rows().size());
            for (const auto row : result.rows()) {
                player_ids.push_back(column_string(row, 0));
            }
            const auto ids_json = encode_player_id_json_array(player_ids);
            const auto currencies_by_player = load_currencies_batch(connection, ids_json);
            const auto inventory_by_player = load_inventory_batch(connection, ids_json);

            players.reserve(result.rows().size());
            for (const auto row : result.rows()) {
                players.push_back(profile_from_row_and_maps(row, currencies_by_player, inventory_by_player));
            }
        });

        return players;
    }

    std::size_t count_filtered(
        const std::optional<std::string>& player_id_filter,
        const std::optional<std::string>& account_id_filter,
        const std::optional<std::string>& nickname_filter) const override {
        std::size_t total = 0;
        client_->with_connection(__func__, "player.count_filtered", [&](mysql::any_connection& connection) {
            mysql::results result;
            connection.execute(
                statements_.player_count_filtered(connection).bind(
                    player_id_filter.value_or(std::string{}),
                    player_id_filter.value_or(std::string{}),
                    account_id_filter.value_or(std::string{}),
                    account_id_filter.value_or(std::string{}),
                    nickname_filter.value_or(std::string{}),
                    nickname_filter.value_or(std::string{})),
                result);
            if (!result.rows().empty()) {
                total = static_cast<std::size_t>(column_i64(result.rows().at(0), 0));
            }
        });
        return total;
    }

    std::vector<mf::game::PlayerProfile> list_filtered(
        std::size_t limit,
        std::size_t offset,
        const std::optional<std::string>& player_id_filter,
        const std::optional<std::string>& account_id_filter,
        const std::optional<std::string>& nickname_filter) const override {
        std::vector<mf::game::PlayerProfile> players;
        client_->with_connection(__func__, "player.list_filtered", [&](mysql::any_connection& connection) {
            mysql::results result;
            connection.execute(
                statements_.player_list_filtered(connection).bind(
                    player_id_filter.value_or(std::string{}),
                    player_id_filter.value_or(std::string{}),
                    account_id_filter.value_or(std::string{}),
                    account_id_filter.value_or(std::string{}),
                    nickname_filter.value_or(std::string{}),
                    nickname_filter.value_or(std::string{}),
                    static_cast<std::uint64_t>(limit),
                    static_cast<std::uint64_t>(offset)),
                result);
            if (result.rows().empty()) {
                return;
            }

            std::vector<std::string> player_ids;
            player_ids.reserve(result.rows().size());
            for (const auto row : result.rows()) {
                player_ids.push_back(column_string(row, 0));
            }
            const auto ids_json = encode_player_id_json_array(player_ids);
            const auto currencies_by_player = load_currencies_batch(connection, ids_json);
            const auto inventory_by_player = load_inventory_batch(connection, ids_json);

            players.reserve(result.rows().size());
            for (const auto row : result.rows()) {
                players.push_back(profile_from_row_and_maps(row, currencies_by_player, inventory_by_player));
            }
        });
        return players;
    }

    mf::game::PlayerAdminPage list_admin_rows(
        std::size_t limit,
        const std::optional<mf::game::PlayerPageCursor>& cursor,
        const std::optional<std::string>& player_id_filter,
        const std::optional<std::string>& account_id_filter,
        const std::optional<std::string>& nickname_filter) const override {
        mf::game::PlayerAdminPage page;
        const auto bounded_limit = std::clamp<std::size_t>(limit, 1, 500);
        const auto fetch_limit = bounded_limit + 1;

        client_->with_connection(__func__, "player.admin_list_filtered", [&](mysql::any_connection& connection) {
            mysql::results result;
            if (cursor) {
                connection.execute(
                    statements_.player_admin_list_filtered_cursor(connection).bind(
                        player_id_filter.value_or(std::string{}),
                        player_id_filter.value_or(std::string{}),
                        account_id_filter.value_or(std::string{}),
                        account_id_filter.value_or(std::string{}),
                        nickname_filter.value_or(std::string{}),
                        nickname_filter.value_or(std::string{}),
                        cursor->last_created_at,
                        cursor->last_created_at,
                        cursor->last_player_id,
                        static_cast<std::uint64_t>(fetch_limit)),
                    result);
            } else {
                connection.execute(
                    statements_.player_admin_list_filtered(connection).bind(
                        player_id_filter.value_or(std::string{}),
                        player_id_filter.value_or(std::string{}),
                        account_id_filter.value_or(std::string{}),
                        account_id_filter.value_or(std::string{}),
                        nickname_filter.value_or(std::string{}),
                        nickname_filter.value_or(std::string{}),
                        static_cast<std::uint64_t>(fetch_limit)),
                    result);
            }
            page.rows.reserve(result.rows().size());
            for (const auto row : result.rows()) {
                page.rows.push_back(row_to_player_admin_row(row));
            }
            if (page.rows.size() > bounded_limit) {
                page.has_more = true;
                const auto& cursor_row = result.rows().at(bounded_limit - 1);
                page.next_created_at = column_i64(cursor_row, 10);
                page.next_player_id = column_string(cursor_row, 0);
                page.rows.resize(bounded_limit);
            }
        });
        return page;
    }

    std::optional<mf::game::PlayerProfile> apply_rewards(
        const std::string& player_id,
        const std::vector<mf::game::RewardEntry>& rewards,
        const std::string&) override {
        std::optional<mf::game::PlayerProfile> updated;
        client_->with_connection(__func__, "player.apply_rewards", [&](mysql::any_connection& connection) {
            mysql::results result;
            connection.execute("START TRANSACTION", result);
            try {
                updated = apply_rewards_in_transaction(connection, player_id, rewards);
                if (!updated) {
                    connection.execute("ROLLBACK", result);
                    return;
                }
                connection.execute("COMMIT", result);
            } catch (...) {
                connection.execute("ROLLBACK", result);
                throw;
            }
        });
        return updated;
    }

    std::optional<mf::game::PlayerProfile> set_inventory_item_quantity(
        const std::string& player_id,
        const std::string& item_id,
        std::int64_t quantity) override {
        std::optional<mf::game::PlayerProfile> updated;
        client_->with_connection(__func__, "player.set_inventory_item_quantity", [&](mysql::any_connection& connection) {
            mysql::results find_result;
            connection.execute(statements_.player_find_by_id(connection).bind(player_id), find_result);
            if (find_result.rows().empty()) {
                return;
            }

            mysql::results write_result;
            if (quantity <= 0) {
                connection.execute(
                    statements_.player_inventory_delete(connection).bind(player_id, item_id),
                    write_result);
            } else {
                connection.execute(
                    statements_.player_inventory_upsert(connection).bind(
                        player_id, item_id, make_inventory_instance_id(), quantity),
                    write_result);
            }
            updated = row_to_profile(connection, find_result.rows().at(0));
        });
        return updated;
    }

    mf::game::InventoryPage list_inventory_items(
        const std::string& player_id,
        std::size_t limit,
        const std::optional<std::string>& cursor) const override {
        mf::game::InventoryPage page;
        const auto bounded_limit = std::clamp<std::size_t>(limit, 1, 500);
        const auto fetch_limit = bounded_limit + 1;

        client_->with_connection(__func__, "player.list_inventory_items", [&](mysql::any_connection& connection) {
            mysql::results page_result;
            if (cursor) {
                const auto page_statement = StatementCache::prepare(
                    connection,
                    "SELECT item_id, instance_id, quantity FROM player_inventory "
                    "WHERE player_id = ? AND item_id > ? ORDER BY item_id ASC LIMIT ?");
                connection.execute(
                    page_statement.bind(player_id, *cursor, static_cast<std::uint64_t>(fetch_limit)),
                    page_result);
            } else {
                const auto page_statement = StatementCache::prepare(
                    connection,
                    "SELECT item_id, instance_id, quantity FROM player_inventory "
                    "WHERE player_id = ? ORDER BY item_id ASC LIMIT ?");
                connection.execute(
                    page_statement.bind(player_id, static_cast<std::uint64_t>(fetch_limit)),
                    page_result);
            }
            page.items.reserve(page_result.rows().size());
            for (const auto row : page_result.rows()) {
                page.items.push_back(mf::game::InventoryItem{
                    .instance_id = column_string(row, 1),
                    .item_id = column_string(row, 0),
                    .quantity = column_i64(row, 2),
                });
            }
        });

        if (page.items.size() > bounded_limit) {
            page.items.resize(bounded_limit);
            page.has_more = true;
            page.next_cursor = page.items.back().item_id;
        }
        return page;
    }

    std::optional<mf::game::PlayerProfile> spend_currency(
        const std::string& player_id,
        const std::string& currency_id,
        std::int64_t amount) override {
        if (amount < 0) {
            return std::nullopt;
        }
        std::optional<mf::game::PlayerProfile> updated;
        client_->with_connection(__func__, "player.spend_currency", [&](mysql::any_connection& connection) {
            mysql::results find_result;
            connection.execute(statements_.player_find_by_id(connection).bind(player_id), find_result);
            if (find_result.rows().empty()) {
                return;
            }
            if (amount == 0) {
                updated = row_to_profile(connection, find_result.rows().at(0));
                return;
            }

            mysql::results spend_result;
            if (currency_id == "gold") {
                connection.execute(
                    statements_.player_spend_gold(connection).bind(amount, player_id, amount),
                    spend_result);
            } else if (currency_id == "gem") {
                connection.execute(
                    statements_.player_spend_gem(connection).bind(amount, player_id, amount),
                    spend_result);
            } else {
                connection.execute(
                    statements_.player_currency_spend(connection).bind(amount, player_id, currency_id, amount),
                    spend_result);
            }
            if (spend_result.affected_rows() == 0) {
                return;
            }
            mysql::results refreshed_result;
            connection.execute(statements_.player_find_by_id(connection).bind(player_id), refreshed_result);
            updated = row_to_profile(connection, refreshed_result.rows().at(0));
        });
        return updated;
    }

private:
    std::optional<mf::game::PlayerProfile> find_by_account_id(const std::string& account_id) const {
        std::optional<mf::game::PlayerProfile> profile;

        client_->with_connection(__func__, "player.find_by_account_id", [&](mysql::any_connection& connection) {
            profile = load_player_profile_single_query(connection, "account_id", account_id);
        });

        return profile;
    }

    std::string allocate_player_id(const std::string& account_id) const {
        // 新账号统一分配 8 位数字 ID，方便玩家手输、搜索和加好友。
        // 如果候选号已被其他账号占用，就顺延探测下一个，直到找到空位。
        for (std::uint32_t probe = 0; probe != 1000; ++probe) {
            const auto candidate = make_numeric_player_id(account_id, probe);
            const auto existing = find_by_player_id(candidate);
            if (!existing || existing->account_id == account_id) {
                return candidate;
            }
        }

        throw std::runtime_error("无法分配唯一的 8 位数字 player_id");
    }

    std::shared_ptr<MySqlClient> client_;
    mutable StatementCache statements_;
    struct LoginIdentityCacheEntry {
        mf::game::PlayerProfile profile;
        std::chrono::steady_clock::time_point expires_at;
    };
    mutable std::mutex login_identity_cache_mutex_;
    static constexpr std::size_t k_login_identity_lock_shards = 32;
    mutable std::array<std::mutex, k_login_identity_lock_shards> login_identity_fill_mutexes_;
    mutable std::unordered_map<std::string, LoginIdentityCacheEntry> login_identity_cache_;
};

class MySqlItemCatalogRepository final : public mf::game::ItemCatalogRepository {
public:
    explicit MySqlItemCatalogRepository(std::shared_ptr<MySqlClient> client)
        : client_(std::move(client)) {}

    mf::game::ItemDefinition upsert_item(mf::game::ItemDefinition item) override {
        client_->with_connection(__func__, "item.upsert", [&](mysql::any_connection& connection) {
            mysql::results result;
            connection.execute(
                statements_.item_upsert(connection).bind(
                    item.item_id,
                    item.name,
                    item.description,
                    item.icon,
                    item.rarity,
                    static_cast<std::uint64_t>(item.max_stack),
                    item.item_type,
                    item.enabled ? 1 : 0),
                result);
        });

        if (const auto saved = find_by_item_id(item.item_id)) {
            return *saved;
        }
        return item;
    }

    std::optional<mf::game::ItemDefinition> find_by_item_id(const std::string& item_id) const override {
        std::optional<mf::game::ItemDefinition> item;
        client_->with_connection(__func__, "item.find_by_id", [&](mysql::any_connection& connection) {
            mysql::results result;
            connection.execute(statements_.item_find_by_id(connection).bind(item_id), result);
            if (!result.rows().empty()) {
                item = row_to_item_definition(result.rows().at(0));
            }
        });
        return item;
    }

    std::vector<mf::game::ItemDefinition> list_items(
        std::size_t limit,
        std::size_t offset,
        const std::optional<std::string>& item_id_filter,
        const std::optional<std::string>& name_filter,
        const std::optional<std::string>& rarity_filter,
        const std::optional<bool>& enabled_filter) const override {
        std::vector<mf::game::ItemDefinition> items;
        client_->with_connection(__func__, "item.list_filtered", [&](mysql::any_connection& connection) {
            mysql::results result;
            const auto enabled_value = enabled_filter ? (*enabled_filter ? 1 : 0) : -1;
            connection.execute(
                statements_.item_list_filtered(connection).bind(
                    item_id_filter.value_or(std::string{}),
                    item_id_filter.value_or(std::string{}),
                    name_filter.value_or(std::string{}),
                    name_filter.value_or(std::string{}),
                    rarity_filter.value_or(std::string{}),
                    rarity_filter.value_or(std::string{}),
                    enabled_value,
                    enabled_value,
                    static_cast<std::uint64_t>(limit),
                    static_cast<std::uint64_t>(offset)),
                result);
            items.reserve(result.rows().size());
            for (const auto row : result.rows()) {
                items.push_back(row_to_item_definition(row));
            }
        });
        return items;
    }

    std::size_t count_items(
        const std::optional<std::string>& item_id_filter,
        const std::optional<std::string>& name_filter,
        const std::optional<std::string>& rarity_filter,
        const std::optional<bool>& enabled_filter) const override {
        std::size_t total = 0;
        client_->with_connection(__func__, "item.count_filtered", [&](mysql::any_connection& connection) {
            mysql::results result;
            const auto enabled_value = enabled_filter ? (*enabled_filter ? 1 : 0) : -1;
            connection.execute(
                statements_.item_count_filtered(connection).bind(
                    item_id_filter.value_or(std::string{}),
                    item_id_filter.value_or(std::string{}),
                    name_filter.value_or(std::string{}),
                    name_filter.value_or(std::string{}),
                    rarity_filter.value_or(std::string{}),
                    rarity_filter.value_or(std::string{}),
                    enabled_value,
                    enabled_value),
                result);
            if (!result.rows().empty()) {
                total = static_cast<std::size_t>(column_i64(result.rows().at(0), 0));
            }
        });
        return total;
    }

private:
    std::shared_ptr<MySqlClient> client_;
    mutable StatementCache statements_;
};

class MySqlGachaRepository final : public mf::game::GachaRepository {
public:
    explicit MySqlGachaRepository(std::shared_ptr<MySqlClient> client)
        : client_(std::move(client)) {}

    mf::game::GachaPool upsert_pool(mf::game::GachaPool pool) override {
        const auto starts_at = pool.starts_at ? std::optional<std::int64_t>{to_unix_seconds(*pool.starts_at)} : std::nullopt;
        const auto ends_at = pool.ends_at ? std::optional<std::int64_t>{to_unix_seconds(*pool.ends_at)} : std::nullopt;
        client_->with_connection(__func__, "gacha.pool_upsert", [&](mysql::any_connection& connection) {
            mysql::results result;
            const auto statement = StatementCache::prepare(connection, 
                "INSERT INTO gacha_pool(pool_id, pool_type, name, description, banner_icon, cost_currency_id, "
                "cost_amount_per_draw, pity_5, pity_4, enabled, starts_at, ends_at) "
                "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, FROM_UNIXTIME(?), FROM_UNIXTIME(?)) "
                "ON DUPLICATE KEY UPDATE pool_type = VALUES(pool_type), name = VALUES(name), description = VALUES(description), "
                "banner_icon = VALUES(banner_icon), cost_currency_id = VALUES(cost_currency_id), "
                "cost_amount_per_draw = VALUES(cost_amount_per_draw), pity_5 = VALUES(pity_5), "
                "pity_4 = VALUES(pity_4), enabled = VALUES(enabled), starts_at = VALUES(starts_at), "
                "ends_at = VALUES(ends_at), updated_at = CURRENT_TIMESTAMP");
            connection.execute(
                statement.bind(
                    pool.pool_id,
                    pool.pool_type,
                    pool.name,
                    pool.description,
                    pool.banner_icon,
                    pool.cost_currency_id,
                    pool.cost_amount_per_draw,
                    pool.pity_5,
                    pool.pity_4,
                    pool.enabled ? 1 : 0,
                    starts_at,
                    ends_at),
                result);
        });
        return find_pool(pool.pool_id).value_or(pool);
    }

    bool delete_pool(const std::string& pool_id) override {
        bool deleted = false;
        client_->with_connection(__func__, "gacha.pool_delete", [&](mysql::any_connection& connection) {
            mysql::results result;
            connection.execute("START TRANSACTION", result);
            try {
                const auto delete_state = StatementCache::prepare(connection, 
                    "DELETE FROM gacha_player_state WHERE pool_id = ?");
                connection.execute(delete_state.bind(pool_id), result);

                const auto delete_pool_statement = StatementCache::prepare(connection, 
                    "DELETE FROM gacha_pool WHERE pool_id = ?");
                connection.execute(delete_pool_statement.bind(pool_id), result);
                deleted = result.affected_rows() > 0;
                connection.execute("COMMIT", result);
            } catch (...) {
                connection.execute("ROLLBACK", result);
                throw;
            }
        });
        return deleted;
    }

    std::optional<mf::game::GachaPool> find_pool(const std::string& pool_id) const override {
        std::optional<mf::game::GachaPool> pool;
        client_->with_connection(__func__, "gacha.pool_find", [&](mysql::any_connection& connection) {
            mysql::results result;
            const auto statement = StatementCache::prepare(connection, 
                "SELECT pool_id, pool_type, name, description, banner_icon, cost_currency_id, cost_amount_per_draw, "
                "pity_5, pity_4, enabled, UNIX_TIMESTAMP(starts_at), UNIX_TIMESTAMP(ends_at), "
                "UNIX_TIMESTAMP(created_at), UNIX_TIMESTAMP(updated_at) "
                "FROM gacha_pool WHERE pool_id = ? LIMIT 1");
            connection.execute(statement.bind(pool_id), result);
            if (!result.rows().empty()) {
                pool = row_to_gacha_pool(result.rows().at(0));
            }
        });
        return pool;
    }

    std::vector<mf::game::GachaPool> list_pools(
        bool include_disabled,
        std::chrono::system_clock::time_point now) const override {
        std::vector<mf::game::GachaPool> pools;
        client_->with_connection(__func__, "gacha.pool_list", [&](mysql::any_connection& connection) {
            mysql::results result;
            const auto statement = StatementCache::prepare(connection, 
                "SELECT pool_id, pool_type, name, description, banner_icon, cost_currency_id, cost_amount_per_draw, "
                "pity_5, pity_4, enabled, UNIX_TIMESTAMP(starts_at), UNIX_TIMESTAMP(ends_at), "
                "UNIX_TIMESTAMP(created_at), UNIX_TIMESTAMP(updated_at) "
                "FROM gacha_pool "
                "WHERE (? = 1 OR (enabled = 1 AND (starts_at IS NULL OR starts_at <= FROM_UNIXTIME(?)) "
                "AND (ends_at IS NULL OR ends_at >= FROM_UNIXTIME(?)))) "
                "ORDER BY updated_at DESC, pool_id ASC");
            connection.execute(
                statement.bind(include_disabled ? 1 : 0, to_unix_seconds(now), to_unix_seconds(now)),
                result);
            pools.reserve(result.rows().size());
            for (const auto row : result.rows()) {
                pools.push_back(row_to_gacha_pool(row));
            }
        });
        return pools;
    }

    mf::game::GachaPoolItem upsert_pool_item(mf::game::GachaPoolItem item) override {
        client_->with_connection(__func__, "gacha.item_upsert", [&](mysql::any_connection& connection) {
            mysql::results result;
            const auto statement = StatementCache::prepare(connection, 
                "INSERT INTO gacha_pool_item(pool_id, item_key, rarity, weight, is_up, "
                "reward_kind, reward_resource_id, reward_amount) "
                "VALUES (?, ?, ?, ?, ?, ?, ?, ?) "
                "ON DUPLICATE KEY UPDATE rarity = VALUES(rarity), weight = VALUES(weight), "
                "is_up = VALUES(is_up), reward_kind = VALUES(reward_kind), "
                "reward_resource_id = VALUES(reward_resource_id), reward_amount = VALUES(reward_amount), "
                "updated_at = CURRENT_TIMESTAMP");
            connection.execute(
                statement.bind(
                    item.pool_id,
                    item.item_key,
                    item.rarity,
                    item.weight,
                    item.is_up ? 1 : 0,
                    reward_kind_to_string(item.reward.kind),
                    item.reward.resource_id,
                    item.reward.amount),
                result);
        });
        return item;
    }

    bool delete_pool_item(const std::string& pool_id, const std::string& item_key) override {
        bool deleted = false;
        client_->with_connection(__func__, "gacha.item_delete", [&](mysql::any_connection& connection) {
            mysql::results result;
            const auto statement = StatementCache::prepare(connection, 
                "DELETE FROM gacha_pool_item WHERE pool_id = ? AND item_key = ?");
            connection.execute(statement.bind(pool_id, item_key), result);
            deleted = result.affected_rows() > 0;
        });
        return deleted;
    }

    std::vector<mf::game::GachaPoolItem> list_pool_items(const std::string& pool_id) const override {
        std::vector<mf::game::GachaPoolItem> items;
        client_->with_connection(__func__, "gacha.item_list", [&](mysql::any_connection& connection) {
            mysql::results result;
            const auto statement = StatementCache::prepare(connection, 
                "SELECT pool_id, item_key, rarity, weight, is_up, reward_kind, reward_resource_id, reward_amount "
                "FROM gacha_pool_item WHERE pool_id = ? ORDER BY rarity DESC, item_key ASC");
            connection.execute(statement.bind(pool_id), result);
            items.reserve(result.rows().size());
            for (const auto row : result.rows()) {
                items.push_back(row_to_gacha_pool_item(row));
            }
        });
        return items;
    }

    mf::game::GachaPlayerState get_player_state(
        const std::string& player_id,
        const std::string& pool_id) const override {
        std::optional<mf::game::GachaPlayerState> state;
        client_->with_connection(__func__, "gacha.state_get", [&](mysql::any_connection& connection) {
            mysql::results result;
            const auto statement = StatementCache::prepare(connection, 
                "SELECT player_id, pool_id, pity_5_count, pity_4_count, total_draws, UNIX_TIMESTAMP(updated_at) "
                "FROM gacha_player_state WHERE player_id = ? AND pool_id = ? LIMIT 1");
            connection.execute(statement.bind(player_id, pool_id), result);
            if (!result.rows().empty()) {
                state = row_to_gacha_player_state(result.rows().at(0));
            }
        });
        return state.value_or(mf::game::GachaPlayerState{.player_id = player_id, .pool_id = pool_id});
    }

    mf::game::GachaPlayerState save_player_state(mf::game::GachaPlayerState state) override {
        client_->with_connection(__func__, "gacha.state_save", [&](mysql::any_connection& connection) {
            mysql::results result;
            const auto statement = StatementCache::prepare(connection, 
                "INSERT INTO gacha_player_state(player_id, pool_id, pity_5_count, pity_4_count, total_draws) "
                "VALUES (?, ?, ?, ?, ?) "
                "ON DUPLICATE KEY UPDATE pity_5_count = VALUES(pity_5_count), "
                "pity_4_count = VALUES(pity_4_count), total_draws = VALUES(total_draws), "
                "updated_at = CURRENT_TIMESTAMP");
            connection.execute(
                statement.bind(
                    state.player_id,
                    state.pool_id,
                    state.pity_5_count,
                    state.pity_4_count,
                    state.total_draws),
                result);
        });
        return get_player_state(state.player_id, state.pool_id);
    }

    void append_history(const std::vector<mf::game::GachaHistoryEntry>& entries) override {
        if (entries.empty()) {
            return;
        }
        client_->with_connection(__func__, "gacha.history_append", [&](mysql::any_connection& connection) {
            mysql::results result;
            const auto statement = StatementCache::prepare(connection, 
                "INSERT INTO gacha_history(history_id, player_id, pool_id, item_key, rarity, is_up, "
                "reward_kind, reward_resource_id, reward_amount, pity_5_before, pity_4_before, drawn_at) "
                "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, FROM_UNIXTIME(?))");
            for (const auto& entry : entries) {
                connection.execute(
                    statement.bind(
                        entry.history_id,
                        entry.player_id,
                        entry.pool_id,
                        entry.item_key,
                        entry.rarity,
                        entry.is_up ? 1 : 0,
                        reward_kind_to_string(entry.reward.kind),
                        entry.reward.resource_id,
                        entry.reward.amount,
                        entry.pity_5_before,
                        entry.pity_4_before,
                        entry.drawn_at ? to_unix_seconds(*entry.drawn_at) : to_unix_seconds(std::chrono::system_clock::now())),
                    result);
            }
        });
    }

    mf::game::GachaHistoryPage list_history(
        const std::string& player_id,
        const std::string& pool_id,
        std::size_t limit,
        const std::optional<mf::game::GachaHistoryCursor>& cursor) const override {
        mf::game::GachaHistoryPage page;
        const auto bounded_limit = std::clamp<std::size_t>(limit, 1, 100);
        const auto fetch_limit = bounded_limit + 1;

        client_->with_connection(__func__, "gacha.history_list", [&](mysql::any_connection& connection) {
            mysql::results result;
            if (cursor) {
                const auto statement = StatementCache::prepare(connection, 
                    "SELECT history_id, player_id, pool_id, item_key, rarity, is_up, reward_kind, reward_resource_id, "
                    "reward_amount, pity_5_before, pity_4_before, UNIX_TIMESTAMP(drawn_at) "
                    "FROM gacha_history WHERE player_id = ? AND (? = '' OR pool_id = ?) "
                    "AND (drawn_at < FROM_UNIXTIME(?) OR (drawn_at = FROM_UNIXTIME(?) AND history_id < ?)) "
                    "ORDER BY drawn_at DESC, history_id DESC LIMIT ?");
                connection.execute(
                    statement.bind(
                        player_id,
                        pool_id,
                        pool_id,
                        cursor->last_drawn_at,
                        cursor->last_drawn_at,
                        cursor->last_history_id,
                        static_cast<std::uint64_t>(fetch_limit)),
                    result);
            } else {
                const auto statement = StatementCache::prepare(connection, 
                    "SELECT history_id, player_id, pool_id, item_key, rarity, is_up, reward_kind, reward_resource_id, "
                    "reward_amount, pity_5_before, pity_4_before, UNIX_TIMESTAMP(drawn_at) "
                    "FROM gacha_history WHERE player_id = ? AND (? = '' OR pool_id = ?) "
                    "ORDER BY drawn_at DESC, history_id DESC LIMIT ?");
                connection.execute(
                    statement.bind(
                        player_id,
                        pool_id,
                        pool_id,
                        static_cast<std::uint64_t>(fetch_limit)),
                    result);
            }
            page.items.reserve(result.rows().size());
            for (const auto row : result.rows()) {
                page.items.push_back(row_to_gacha_history(row));
            }
        });

        if (page.items.size() > bounded_limit) {
            page.items.resize(bounded_limit);
            page.has_more = true;
            const auto& oldest = page.items.back();
            page.next_drawn_at = to_unix_seconds(oldest.drawn_at.value_or(std::chrono::system_clock::time_point{}));
            page.next_history_id = oldest.history_id;
        }
        return page;
    }

private:
    std::shared_ptr<MySqlClient> client_;
};

class MySqlGachaTransactionRepository final : public mf::game::GachaTransactionRepository {
public:
    explicit MySqlGachaTransactionRepository(std::shared_ptr<MySqlClient> client)
        : client_(std::move(client)) {}

    mf::game::GachaDrawTransactionResult draw_once(
        const std::string& player_id,
        const std::string& pool_id,
        const mf::game::GachaPool& pool,
        const std::vector<mf::game::GachaPoolItem>& items,
        std::int64_t count,
        std::int64_t total_cost,
        const std::vector<std::string>& history_ids,
        const std::string& grant_id,
        std::chrono::system_clock::time_point drawn_at,
        const std::function<mf::game::GachaPoolItem(const mf::game::GachaPlayerState&)>& pick_item) override {
        (void)items;
        return client_->with_connection(__func__, "gacha.draw_once", [&](mysql::any_connection& connection) {
            mysql::results control_result;
            connection.execute("START TRANSACTION", control_result);
            try {
                mf::game::GachaDrawTransactionResult result;

                // 用 INSERT ... ON DUPLICATE KEY UPDATE 确保状态行存在并拿到行锁，
                // 避免“SELECT FOR UPDATE 命中空行再 INSERT”在并发首次抽卡时产生 gap 死锁。
                const auto ensure_state_statement = StatementCache::prepare(connection,
                    "INSERT INTO gacha_player_state(player_id, pool_id, pity_5_count, pity_4_count, total_draws) "
                    "VALUES (?, ?, 0, 0, 0) "
                    "ON DUPLICATE KEY UPDATE updated_at = CURRENT_TIMESTAMP");
                connection.execute(ensure_state_statement.bind(player_id, pool_id), control_result);

                const auto read_state_statement = StatementCache::prepare(connection,
                    "SELECT player_id, pool_id, pity_5_count, pity_4_count, total_draws, UNIX_TIMESTAMP(updated_at) "
                    "FROM gacha_player_state WHERE player_id = ? AND pool_id = ?");
                mysql::results state_result;
                connection.execute(read_state_statement.bind(player_id, pool_id), state_result);
                mf::game::GachaPlayerState state;
                state = state_result.rows().empty()
                            ? mf::game::GachaPlayerState{.player_id = player_id, .pool_id = pool_id}
                            : row_to_gacha_player_state(state_result.rows().at(0));

                // 扣费使用条件 UPDATE，余额不足时不影响任何数据，直接回滚返回。
                if (total_cost > 0) {
                    mysql::results spend_result;
                    if (pool.cost_currency_id == "gold") {
                        const auto statement = StatementCache::prepare(connection,
                            "UPDATE player_profile SET gold = gold - ?, updated_at = CURRENT_TIMESTAMP "
                            "WHERE player_id = ? AND gold >= ?");
                        connection.execute(statement.bind(total_cost, player_id, total_cost), spend_result);
                    } else if (pool.cost_currency_id == "gem") {
                        const auto statement = StatementCache::prepare(connection,
                            "UPDATE player_profile SET gem = gem - ?, updated_at = CURRENT_TIMESTAMP "
                            "WHERE player_id = ? AND gem >= ?");
                        connection.execute(statement.bind(total_cost, player_id, total_cost), spend_result);
                    } else {
                        const auto statement = StatementCache::prepare(connection,
                            "UPDATE player_currency SET amount = amount - ? "
                            "WHERE player_id = ? AND currency_id = ? AND amount >= ?");
                        connection.execute(
                            statement.bind(total_cost, player_id, pool.cost_currency_id, total_cost),
                            spend_result);
                    }
                    if (spend_result.affected_rows() == 0) {
                        result.currency_not_enough = true;
                        connection.execute("ROLLBACK", control_result);
                        return result;
                    }
                }

                std::vector<mf::game::RewardEntry> rewards;
                result.history.reserve(static_cast<std::size_t>(count));
                rewards.reserve(static_cast<std::size_t>(count));
                for (std::int64_t index = 0; index < count; ++index) {
                    const auto pity_5_before = state.pity_5_count;
                    const auto pity_4_before = state.pity_4_count;
                    const auto item = pick_item(state);

                    if (item.rarity >= 5) {
                        state.pity_5_count = 0;
                        state.pity_4_count = 0;
                    } else if (item.rarity >= 4) {
                        ++state.pity_5_count;
                        state.pity_4_count = 0;
                    } else {
                        ++state.pity_5_count;
                        ++state.pity_4_count;
                    }
                    ++state.total_draws;

                    result.history.push_back(mf::game::GachaHistoryEntry{
                        .history_id = history_ids[static_cast<std::size_t>(index)],
                        .player_id = player_id,
                        .pool_id = pool_id,
                        .item_key = item.item_key,
                        .rarity = item.rarity,
                        .is_up = item.is_up,
                        .reward = item.reward,
                        .pity_5_before = pity_5_before,
                        .pity_4_before = pity_4_before,
                        .drawn_at = drawn_at,
                    });
                    rewards.push_back(item.reward);
                }

                const auto update_state_statement = StatementCache::prepare(connection,
                    "UPDATE gacha_player_state SET pity_5_count = ?, pity_4_count = ?, total_draws = ?, "
                    "updated_at = CURRENT_TIMESTAMP WHERE player_id = ? AND pool_id = ?");
                connection.execute(
                    update_state_statement.bind(
                        state.pity_5_count,
                        state.pity_4_count,
                        state.total_draws,
                        player_id,
                        pool_id),
                    control_result);
                state.updated_at = drawn_at;

                const auto insert_history_statement = StatementCache::prepare(connection,
                    "INSERT INTO gacha_history(history_id, player_id, pool_id, item_key, rarity, is_up, "
                    "reward_kind, reward_resource_id, reward_amount, pity_5_before, pity_4_before, drawn_at) "
                    "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, FROM_UNIXTIME(?))");
                for (const auto& entry : result.history) {
                    connection.execute(
                        insert_history_statement.bind(
                            entry.history_id,
                            entry.player_id,
                            entry.pool_id,
                            entry.item_key,
                            entry.rarity,
                            entry.is_up ? 1 : 0,
                            reward_kind_to_string(entry.reward.kind),
                            entry.reward.resource_id,
                            entry.reward.amount,
                            entry.pity_5_before,
                            entry.pity_4_before,
                            to_unix_seconds(entry.drawn_at.value_or(drawn_at))),
                        control_result);
                }

                const auto insert_grant_statement = StatementCache::prepare(connection,
                    "INSERT IGNORE INTO reward_grant(grant_id, player_id, source_type, source_id, rewards_json, granted_at) "
                    "VALUES (?, ?, 'gacha', ?, CAST(? AS JSON), FROM_UNIXTIME(?))");
                mysql::results insert_grant_result;
                connection.execute(
                    insert_grant_statement.bind(
                        grant_id,
                        player_id,
                        pool_id,
                        encode_rewards_json(rewards),
                        to_unix_seconds(drawn_at)),
                    insert_grant_result);

                if (insert_grant_result.affected_rows() == 0) {
                    const auto grant_statement = StatementCache::prepare(connection,
                        "SELECT grant_id, player_id, source_type, source_id, CAST(rewards_json AS CHAR), "
                        "UNIX_TIMESTAMP(granted_at) FROM reward_grant WHERE grant_id = ?");
                    mysql::results grant_result;
                    connection.execute(grant_statement.bind(grant_id), grant_result);
                    if (!grant_result.rows().empty()) {
                        result.grant = row_to_reward_grant(grant_result.rows().at(0));
                    }
                    connection.execute("ROLLBACK", control_result);
                    result.already_granted = true;
                    return result;
                }

                auto player = apply_rewards_in_transaction(connection, player_id, rewards);
                if (!player) {
                    result.player_not_found = true;
                    connection.execute("ROLLBACK", control_result);
                    return result;
                }

                result.grant = mf::game::RewardGrantRecord{
                    .grant_id = grant_id,
                    .player_id = player_id,
                    .source_type = "gacha",
                    .source_id = pool_id,
                    .rewards = std::move(rewards),
                    .granted_at = drawn_at,
                };
                result.state = std::move(state);
                connection.execute("COMMIT", control_result);
                return result;
            } catch (...) {
                try { connection.execute("ROLLBACK", control_result); } catch (...) {}
                throw;
            }
        });
    }

private:
    std::shared_ptr<MySqlClient> client_;
};

class MySqlShopRepository final : public mf::game::ShopRepository {
public:
    explicit MySqlShopRepository(std::shared_ptr<MySqlClient> client)
        : client_(std::move(client)) {}

    // 新增或更新商城商品配置。
    // rewards_json 存统一奖励数组，方便商品可以同时发货币和道具。
    mf::game::ShopProduct upsert_product(mf::game::ShopProduct product) override {
        const auto starts_at = product.starts_at ? std::optional<std::int64_t>{to_unix_seconds(*product.starts_at)} : std::nullopt;
        const auto ends_at = product.ends_at ? std::optional<std::int64_t>{to_unix_seconds(*product.ends_at)} : std::nullopt;
        const auto rewards_payload = encode_rewards_json(product.rewards);
        client_->with_connection(__func__, "shop.product_upsert", [&](mysql::any_connection& connection) {
            mysql::results result;
            const auto statement = StatementCache::prepare(connection, 
                "INSERT INTO shop_product(product_id, category, name, description, icon, price_currency_id, "
                "price_amount, rewards_json, purchase_limit, enabled, starts_at, ends_at) "
                "VALUES (?, ?, ?, ?, ?, ?, ?, CAST(? AS JSON), ?, ?, FROM_UNIXTIME(?), FROM_UNIXTIME(?)) "
                "ON DUPLICATE KEY UPDATE category = VALUES(category), name = VALUES(name), "
                "description = VALUES(description), icon = VALUES(icon), price_currency_id = VALUES(price_currency_id), "
                "price_amount = VALUES(price_amount), rewards_json = VALUES(rewards_json), "
                "purchase_limit = VALUES(purchase_limit), enabled = VALUES(enabled), starts_at = VALUES(starts_at), "
                "ends_at = VALUES(ends_at), updated_at = CURRENT_TIMESTAMP");
            connection.execute(
                statement.bind(
                    product.product_id,
                    product.category,
                    product.name,
                    product.description,
                    product.icon,
                    product.price_currency_id,
                    product.price_amount,
                    rewards_payload,
                    product.purchase_limit,
                    product.enabled ? 1 : 0,
                    starts_at,
                    ends_at),
                result);
        });
        return find_product(product.product_id).value_or(product);
    }

    // 删除商品配置。
    // 这里只删配置，不删 shop_purchase 历史流水。
    bool delete_product(const std::string& product_id) override {
        bool deleted = false;
        client_->with_connection(__func__, "shop.product_delete", [&](mysql::any_connection& connection) {
            mysql::results result;
            const auto statement = StatementCache::prepare(connection, "DELETE FROM shop_product WHERE product_id = ?");
            connection.execute(statement.bind(product_id), result);
            deleted = result.affected_rows() > 0;
        });
        return deleted;
    }

    // 按商品 ID 查单个商品。
    std::optional<mf::game::ShopProduct> find_product(const std::string& product_id) const override {
        std::optional<mf::game::ShopProduct> product;
        client_->with_connection(__func__, "shop.product_find", [&](mysql::any_connection& connection) {
            mysql::results result;
            const auto statement = StatementCache::prepare(connection, 
                "SELECT product_id, category, name, description, icon, price_currency_id, price_amount, "
                "CAST(rewards_json AS CHAR), purchase_limit, enabled, UNIX_TIMESTAMP(starts_at), "
                "UNIX_TIMESTAMP(ends_at), UNIX_TIMESTAMP(created_at), UNIX_TIMESTAMP(updated_at) "
                "FROM shop_product WHERE product_id = ? LIMIT 1");
            connection.execute(statement.bind(product_id), result);
            if (!result.rows().empty()) {
                product = row_to_shop_product(result.rows().at(0));
            }
        });
        return product;
    }

    // 列出商品。
    // 玩家侧 include_disabled=false，只返回启用且在时间窗口内的商品。
    std::vector<mf::game::ShopProduct> list_products(
        bool include_disabled,
        std::chrono::system_clock::time_point now) const override {
        std::vector<mf::game::ShopProduct> products;
        client_->with_connection(__func__, "shop.product_list", [&](mysql::any_connection& connection) {
            mysql::results result;
            const auto statement = StatementCache::prepare(connection, 
                "SELECT product_id, category, name, description, icon, price_currency_id, price_amount, "
                "CAST(rewards_json AS CHAR), purchase_limit, enabled, UNIX_TIMESTAMP(starts_at), "
                "UNIX_TIMESTAMP(ends_at), UNIX_TIMESTAMP(created_at), UNIX_TIMESTAMP(updated_at) "
                "FROM shop_product "
                "WHERE (? = 1 OR (enabled = 1 AND (starts_at IS NULL OR starts_at <= FROM_UNIXTIME(?)) "
                "AND (ends_at IS NULL OR ends_at >= FROM_UNIXTIME(?)))) "
                "ORDER BY category ASC, updated_at DESC, product_id ASC");
            connection.execute(
                statement.bind(include_disabled ? 1 : 0, to_unix_seconds(now), to_unix_seconds(now)),
                result);
            products.reserve(result.rows().size());
            for (const auto row : result.rows()) {
                products.push_back(row_to_shop_product(row));
            }
        });
        return products;
    }

    // 统计某玩家对某商品的购买次数，用于限购。
    std::int64_t count_purchases(const std::string& player_id, const std::string& product_id) const override {
        std::int64_t total = 0;
        client_->with_connection(__func__, "shop.purchase_count", [&](mysql::any_connection& connection) {
            mysql::results result;
            const auto statement = StatementCache::prepare(connection, 
                "SELECT COUNT(*) FROM shop_purchase WHERE player_id = ? AND product_id = ?");
            connection.execute(statement.bind(player_id, product_id), result);
            if (!result.rows().empty()) {
                total = column_i64(result.rows().at(0), 0);
            }
        });
        return total;
    }

    // 写入购买流水。
    mf::game::ShopPurchaseRecord append_purchase(mf::game::ShopPurchaseRecord purchase) override {
        const auto purchased_at = purchase.purchased_at.value_or(std::chrono::system_clock::now());
        const auto rewards_payload = encode_rewards_json(purchase.rewards);
        client_->with_connection(__func__, "shop.purchase_append", [&](mysql::any_connection& connection) {
            mysql::results result;
            const auto statement = StatementCache::prepare(connection, 
                "INSERT INTO shop_purchase(purchase_id, player_id, product_id, price_currency_id, "
                "price_amount, rewards_json, purchased_at) "
                "VALUES (?, ?, ?, ?, ?, CAST(? AS JSON), FROM_UNIXTIME(?))");
            connection.execute(
                statement.bind(
                    purchase.purchase_id,
                    purchase.player_id,
                    purchase.product_id,
                    purchase.price_currency_id,
                    purchase.price_amount,
                    rewards_payload,
                    to_unix_seconds(purchased_at)),
                result);
        });
        purchase.purchased_at = purchased_at;
        return purchase;
    }

    // 查询玩家购买历史。
    mf::game::ShopPurchasePage list_purchases(
        const std::string& player_id,
        std::size_t limit,
        const std::optional<mf::game::ShopPurchaseCursor>& cursor) const override {
        mf::game::ShopPurchasePage page;
        const auto bounded_limit = std::clamp<std::size_t>(limit, 1, 100);
        const auto fetch_limit = bounded_limit + 1;

        client_->with_connection(__func__, "shop.purchase_list", [&](mysql::any_connection& connection) {
            mysql::results result;
            if (cursor) {
                const auto statement = StatementCache::prepare(connection, 
                    "SELECT purchase_id, player_id, product_id, price_currency_id, price_amount, "
                    "CAST(rewards_json AS CHAR), UNIX_TIMESTAMP(purchased_at) "
                    "FROM shop_purchase WHERE player_id = ? "
                    "AND (purchased_at < FROM_UNIXTIME(?) OR (purchased_at = FROM_UNIXTIME(?) AND purchase_id < ?)) "
                    "ORDER BY purchased_at DESC, purchase_id DESC LIMIT ?");
                connection.execute(
                    statement.bind(
                        player_id,
                        cursor->last_purchased_at,
                        cursor->last_purchased_at,
                        cursor->last_purchase_id,
                        static_cast<std::uint64_t>(fetch_limit)),
                    result);
            } else {
                const auto statement = StatementCache::prepare(connection, 
                    "SELECT purchase_id, player_id, product_id, price_currency_id, price_amount, "
                    "CAST(rewards_json AS CHAR), UNIX_TIMESTAMP(purchased_at) "
                    "FROM shop_purchase WHERE player_id = ? "
                    "ORDER BY purchased_at DESC, purchase_id DESC LIMIT ?");
                connection.execute(
                    statement.bind(player_id, static_cast<std::uint64_t>(fetch_limit)),
                    result);
            }
            page.items.reserve(result.rows().size());
            for (const auto row : result.rows()) {
                page.items.push_back(row_to_shop_purchase(row));
            }
        });

        if (page.items.size() > bounded_limit) {
            page.items.resize(bounded_limit);
            page.has_more = true;
            const auto& oldest = page.items.back();
            page.next_purchased_at = to_unix_seconds(oldest.purchased_at.value_or(std::chrono::system_clock::time_point{}));
            page.next_purchase_id = oldest.purchase_id;
        }
        return page;
    }

private:
    std::shared_ptr<MySqlClient> client_;
};

class MySqlLootRepository final : public mf::game::LootRepository {
public:
    explicit MySqlLootRepository(std::shared_ptr<MySqlClient> client)
        : client_(std::move(client)) {}

    // 创建掉落记录。
    mf::game::LootDrop create_drop(mf::game::LootDrop drop) override {
        const auto spawned_at = drop.spawned_at.value_or(std::chrono::system_clock::now());
        const auto expires_at = drop.expires_at ? std::optional<std::int64_t>{to_unix_seconds(*drop.expires_at)} : std::nullopt;
        const auto rewards_payload = encode_rewards_json(drop.rewards);
        client_->with_connection(__func__, "loot.drop_create", [&](mysql::any_connection& connection) {
            mysql::results result;
            const auto statement = StatementCache::prepare(connection, 
                "INSERT INTO loot_drop(drop_id, owner_player_id, scene_id, rewards_json, picked, "
                "picked_by_player_id, spawned_at, expires_at, picked_at) "
                "VALUES (?, ?, ?, CAST(? AS JSON), ?, ?, FROM_UNIXTIME(?), FROM_UNIXTIME(?), NULL)");
            connection.execute(
                statement.bind(
                    drop.drop_id,
                    drop.owner_player_id,
                    drop.scene_id,
                    rewards_payload,
                    drop.picked ? 1 : 0,
                    drop.picked_by_player_id,
                    to_unix_seconds(spawned_at),
                    expires_at),
                result);
        });
        return find_drop(drop.drop_id).value_or(drop);
    }

    // 按 drop_id 查询掉落。
    std::optional<mf::game::LootDrop> find_drop(const std::string& drop_id) const override {
        std::optional<mf::game::LootDrop> drop;
        client_->with_connection(__func__, "loot.drop_find", [&](mysql::any_connection& connection) {
            mysql::results result;
            const auto statement = StatementCache::prepare(connection, 
                "SELECT drop_id, owner_player_id, scene_id, CAST(rewards_json AS CHAR), picked, "
                "picked_by_player_id, UNIX_TIMESTAMP(spawned_at), UNIX_TIMESTAMP(expires_at), UNIX_TIMESTAMP(picked_at) "
                "FROM loot_drop WHERE drop_id = ? LIMIT 1");
            connection.execute(statement.bind(drop_id), result);
            if (!result.rows().empty()) {
                drop = row_to_loot_drop(result.rows().at(0));
            }
        });
        return drop;
    }

    // 原子标记掉落已拾取。
    // WHERE picked=0 让重复拾取只能有一个请求成功。
    std::optional<mf::game::LootDrop> mark_picked(
        const std::string& drop_id,
        const std::string& picked_by_player_id,
        std::chrono::system_clock::time_point picked_at) override {
        bool updated = false;
        client_->with_connection(__func__, "loot.drop_mark_picked", [&](mysql::any_connection& connection) {
            mysql::results result;
            const auto statement = StatementCache::prepare(connection, 
                "UPDATE loot_drop SET picked = 1, picked_by_player_id = ?, picked_at = FROM_UNIXTIME(?) "
                "WHERE drop_id = ? AND picked = 0");
            connection.execute(statement.bind(picked_by_player_id, to_unix_seconds(picked_at), drop_id), result);
            updated = result.affected_rows() > 0;
        });
        if (!updated) {
            return std::nullopt;
        }
        return find_drop(drop_id);
    }

    // 查询掉落列表。
    mf::game::LootDropPage list_drops(
        const std::string& owner_player_id,
        bool include_picked,
        std::chrono::system_clock::time_point now,
        std::size_t limit,
        const std::optional<mf::game::LootDropCursor>& cursor) const override {
        mf::game::LootDropPage page;
        const auto bounded_limit = std::clamp<std::size_t>(limit, 1, 100);
        const auto fetch_limit = bounded_limit + 1;
        const auto include_picked_flag = include_picked ? 1 : 0;
        const auto now_seconds = to_unix_seconds(now);

        client_->with_connection(__func__, "loot.drop_list", [&](mysql::any_connection& connection) {
            mysql::results result;
            if (cursor) {
                const auto statement = StatementCache::prepare(connection, 
                    "SELECT drop_id, owner_player_id, scene_id, CAST(rewards_json AS CHAR), picked, "
                    "picked_by_player_id, UNIX_TIMESTAMP(spawned_at), UNIX_TIMESTAMP(expires_at), UNIX_TIMESTAMP(picked_at) "
                    "FROM loot_drop "
                    "WHERE (? = '' OR owner_player_id = ?) "
                    "AND (? = 1 OR picked = 0) "
                    "AND (? = 1 OR expires_at IS NULL OR expires_at >= FROM_UNIXTIME(?)) "
                    "AND (spawned_at < FROM_UNIXTIME(?) OR (spawned_at = FROM_UNIXTIME(?) AND drop_id < ?)) "
                    "ORDER BY spawned_at DESC, drop_id DESC LIMIT ?");
                connection.execute(
                    statement.bind(
                        owner_player_id,
                        owner_player_id,
                        include_picked_flag,
                        include_picked_flag,
                        now_seconds,
                        cursor->last_spawned_at,
                        cursor->last_spawned_at,
                        cursor->last_drop_id,
                        static_cast<std::uint64_t>(fetch_limit)),
                    result);
            } else {
                const auto statement = StatementCache::prepare(connection, 
                    "SELECT drop_id, owner_player_id, scene_id, CAST(rewards_json AS CHAR), picked, "
                    "picked_by_player_id, UNIX_TIMESTAMP(spawned_at), UNIX_TIMESTAMP(expires_at), UNIX_TIMESTAMP(picked_at) "
                    "FROM loot_drop "
                    "WHERE (? = '' OR owner_player_id = ?) "
                    "AND (? = 1 OR picked = 0) "
                    "AND (? = 1 OR expires_at IS NULL OR expires_at >= FROM_UNIXTIME(?)) "
                    "ORDER BY spawned_at DESC, drop_id DESC LIMIT ?");
                connection.execute(
                    statement.bind(
                        owner_player_id,
                        owner_player_id,
                        include_picked_flag,
                        include_picked_flag,
                        now_seconds,
                        static_cast<std::uint64_t>(fetch_limit)),
                    result);
            }
            page.items.reserve(result.rows().size());
            for (const auto row : result.rows()) {
                page.items.push_back(row_to_loot_drop(row));
            }
        });

        if (page.items.size() > bounded_limit) {
            page.items.resize(bounded_limit);
            page.has_more = true;
            const auto& oldest = page.items.back();
            page.next_spawned_at = to_unix_seconds(oldest.spawned_at.value_or(std::chrono::system_clock::time_point{}));
            page.next_drop_id = oldest.drop_id;
        }
        return page;
    }

private:
    std::shared_ptr<MySqlClient> client_;
};

class MySqlRewardTransactionRepository final : public mf::game::RewardTransactionRepository {
public:
    explicit MySqlRewardTransactionRepository(std::shared_ptr<MySqlClient> client)
        : client_(std::move(client)) {}

    // 使用 INSERT IGNORE 抢占 grant_id；只有抢占成功的事务才会修改玩家资产。
    mf::game::RewardGrantTransactionResult grant_once(
        const std::string& player_id,
        const std::string& grant_id,
        const std::string& source_type,
        const std::string& source_id,
        const std::vector<mf::game::RewardEntry>& rewards) override {
        return client_->with_connection(__func__, "reward.grant_once", [&](mysql::any_connection& connection) {
            mysql::results control_result;
            connection.execute("START TRANSACTION", control_result);
            try {
                const auto granted_at = std::chrono::system_clock::now();
                const auto insert_statement = StatementCache::prepare(connection, 
                    "INSERT IGNORE INTO reward_grant(grant_id, player_id, source_type, source_id, rewards_json, granted_at) "
                    "VALUES (?, ?, ?, ?, CAST(? AS JSON), FROM_UNIXTIME(?))");
                mysql::results insert_result;
                connection.execute(
                    insert_statement.bind(
                        grant_id,
                        player_id,
                        source_type,
                        source_id,
                        encode_rewards_json(rewards),
                        to_unix_seconds(granted_at)),
                    insert_result);

                if (insert_result.affected_rows() == 0) {
                    const auto grant_statement = StatementCache::prepare(connection, 
                        "SELECT grant_id, player_id, source_type, source_id, CAST(rewards_json AS CHAR), "
                        "UNIX_TIMESTAMP(granted_at) FROM reward_grant WHERE grant_id = ?");
                    mysql::results grant_result;
                    connection.execute(grant_statement.bind(grant_id), grant_result);
                    const auto player_statement = StatementCache::prepare(connection, 
                        "SELECT player_id, account_id, nickname, gold, gem "
                        "FROM player_profile WHERE player_id = ?");
                    mysql::results player_result;
                    connection.execute(player_statement.bind(player_id), player_result);
                    connection.execute("COMMIT", control_result);
                    return mf::game::RewardGrantTransactionResult{
                        .grant = grant_result.rows().empty()
                                     ? std::nullopt
                                     : std::optional<mf::game::RewardGrantRecord>{row_to_reward_grant(grant_result.rows().at(0))},
                        .player = player_result.rows().empty()
                                      ? std::nullopt
                                      : std::optional<mf::game::PlayerProfile>{row_to_profile(connection, player_result.rows().at(0))},
                        .already_granted = true,
                    };
                }

                auto player = apply_rewards_in_transaction(connection, player_id, rewards);
                if (!player) {
                    connection.execute("ROLLBACK", control_result);
                    return mf::game::RewardGrantTransactionResult{};
                }
                connection.execute("COMMIT", control_result);
                return mf::game::RewardGrantTransactionResult{
                    .grant = mf::game::RewardGrantRecord{
                        .grant_id = grant_id,
                        .player_id = player_id,
                        .source_type = source_type,
                        .source_id = source_id,
                        .rewards = rewards,
                        .granted_at = granted_at,
                    },
                    .player = std::move(player),
                };
            } catch (...) {
                try { connection.execute("ROLLBACK", control_result); } catch (...) {}
                throw;
            }
        });
    }

private:
    std::shared_ptr<MySqlClient> client_;
};

class MySqlLootTransactionRepository final : public mf::game::LootTransactionRepository {
public:
    explicit MySqlLootTransactionRepository(std::shared_ptr<MySqlClient> client)
        : client_(std::move(client)) {}

    // 锁住 loot_drop 后再发奖；任何一步失败都会回滚，掉落不会提前消失。
    mf::game::LootPickupTransactionResult pickup_once(
        const std::string& player_id,
        const std::string& drop_id,
        std::chrono::system_clock::time_point now) override {
        return client_->with_connection(__func__, "loot.pickup_once", [&](mysql::any_connection& connection) {
            mysql::results control_result;
            connection.execute("START TRANSACTION", control_result);
            try {
                const auto drop_statement = StatementCache::prepare(connection, 
                    "SELECT drop_id, owner_player_id, scene_id, CAST(rewards_json AS CHAR), picked, "
                    "picked_by_player_id, UNIX_TIMESTAMP(spawned_at), UNIX_TIMESTAMP(expires_at), "
                    "UNIX_TIMESTAMP(picked_at) FROM loot_drop WHERE drop_id = ? FOR UPDATE");
                mysql::results drop_result;
                connection.execute(drop_statement.bind(drop_id), drop_result);
                if (drop_result.rows().empty()) {
                    connection.execute("ROLLBACK", control_result);
                    return mf::game::LootPickupTransactionResult{.status = mf::game::LootPickupStatus::not_found};
                }
                auto drop = row_to_loot_drop(drop_result.rows().at(0));
                if (drop.picked && drop.picked_by_player_id != player_id) {
                    connection.execute("COMMIT", control_result);
                    return mf::game::LootPickupTransactionResult{.status = mf::game::LootPickupStatus::already_picked_by_other};
                }
                if (!drop.owner_player_id.empty() && drop.owner_player_id != player_id) {
                    connection.execute("ROLLBACK", control_result);
                    return mf::game::LootPickupTransactionResult{.status = mf::game::LootPickupStatus::not_owner};
                }
                if (drop.expires_at && *drop.expires_at < now) {
                    connection.execute("ROLLBACK", control_result);
                    return mf::game::LootPickupTransactionResult{.status = mf::game::LootPickupStatus::expired};
                }

                const auto grant_id = "loot_" + drop_id;
                std::optional<mf::game::RewardGrantRecord> grant;
                std::optional<mf::game::PlayerProfile> player;
                bool already_granted = false;
                const auto insert_statement = StatementCache::prepare(connection, 
                    "INSERT IGNORE INTO reward_grant(grant_id, player_id, source_type, source_id, rewards_json, granted_at) "
                    "VALUES (?, ?, 'loot', ?, CAST(? AS JSON), FROM_UNIXTIME(?))");
                mysql::results insert_result;
                connection.execute(
                    insert_statement.bind(
                        grant_id,
                        player_id,
                        drop_id,
                        encode_rewards_json(drop.rewards),
                        to_unix_seconds(now)),
                    insert_result);
                if (insert_result.affected_rows() > 0) {
                    player = apply_rewards_in_transaction(connection, player_id, drop.rewards);
                    if (!player) {
                        connection.execute("ROLLBACK", control_result);
                        return mf::game::LootPickupTransactionResult{.status = mf::game::LootPickupStatus::player_not_found};
                    }
                    grant = mf::game::RewardGrantRecord{
                        .grant_id = grant_id,
                        .player_id = player_id,
                        .source_type = "loot",
                        .source_id = drop_id,
                        .rewards = drop.rewards,
                        .granted_at = now,
                    };
                } else {
                    already_granted = true;
                    const auto grant_statement = StatementCache::prepare(connection, 
                        "SELECT grant_id, player_id, source_type, source_id, CAST(rewards_json AS CHAR), "
                        "UNIX_TIMESTAMP(granted_at) FROM reward_grant WHERE grant_id = ?");
                    mysql::results grant_result;
                    connection.execute(grant_statement.bind(grant_id), grant_result);
                    if (!grant_result.rows().empty()) grant = row_to_reward_grant(grant_result.rows().at(0));
                    const auto player_statement = StatementCache::prepare(connection, 
                        "SELECT player_id, account_id, nickname, gold, gem "
                        "FROM player_profile WHERE player_id = ?");
                    mysql::results player_result;
                    connection.execute(player_statement.bind(player_id), player_result);
                    if (!player_result.rows().empty()) player = row_to_profile(connection, player_result.rows().at(0));
                }

                const auto update_statement = StatementCache::prepare(connection, 
                    "UPDATE loot_drop SET picked = 1, picked_by_player_id = ?, picked_at = FROM_UNIXTIME(?) "
                    "WHERE drop_id = ?");
                mysql::results update_result;
                connection.execute(update_statement.bind(player_id, to_unix_seconds(now), drop_id), update_result);
                drop.picked = true;
                drop.picked_by_player_id = player_id;
                drop.picked_at = now;
                connection.execute("COMMIT", control_result);
                return mf::game::LootPickupTransactionResult{
                    .status = already_granted
                                  ? mf::game::LootPickupStatus::already_picked_by_self
                                  : mf::game::LootPickupStatus::picked,
                    .value = (grant && player)
                                 ? std::optional<mf::game::LootPickupResult>{mf::game::LootPickupResult{
                                       .drop = std::move(drop),
                                       .grant = std::move(grant),
                                       .player = std::move(*player),
                                   }}
                                 : std::nullopt,
                };
            } catch (...) {
                try { connection.execute("ROLLBACK", control_result); } catch (...) {}
                throw;
            }
        });
    }

private:
    std::shared_ptr<MySqlClient> client_;
};

class MySqlShopTransactionRepository final : public mf::game::ShopTransactionRepository {
public:
    explicit MySqlShopTransactionRepository(std::shared_ptr<MySqlClient> client)
        : client_(std::move(client)) {}

    // 原子购买：占限购名额、扣货币、发奖励、写流水都在同一个事务内完成。
    // 任何一步失败都会 ROLLBACK，避免“扣了钱没发货”或“并发突破限购”。
    mf::game::ShopPurchaseTransactionResult purchase_once(
        const std::string& purchase_id,
        const std::string& player_id,
        const mf::game::ShopProduct& product,
        std::chrono::system_clock::time_point purchased_at) override {
        return client_->with_connection(__func__, "shop.purchase_once", [&](mysql::any_connection& connection) {
            mysql::results control_result;
            connection.execute("START TRANSACTION", control_result);
            try {
                mf::game::ShopPurchaseTransactionResult result;

                // 1. 原子占限购名额：单条 INSERT ... ON DUPLICATE KEY UPDATE 在行锁下完成判断和自增。
                if (product.purchase_limit > 0) {
                    const auto claim_statement = StatementCache::prepare(connection,
                        "INSERT INTO shop_purchase_limit(player_id, product_id, purchase_count) "
                        "VALUES (?, ?, 1) "
                        "ON DUPLICATE KEY UPDATE purchase_count = IF(purchase_count < ?, purchase_count + 1, purchase_count)");
                    mysql::results claim_result;
                    connection.execute(
                        claim_statement.bind(player_id, product.product_id, product.purchase_limit),
                        claim_result);
                    if (claim_result.affected_rows() == 0) {
                        result.limit_reached = true;
                        const auto count_statement = StatementCache::prepare(connection,
                            "SELECT purchase_count FROM shop_purchase_limit WHERE player_id = ? AND product_id = ?");
                        mysql::results count_result;
                        connection.execute(count_statement.bind(player_id, product.product_id), count_result);
                        if (!count_result.rows().empty()) {
                            result.purchased_count = column_i64(count_result.rows().at(0), 0);
                        }
                        connection.execute("ROLLBACK", control_result);
                        return result;
                    }
                }

                // 2. 条件原子扣货币；affected_rows==0 即余额不足。
                if (product.price_amount > 0) {
                    mysql::results spend_result;
                    if (product.price_currency_id == "gold") {
                        const auto statement = StatementCache::prepare(connection,
                            "UPDATE player_profile SET gold = gold - ?, updated_at = CURRENT_TIMESTAMP "
                            "WHERE player_id = ? AND gold >= ?");
                        connection.execute(statement.bind(product.price_amount, player_id, product.price_amount), spend_result);
                    } else if (product.price_currency_id == "gem") {
                        const auto statement = StatementCache::prepare(connection,
                            "UPDATE player_profile SET gem = gem - ?, updated_at = CURRENT_TIMESTAMP "
                            "WHERE player_id = ? AND gem >= ?");
                        connection.execute(statement.bind(product.price_amount, player_id, product.price_amount), spend_result);
                    } else {
                        const auto statement = StatementCache::prepare(connection,
                            "UPDATE player_currency SET amount = amount - ? "
                            "WHERE player_id = ? AND currency_id = ? AND amount >= ?");
                        connection.execute(
                            statement.bind(product.price_amount, player_id, product.price_currency_id, product.price_amount),
                            spend_result);
                    }
                    if (spend_result.affected_rows() == 0) {
                        result.currency_not_enough = true;
                        connection.execute("ROLLBACK", control_result);
                        return result;
                    }
                }

                // 3. 幂等发奖：INSERT IGNORE 抢占 grant_id，抢占成功才改玩家资产。
                const auto insert_grant_statement = StatementCache::prepare(connection,
                    "INSERT IGNORE INTO reward_grant(grant_id, player_id, source_type, source_id, rewards_json, granted_at) "
                    "VALUES (?, ?, 'shop', ?, CAST(? AS JSON), FROM_UNIXTIME(?))");
                mysql::results insert_grant_result;
                connection.execute(
                    insert_grant_statement.bind(
                        purchase_id,
                        player_id,
                        product.product_id,
                        encode_rewards_json(product.rewards),
                        to_unix_seconds(purchased_at)),
                    insert_grant_result);
                std::optional<mf::game::RewardGrantRecord> grant;
                if (insert_grant_result.affected_rows() > 0) {
                    auto player = apply_rewards_in_transaction(connection, player_id, product.rewards);
                    if (!player) {
                        result.player_not_found = true;
                        connection.execute("ROLLBACK", control_result);
                        return result;
                    }
                    result.player = std::move(player);
                    grant = mf::game::RewardGrantRecord{
                        .grant_id = purchase_id,
                        .player_id = player_id,
                        .source_type = "shop",
                        .source_id = product.product_id,
                        .rewards = product.rewards,
                        .granted_at = purchased_at,
                    };
                } else {
                    const auto grant_statement = StatementCache::prepare(connection,
                        "SELECT grant_id, player_id, source_type, source_id, CAST(rewards_json AS CHAR), "
                        "UNIX_TIMESTAMP(granted_at) FROM reward_grant WHERE grant_id = ?");
                    mysql::results grant_result;
                    connection.execute(grant_statement.bind(purchase_id), grant_result);
                    if (!grant_result.rows().empty()) {
                        grant = row_to_reward_grant(grant_result.rows().at(0));
                    }
                    const auto player_statement = StatementCache::prepare(connection,
                        "SELECT player_id, account_id, nickname, gold, gem FROM player_profile WHERE player_id = ?");
                    mysql::results player_result;
                    connection.execute(player_statement.bind(player_id), player_result);
                    if (player_result.rows().empty()) {
                        result.player_not_found = true;
                        connection.execute("ROLLBACK", control_result);
                        return result;
                    }
                    result.player = row_to_profile(connection, player_result.rows().at(0));
                }

                // 4. 写购买流水。
                const auto insert_purchase_statement = StatementCache::prepare(connection,
                    "INSERT INTO shop_purchase(purchase_id, player_id, product_id, price_currency_id, "
                    "price_amount, rewards_json, purchased_at) "
                    "VALUES (?, ?, ?, ?, ?, CAST(? AS JSON), FROM_UNIXTIME(?))");
                mysql::results insert_purchase_result;
                connection.execute(
                    insert_purchase_statement.bind(
                        purchase_id,
                        player_id,
                        product.product_id,
                        product.price_currency_id,
                        product.price_amount,
                        encode_rewards_json(product.rewards),
                        to_unix_seconds(purchased_at)),
                    insert_purchase_result);

                result.purchase = mf::game::ShopPurchaseRecord{
                    .purchase_id = purchase_id,
                    .player_id = player_id,
                    .product_id = product.product_id,
                    .price_currency_id = product.price_currency_id,
                    .price_amount = product.price_amount,
                    .rewards = product.rewards,
                    .purchased_at = purchased_at,
                };
                result.grant = std::move(grant);

                connection.execute("COMMIT", control_result);
                return result;
            } catch (...) {
                try { connection.execute("ROLLBACK", control_result); } catch (...) {}
                throw;
            }
        });
    }

private:
    std::shared_ptr<MySqlClient> client_;
};

class MySqlFriendRepository final : public mf::game::FriendRepository {
public:
    explicit MySqlFriendRepository(std::shared_ptr<MySqlClient> client)
        : client_(std::move(client)) {}

    bool add_friendship(const std::string& player_id, const std::string& friend_player_id) override {
        if (player_id == friend_player_id) {
            return false;
        }

        return client_->with_connection(__func__, [&](mysql::any_connection& connection) {
            mysql::results result;

            connection.execute("START TRANSACTION", result);
            try {
                mysql::results exists_result;
                connection.execute(
                    statements_.friend_exists(connection).bind(
                        player_id,
                        friend_player_id,
                        friend_player_id,
                        player_id),
                    exists_result);
                if (!exists_result.rows().empty()) {
                    connection.execute("ROLLBACK", result);
                    return false;
                }

                // 关系库版本仍然保留双向两行记录，
                // 这样 list_friends(player_id) 只查自己的出边即可，查询逻辑更直观。
                connection.execute(
                    statements_.friend_insert(connection).bind(player_id, friend_player_id),
                    result);
                connection.execute(
                    statements_.friend_insert(connection).bind(friend_player_id, player_id),
                    result);
                connection.execute("COMMIT", result);
                return true;
            } catch (...) {
                try {
                    connection.execute("ROLLBACK", result);
                } catch (...) {
                }
                throw;
            }
        });
    }

    std::vector<mf::game::FriendLink> list_friends(const std::string& player_id) const override {
        std::vector<mf::game::FriendLink> links;

        client_->with_connection(__func__, "admin_task.list_filtered", [&](mysql::any_connection& connection) {
            mysql::results result;
            connection.execute(statements_.friend_list(connection).bind(player_id), result);
            links.reserve(result.rows().size());
            for (const auto row : result.rows()) {
                links.push_back(mf::game::FriendLink{
                    .player_id = column_string(row, 0),
                    .friend_player_id = column_string(row, 1),
                });
            }
        });

        return links;
    }

    std::vector<mf::game::FriendLink> list_friends_by_last_message(
        const std::string& player_id,
        std::size_t limit,
        std::size_t offset,
        const std::optional<mf::game::FriendPageCursor>& cursor) const override {
        std::vector<mf::game::FriendLink> links;

        client_->with_connection(__func__, "social.friends_by_last_message", [&](mysql::any_connection& connection) {
            mysql::results result;
            if (cursor) {
                connection.execute(
                    statements_.friend_list_by_last_message_cursor(connection).bind(
                        player_id,
                        cursor->last_sent_at,
                        cursor->last_sent_at,
                        cursor->last_friend_player_id,
                        static_cast<std::uint64_t>(limit)),
                    result);
            } else {
                connection.execute(
                    statements_.friend_list_by_last_message(connection).bind(
                        player_id,
                        static_cast<std::uint64_t>(limit),
                        static_cast<std::uint64_t>(offset)),
                    result);
            }
            links.reserve(result.rows().size());
            for (const auto row : result.rows()) {
                links.push_back(mf::game::FriendLink{
                    .player_id = column_string(row, 0),
                    .friend_player_id = column_string(row, 1),
                    .friend_nickname = column_string(row, 3),
                    .last_sent_at = column_i64(row, 2),
                });
            }
        });

        return links;
    }

    bool send_friend_request(const std::string& requester_player_id, const std::string& target_player_id) override {
        if (requester_player_id == target_player_id) {
            return false;
        }

        return client_->with_connection(__func__, [&](mysql::any_connection& connection) {
            mysql::results result;
            connection.execute("START TRANSACTION", result);
            try {
                mysql::results exists_result;
                connection.execute(
                    statements_.friend_request_exists(connection).bind(
                        requester_player_id,
                        target_player_id,
                        target_player_id,
                        requester_player_id),
                    exists_result);
                if (!exists_result.rows().empty()) {
                    connection.execute("ROLLBACK", result);
                    return false;
                }

                connection.execute(
                    statements_.friend_request_insert(connection).bind(
                        requester_player_id,
                        target_player_id,
                        "pending_outgoing"),
                    result);
                connection.execute(
                    statements_.friend_request_insert(connection).bind(
                        target_player_id,
                        requester_player_id,
                        "pending_incoming"),
                    result);
                connection.execute("COMMIT", result);
                return true;
            } catch (...) {
                try {
                    connection.execute("ROLLBACK", result);
                } catch (...) {
                }
                throw;
            }
        });
    }

    std::vector<mf::game::FriendRequest> list_incoming_requests(const std::string& player_id) const override {
        std::vector<mf::game::FriendRequest> requests;
        client_->with_connection(__func__, "admin_task.count_filtered", [&](mysql::any_connection& connection) {
            mysql::results result;
            connection.execute(statements_.friend_request_list_incoming(connection).bind(player_id), result);
            requests.reserve(result.rows().size());
            for (const auto row : result.rows()) {
                requests.push_back(row_to_friend_request(row));
            }
        });
        return requests;
    }

    std::vector<mf::game::FriendRequest> list_outgoing_requests(const std::string& player_id) const override {
        std::vector<mf::game::FriendRequest> requests;
        client_->with_connection(__func__, "admin_task.find_by_id", [&](mysql::any_connection& connection) {
            mysql::results result;
            connection.execute(statements_.friend_request_list_outgoing(connection).bind(player_id), result);
            requests.reserve(result.rows().size());
            for (const auto row : result.rows()) {
                requests.push_back(row_to_friend_request(row));
            }
        });
        return requests;
    }

    bool respond_friend_request(
        const std::string& requester_player_id,
        const std::string& target_player_id,
        bool accept) override {
        return client_->with_connection(__func__, [&](mysql::any_connection& connection) {
            mysql::results result;
            connection.execute("START TRANSACTION", result);
            try {
                if (accept) {
                    connection.execute(
                        statements_.friend_request_update_status(connection).bind(
                            "accepted",
                            requester_player_id,
                            target_player_id,
                            target_player_id,
                            requester_player_id),
                        result);
                } else {
                    connection.execute(
                        statements_.friend_request_delete_pair(connection).bind(
                            requester_player_id,
                            target_player_id,
                            target_player_id,
                            requester_player_id),
                        result);
                }
                connection.execute("COMMIT", result);
                return true;
            } catch (...) {
                try {
                    connection.execute("ROLLBACK", result);
                } catch (...) {
                }
                throw;
            }
        });
    }

    bool set_chat_block(
        const std::string& player_id,
        const std::string& blocked_player_id,
        bool blocked) override {
        return client_->with_connection(__func__, [&](mysql::any_connection& connection) {
            mysql::results result;
            if (blocked) {
                connection.execute(
                    statements_.chat_block_upsert(connection).bind(player_id, blocked_player_id),
                    result);
            } else {
                connection.execute(
                    statements_.chat_block_delete(connection).bind(player_id, blocked_player_id),
                    result);
            }
            return true;
        });
    }

    bool is_chat_blocked(const std::string& player_id, const std::string& blocked_player_id) const override {
        bool blocked = false;
        client_->with_connection(__func__, "reward_grant.list_filtered", [&](mysql::any_connection& connection) {
            mysql::results result;
            connection.execute(
                statements_.chat_block_exists(connection).bind(player_id, blocked_player_id),
                result);
            blocked = !result.rows().empty();
        });
        return blocked;
    }

    std::vector<mf::game::ChatBlockEntry> list_chat_blocks(const std::string& player_id) const override {
        std::vector<mf::game::ChatBlockEntry> blocks;
        client_->with_connection(__func__, "mail.list_filtered", [&](mysql::any_connection& connection) {
            mysql::results result;
            connection.execute(statements_.chat_block_list(connection).bind(player_id), result);
            blocks.reserve(result.rows().size());
            for (const auto row : result.rows()) {
                blocks.push_back(row_to_chat_block(row));
            }
        });
        return blocks;
    }

private:
    std::shared_ptr<MySqlClient> client_;
    mutable StatementCache statements_;
};

class MySqlChatRepository final : public mf::game::ChatRepository {
    void append_messages_on_connection(
        mysql::any_connection& connection,
        const std::vector<mf::game::ChatMessage>& messages,
        mysql::results& result);

public:
    explicit MySqlChatRepository(std::shared_ptr<MySqlClient> client)
        : client_(std::move(client)) {}

    bool supports_outbox() const override { return true; }

    mf::game::ChatMessage append_to_outbox(mf::game::ChatMessage message) override {
        if (message.sent_at.time_since_epoch().count() == 0) {
            message.sent_at = std::chrono::system_clock::now();
        }
        if (message.message_id.empty()) {
            message.message_id = make_chat_message_id(message);
        }

        client_->with_connection(__func__, "chat.outbox_insert", [&](mysql::any_connection& connection) {
            mysql::results result;
            connection.execute(
                statements_.chat_outbox_insert(connection).bind(
                    message.message_id,
                    message.channel.channel_id,
                    std::string(mf::chat::to_string(message.channel.kind)),
                    message.channel.scope_id,
                    std::string(mf::chat::to_string(message.kind)),
                    message.sender_player_id,
                    message.sender_nickname,
                    message.text,
                    to_unix_seconds(message.sent_at)),
                result);
        });
        return message;
    }

    mf::game::ChatMessage append(mf::game::ChatMessage message) override {
        if (message.sent_at.time_since_epoch().count() == 0) {
            message.sent_at = std::chrono::system_clock::now();
        }
        if (message.message_id.empty()) {
            message.message_id = make_chat_message_id(message);
        }

        client_->with_connection(__func__, "admin_task.claim_due", [&](mysql::any_connection& connection) {
            mysql::results result;
            append_messages_on_connection(connection, {message}, result);
        });

        return message;
    }

    std::vector<mf::game::ChatMessage> append_batch(
        std::vector<mf::game::ChatMessage> messages) override {
        if (messages.empty()) {
            return messages;
        }
        for (auto& message : messages) {
            if (message.sent_at.time_since_epoch().count() == 0) {
                message.sent_at = std::chrono::system_clock::now();
            }
            if (message.message_id.empty()) {
                message.message_id = make_chat_message_id(message);
            }
        }

        client_->with_connection(__func__, "chat.append_batch", [&](mysql::any_connection& connection) {
            mysql::results result;
            connection.execute("START TRANSACTION", result);
            try {
                append_messages_on_connection(connection, messages, result);
                connection.execute("COMMIT", result);
            } catch (...) {
                try { connection.execute("ROLLBACK", result); } catch (...) {}
                throw;
            }
        });
        return messages;
    }

    mf::game::ChatOutboxBatchResult drain_outbox_batch(std::size_t limit) override {
        mf::game::ChatOutboxBatchResult result_batch;
        client_->with_connection(__func__, "chat.outbox_drain", [&](mysql::any_connection& connection) {
            mysql::results result;
            connection.execute("START TRANSACTION", result);
            try {
            connection.execute(
                statements_.chat_outbox_list(connection).bind(static_cast<std::uint64_t>(limit)),
                result);
            result_batch.messages.reserve(result.rows().size());
            for (const auto row : result.rows()) {
                result_batch.messages.push_back(row_to_message(row));
            }
                if (!result_batch.messages.empty()) {
                    append_messages_on_connection(connection, result_batch.messages, result);
                    for (const auto& message : result_batch.messages) {
                        connection.execute(
                            statements_.chat_outbox_delete(connection).bind(message.message_id),
                            result);
                    }
                }
                mysql::results health_result;
                connection.execute(statements_.chat_outbox_health(connection).bind(), health_result);
                if (!health_result.rows().empty()) {
                    const auto row = health_result.rows().at(0);
                    result_batch.pending_count = static_cast<std::size_t>(column_i64(row, 0));
                    result_batch.failed_count = static_cast<std::size_t>(column_i64(row, 1));
                    if (const auto oldest_created_at = optional_time_point_from_row(row, 2)) {
                        result_batch.oldest_pending_age_seconds = std::max<std::int64_t>(
                            0,
                            to_unix_seconds(std::chrono::system_clock::now()) -
                                to_unix_seconds(*oldest_created_at));
                    }
                }
                connection.execute("COMMIT", result);
            } catch (const std::exception& error) {
                try { connection.execute("ROLLBACK", result); } catch (...) {}
                if (!result_batch.messages.empty()) {
                    try {
                        connection.execute("START TRANSACTION", result);
                        auto error_text = std::string{error.what()};
                        if (error_text.size() > 512) {
                            error_text.resize(512);
                        }
                        for (const auto& message : result_batch.messages) {
                            connection.execute(
                                statements_.chat_outbox_fail(connection).bind(error_text, message.message_id),
                                result);
                        }
                        connection.execute("COMMIT", result);
                    } catch (...) {
                        try { connection.execute("ROLLBACK", result); } catch (...) {}
                    }
                }
                throw;
            } catch (...) {
                try { connection.execute("ROLLBACK", result); } catch (...) {}
                throw;
            }
        });
        return result_batch;
    }

    mf::game::ChatOutboxHealth outbox_health() const override {
        mf::game::ChatOutboxHealth health;
        client_->with_connection(__func__, "chat.outbox_health", [&](mysql::any_connection& connection) {
            mysql::results result;
            connection.execute(statements_.chat_outbox_health(connection).bind(), result);
            if (result.rows().empty()) {
                return;
            }
            const auto row = result.rows().at(0);
            health.pending_count = static_cast<std::size_t>(column_i64(row, 0));
            health.failed_count = static_cast<std::size_t>(column_i64(row, 1));
            const auto oldest_created_at = optional_time_point_from_row(row, 2);
            if (oldest_created_at) {
                health.oldest_pending_age_seconds = std::max<std::int64_t>(
                    0,
                    to_unix_seconds(std::chrono::system_clock::now()) -
                        to_unix_seconds(*oldest_created_at));
            }
        });
        return health;
    }

    std::size_t retry_failed_outbox(std::size_t limit) override {
        std::size_t retried = 0;
        client_->with_connection(__func__, "chat.outbox_retry_failed", [&](mysql::any_connection& connection) {
            mysql::results result;
            connection.execute(
                statements_.chat_outbox_retry_failed(connection).bind(static_cast<std::uint64_t>(limit)),
                result);
            retried = static_cast<std::size_t>(result.affected_rows());
        });
        return retried;
    }

    std::optional<mf::game::ChatMessage> recall(
        const std::string& channel_id,
        const std::string& message_id,
        const std::string& recalled_by_player_id) override {
        client_->with_connection(__func__, "admin_task.mark_completed", [&](mysql::any_connection& connection) {
            mysql::results result;
            connection.execute(
                statements_.chat_recall(connection).bind(
                    recalled_by_player_id,
                    channel_id,
                    message_id,
                    recalled_by_player_id),
                result);
        });

        std::optional<mf::game::ChatMessage> message;
        client_->with_connection(__func__, "admin_task.mark_failed", [&](mysql::any_connection& connection) {
            mysql::results result;
            connection.execute(
                statements_.chat_find_by_id(connection).bind(channel_id, message_id),
                result);
            if (!result.rows().empty()) {
                message = row_to_message(result.rows().at(0));
            }
        });
        return message;
    }

    std::vector<mf::game::ChatMessage> recent_messages(const std::string& channel_id, std::size_t limit) const override {
        std::vector<mf::game::ChatMessage> messages;

        client_->with_connection(__func__, [&](mysql::any_connection& connection) {
            mysql::results result;
            connection.execute(
                statements_.chat_recent(connection).bind(channel_id, static_cast<std::uint64_t>(limit)),
                result);

            messages.reserve(result.rows().size());
            for (const auto row : result.rows()) {
                messages.push_back(row_to_message(row));
            }
        });

        // SQL 这里按 sent_at DESC 拉最近 N 条，最后再反转回“旧 -> 新”，
        // 这样客户端拿到的顺序和内存实现保持一致。
        std::reverse(messages.begin(), messages.end());
        return messages;
    }

    mf::game::ChatHistoryPage history_page(
        const std::string& channel_id,
        std::string_view cursor,
        std::size_t limit) const override {
        mf::game::ChatHistoryPage page;
        const auto bounded_limit = std::clamp<std::size_t>(limit, 1, 100);
        const auto fetch_limit = bounded_limit + 1;

        std::optional<std::int64_t> cursor_sent_at;
        std::string cursor_message_id;
        if (!cursor.empty()) {
            const auto separator = cursor.find('-');
            if (separator == std::string_view::npos) {
                return page;
            }
            try {
                cursor_sent_at = std::stoll(std::string(cursor.substr(0, separator)));
            } catch (const std::exception&) {
                return page;
            }
            cursor_message_id = std::string(cursor.substr(separator + 1));
            if (cursor_message_id.empty()) {
                return page;
            }
        }

        std::vector<mf::game::ChatMessage> messages;
        client_->with_connection(__func__, [&](mysql::any_connection& connection) {
            mysql::results result;
            if (cursor_sent_at) {
                connection.execute(
                    statements_.chat_history_page_next(connection).bind(
                        channel_id,
                        static_cast<std::uint64_t>(*cursor_sent_at),
                        static_cast<std::uint64_t>(*cursor_sent_at),
                        cursor_message_id,
                        static_cast<std::uint64_t>(fetch_limit)),
                    result);
            } else {
                connection.execute(
                    statements_.chat_history_page_first(connection).bind(
                        channel_id, static_cast<std::uint64_t>(fetch_limit)),
                    result);
            }

            messages.reserve(result.rows().size());
            for (const auto row : result.rows()) {
                messages.push_back(row_to_message(row));
            }
        });

        if (messages.size() > bounded_limit) {
            messages.resize(bounded_limit);
            page.has_more = true;
            const auto& oldest = messages.back();
            page.next_cursor =
                std::to_string(to_unix_seconds(oldest.sent_at)) + "-" + oldest.message_id;
        }

        // 与 recent_messages 保持一致：返回“旧 -> 新”。
        std::reverse(messages.begin(), messages.end());
        page.messages = std::move(messages);
        return page;
    }

    std::vector<mf::game::ChatMessage> latest_messages(
        const std::vector<std::string>& channel_ids,
        std::size_t limit) const override {
        std::vector<mf::game::ChatMessage> messages;
        if (channel_ids.empty()) {
            return messages;
        }

        boost::json::array wanted_channels;
        wanted_channels.reserve(channel_ids.size());
        for (const auto& channel_id : channel_ids) {
            wanted_channels.emplace_back(channel_id);
        }
        const auto wanted_channels_json = boost::json::serialize(wanted_channels);
        const auto bounded_limit = std::max<std::size_t>(1, limit);

        client_->with_connection(__func__, "chat.latest_messages", [&](mysql::any_connection& connection) {
            mysql::results result;
            if (bounded_limit == 1) {
                // 好友会话列表只需要每个频道最新的一条；按频道做有索引的 LIMIT 1，
                // 避免 ROW_NUMBER() 为这些频道的全部历史消息排序并编号。
                const auto statement = StatementCache::prepare(
                    connection,
                    "SELECT cm.message_id, cm.channel_id, cm.channel_kind, cm.scope_id, cm.message_kind, "
                    "cm.sender_player_id, cm.sender_nickname, cm.text_body, cm.recalled, "
                    "cm.recalled_by_player_id, UNIX_TIMESTAMP(cm.recalled_at), UNIX_TIMESTAMP(cm.sent_at) "
                    "FROM JSON_TABLE(?, '$[*]' COLUMNS(channel_id VARCHAR(128) PATH '$')) AS ids "
                    "JOIN chat_message cm ON cm.message_id = ("
                    "  SELECT newest.message_id FROM chat_message newest "
                    "  WHERE newest.channel_id = ids.channel_id COLLATE utf8mb4_0900_ai_ci "
                    "  ORDER BY newest.sent_at DESC, newest.message_id DESC LIMIT 1"
                    ")");
                connection.execute(statement.bind(wanted_channels_json), result);
            } else {
                // 通用多条查询仍需为每个频道返回最近 N 条消息。
                const auto statement = StatementCache::prepare(
                    connection,
                    "SELECT message_id, channel_id, channel_kind, scope_id, message_kind, "
                    "sender_player_id, sender_nickname, text_body, recalled, recalled_by_player_id, "
                    "UNIX_TIMESTAMP(recalled_at), UNIX_TIMESTAMP(sent_at) "
                    "FROM ("
                    "  SELECT cm.*, ROW_NUMBER() OVER ("
                    "    PARTITION BY cm.channel_id ORDER BY cm.sent_at DESC, cm.message_id DESC"
                    "  ) AS rn "
                    "  FROM chat_message cm "
                    "  JOIN JSON_TABLE(?, '$[*]' COLUMNS(channel_id VARCHAR(128) PATH '$')) AS ids "
                    "    ON ids.channel_id = cm.channel_id COLLATE utf8mb4_0900_ai_ci"
                    ") ranked WHERE ranked.rn <= ?");
                connection.execute(
                    statement.bind(wanted_channels_json, static_cast<std::uint64_t>(bounded_limit)),
                    result);
            }
            messages.reserve(result.rows().size());
            for (const auto row : result.rows()) {
                messages.push_back(row_to_message(row));
            }
        });

        return messages;
    }

private:
    std::shared_ptr<MySqlClient> client_;
    mutable StatementCache statements_;
};

class MySqlChatModerationRepository final : public mf::game::ChatModerationRepository {
public:
    explicit MySqlChatModerationRepository(std::shared_ptr<MySqlClient> client)
        : client_(std::move(client)) {}

    mf::game::ChatMuteEntry mute_player(
        const std::string& channel_id,
        const std::string& muted_player_id,
        const std::string& muted_by_player_id,
        std::string reason,
        std::optional<std::chrono::system_clock::time_point> muted_until) override {
        const auto effective_until = muted_until.value_or(std::chrono::system_clock::now() + std::chrono::hours{24 * 365});
        client_->with_connection(__func__, [&](mysql::any_connection& connection) {
            mysql::results result;
            connection.execute(
                statements_.chat_mute_upsert(connection).bind(
                    channel_id,
                    muted_player_id,
                    muted_by_player_id,
                    reason,
                    to_unix_seconds(effective_until)),
                result);
        });

        return mf::game::ChatMuteEntry{
            .channel_id = channel_id,
            .muted_player_id = muted_player_id,
            .muted_by_player_id = muted_by_player_id,
            .reason = std::move(reason),
            .muted_until = muted_until,
        };
    }

    void clear_mute(const std::string& channel_id, const std::string& muted_player_id) override {
        client_->with_connection(__func__, [&](mysql::any_connection& connection) {
            mysql::results result;
            connection.execute(statements_.chat_mute_clear(connection).bind(channel_id, muted_player_id), result);
        });
    }

    std::optional<mf::game::ChatMuteEntry> find_active_mute(
        const std::string& channel_id,
        const std::string& muted_player_id) const override {
        std::optional<mf::game::ChatMuteEntry> mute;
        client_->with_connection(__func__, [&](mysql::any_connection& connection) {
            mysql::results result;
            connection.execute(
                statements_.chat_mute_find_active(connection).bind(channel_id, muted_player_id),
                result);
            if (!result.rows().empty()) {
                mute = row_to_chat_mute(result.rows().at(0));
            }
        });
        return mute;
    }

private:
    std::shared_ptr<MySqlClient> client_;
    mutable StatementCache statements_;
};

void MySqlChatRepository::append_messages_on_connection(
    mysql::any_connection& connection,
    const std::vector<mf::game::ChatMessage>& messages,
    mysql::results& result) {
    for (const auto& message : messages) {
        connection.execute(
            statements_.chat_insert(connection).bind(
                message.message_id,
                message.channel.channel_id,
                std::string(mf::chat::to_string(message.channel.kind)),
                message.channel.scope_id,
                std::string(mf::chat::to_string(message.kind)),
                message.sender_player_id,
                message.sender_nickname,
                message.text,
                to_unix_seconds(message.sent_at)),
            result);

        if (message.channel.kind == mf::game::ChatChannelKind::friend_direct) {
            const auto separator = message.channel.scope_id.find(':');
            if (separator != std::string::npos) {
                const auto player_a = message.channel.scope_id.substr(0, separator);
                const auto player_b = message.channel.scope_id.substr(separator + 1);
                connection.execute(
                    statements_.friend_last_message_touch(connection).bind(
                        to_unix_seconds(message.sent_at),
                        to_unix_seconds(message.sent_at),
                        player_a,
                        player_b,
                        player_b,
                        player_a),
                    result);
            }
        }
    }
}

class MySqlVoiceRepository final : public mf::voice::VoiceRepository {
public:
    explicit MySqlVoiceRepository(std::shared_ptr<MySqlClient> client)
        : client_(std::move(client)) {}

    mf::voice::VoiceRoomMember upsert_member(mf::voice::VoiceRoomMember member) override {
        client_->with_connection(__func__, "voice.member_upsert", [&](mysql::any_connection& connection) {
            const auto statement = StatementCache::prepare(connection, 
                "INSERT INTO voice_room_member(room_id, scope_type, scope_id, player_id, display_name, member_role) "
                "VALUES (?, ?, ?, ?, ?, ?) ON DUPLICATE KEY UPDATE scope_type=VALUES(scope_type), "
                "scope_id=VALUES(scope_id), display_name=VALUES(display_name), member_role=VALUES(member_role)");
            mysql::results result;
            connection.execute(
                statement.bind(
                    member.room_id,
                    member.scope_type,
                    member.scope_id,
                    member.player_id,
                    member.display_name,
                    member.role),
                result);
        });
        return find_member(member.room_id, member.player_id).value_or(std::move(member));
    }

    std::optional<mf::voice::VoiceRoomMember> find_member(
        const std::string& room_id,
        const std::string& player_id) const override {
        std::optional<mf::voice::VoiceRoomMember> member;
        client_->with_connection(__func__, "voice.member_find", [&](mysql::any_connection& connection) {
            const auto statement = StatementCache::prepare(connection, 
                "SELECT scope_type, scope_id, room_id, player_id, display_name, member_role, "
                "is_muted_by_server, is_banned, UNIX_TIMESTAMP(joined_at), UNIX_TIMESTAMP(updated_at) "
                "FROM voice_room_member WHERE room_id=? AND player_id=?");
            mysql::results result;
            connection.execute(statement.bind(room_id, player_id), result);
            if (!result.rows().empty()) member = row_to_member(result.rows().at(0));
        });
        return member;
    }

    std::vector<mf::voice::VoiceRoomMember> list_members(const std::string& room_id) const override {
        std::vector<mf::voice::VoiceRoomMember> members;
        client_->with_connection(__func__, "voice.member_list", [&](mysql::any_connection& connection) {
            const auto statement = StatementCache::prepare(connection, 
                "SELECT scope_type, scope_id, room_id, player_id, display_name, member_role, "
                "is_muted_by_server, is_banned, UNIX_TIMESTAMP(joined_at), UNIX_TIMESTAMP(updated_at) "
                "FROM voice_room_member WHERE room_id=? ORDER BY joined_at, player_id");
            mysql::results result;
            connection.execute(statement.bind(room_id), result);
            for (const auto row : result.rows()) members.push_back(row_to_member(row));
        });
        return members;
    }

    bool remove_member(const std::string& room_id, const std::string& player_id) override {
        bool removed = false;
        client_->with_connection(__func__, "voice.member_remove", [&](mysql::any_connection& connection) {
            const auto statement = StatementCache::prepare(connection, 
                "DELETE FROM voice_room_member WHERE room_id=? AND player_id=?");
            mysql::results result;
            connection.execute(statement.bind(room_id, player_id), result);
            removed = result.affected_rows() > 0;
        });
        return removed;
    }

    std::optional<mf::voice::VoiceRoomMember> set_moderation(
        const std::string& room_id,
        const std::string& player_id,
        bool server_muted,
        bool banned) override {
        bool changed = false;
        client_->with_connection(__func__, "voice.member_moderate", [&](mysql::any_connection& connection) {
            const auto statement = StatementCache::prepare(connection, 
                "UPDATE voice_room_member SET is_muted_by_server=?, is_banned=? "
                "WHERE room_id=? AND player_id=?");
            mysql::results result;
            connection.execute(statement.bind(server_muted ? 1 : 0, banned ? 1 : 0, room_id, player_id), result);
            changed = result.affected_rows() > 0;
        });
        return changed ? find_member(room_id, player_id) : std::nullopt;
    }

    mf::voice::VoiceModerationEvent append_moderation_event(mf::voice::VoiceModerationEvent event) override {
        const auto created_at = std::chrono::system_clock::now();
        if (event.event_id.empty()) {
            static std::atomic<std::uint64_t> sequence{1};
            event.event_id = "voice_evt_" + make_hash_hex(
                event.room_id + '|' + event.target_player_id + '|' + event.operator_id + '|' +
                std::to_string(to_unix_seconds(created_at)) + '|' + event.action + '|' +
                std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));
        }
        client_->with_connection(__func__, "voice.moderation_append", [&](mysql::any_connection& connection) {
            const auto statement = StatementCache::prepare(connection, 
                "INSERT INTO voice_moderation_event(event_id, room_id, target_player_id, operator_id, "
                "action_name, reason, created_at) VALUES (?, ?, ?, ?, ?, ?, FROM_UNIXTIME(?))");
            mysql::results result;
            connection.execute(
                statement.bind(
                    event.event_id,
                    event.room_id,
                    event.target_player_id,
                    event.operator_id,
                    event.action,
                    event.reason,
                    to_unix_seconds(created_at)),
                result);
        });
        event.created_at = created_at;
        return event;
    }

private:
    static mf::voice::VoiceRoomMember row_to_member(const mysql::row_view& row) {
        return {
            .scope_type = column_string(row, 0),
            .scope_id = column_string(row, 1),
            .room_id = column_string(row, 2),
            .player_id = column_string(row, 3),
            .display_name = column_string(row, 4),
            .role = column_string(row, 5),
            .server_muted = column_i64(row, 6) != 0,
            .banned = column_i64(row, 7) != 0,
            .joined_at = optional_time_point_from_row(row, 8),
            .updated_at = optional_time_point_from_row(row, 9),
        };
    }

    std::shared_ptr<MySqlClient> client_;
};

class MySqlQuestRepository final : public mf::game::QuestRepository {
public:
    explicit MySqlQuestRepository(std::shared_ptr<MySqlClient> client)
        : client_(std::move(client)) {}

    // 新增或更新任务定义；玩家已有任务保留接受时的 quest_version。
    mf::game::QuestDefinition upsert_definition(mf::game::QuestDefinition definition) override {
        client_->with_connection(__func__, "quest.definition_upsert", [&](mysql::any_connection& connection) {
            const auto statement = StatementCache::prepare(connection, 
                "INSERT INTO quest_definition(quest_id, version, title, description_text, claim_mode, "
                "turn_in_npc_id, completion_flag, pre_quest_ids_json, required_flags_json, objectives_json, "
                "rewards_json, enabled) VALUES (?, ?, ?, ?, ?, ?, ?, CAST(? AS JSON), CAST(? AS JSON), "
                "CAST(? AS JSON), CAST(? AS JSON), ?) ON DUPLICATE KEY UPDATE version=VALUES(version), "
                "title=VALUES(title), description_text=VALUES(description_text), claim_mode=VALUES(claim_mode), "
                "turn_in_npc_id=VALUES(turn_in_npc_id), completion_flag=VALUES(completion_flag), "
                "pre_quest_ids_json=VALUES(pre_quest_ids_json), required_flags_json=VALUES(required_flags_json), "
                "objectives_json=VALUES(objectives_json), rewards_json=VALUES(rewards_json), enabled=VALUES(enabled)");
            mysql::results result;
            connection.execute(
                statement.bind(
                    definition.quest_id,
                    definition.version,
                    definition.title,
                    definition.description,
                    quest_claim_mode_to_string(definition.claim_mode),
                    definition.turn_in_npc_id,
                    definition.completion_flag,
                    encode_string_array(definition.pre_quest_ids),
                    encode_string_array(definition.required_flags),
                    encode_quest_objectives(definition.objectives),
                    encode_rewards_json(definition.rewards),
                    definition.enabled ? 1 : 0),
                result);
        });
        return find_definition(definition.quest_id).value_or(std::move(definition));
    }

    std::vector<mf::game::QuestDefinition> list_definitions(bool include_disabled) const override {
        std::vector<mf::game::QuestDefinition> definitions;
        client_->with_connection(__func__, "quest.definition_list", [&](mysql::any_connection& connection) {
            const auto statement = StatementCache::prepare(connection, 
                "SELECT quest_id, version, title, description_text, claim_mode, turn_in_npc_id, completion_flag, "
                "CAST(pre_quest_ids_json AS CHAR), CAST(required_flags_json AS CHAR), CAST(objectives_json AS CHAR), "
                "CAST(rewards_json AS CHAR), enabled, UNIX_TIMESTAMP(created_at), UNIX_TIMESTAMP(updated_at) "
                "FROM quest_definition WHERE (? = 1 OR enabled = 1) ORDER BY quest_id");
            mysql::results result;
            connection.execute(statement.bind(include_disabled ? 1 : 0), result);
            for (const auto row : result.rows()) definitions.push_back(row_to_quest_definition(row));
        });
        return definitions;
    }

    std::optional<mf::game::QuestDefinition> find_definition(const std::string& quest_id) const override {
        std::optional<mf::game::QuestDefinition> definition;
        client_->with_connection(__func__, "quest.definition_find", [&](mysql::any_connection& connection) {
            definition = find_definition_on(connection, quest_id, false);
        });
        return definition;
    }

    mf::game::QuestSnapshot get_snapshot(const std::string& player_id) const override {
        return client_->with_connection(__func__, "quest.snapshot", [&](mysql::any_connection& connection) {
            return load_snapshot(connection, player_id);
        });
    }

    // 接受任务时锁定定义并验证前置任务与剧情标记，重复接受直接返回已有状态。
    std::optional<mf::game::PlayerQuest> accept(
        const std::string& player_id,
        const std::string& quest_id,
        const std::string& request_id,
        std::string& failure_code) override {
        std::optional<mf::game::PlayerQuest> accepted;
        client_->with_connection(__func__, "quest.accept", [&](mysql::any_connection& connection) {
            mysql::results control;
            connection.execute("START TRANSACTION", control);
            try {
                const auto definition = find_definition_on(connection, quest_id, true);
                if (!definition || !definition->enabled) {
                    failure_code = "quest_not_found";
                    connection.execute("ROLLBACK", control);
                    return;
                }
                const auto existing = find_player_quest_on(connection, player_id, quest_id, true);
                if (existing) {
                    accepted = existing;
                    connection.execute("COMMIT", control);
                    return;
                }
                const auto request_statement = StatementCache::prepare(connection, 
                    "SELECT quest_id FROM player_quest WHERE player_id=? AND accept_request_id=? LIMIT 1");
                mysql::results request_result;
                connection.execute(request_statement.bind(player_id, request_id), request_result);
                if (!request_result.rows().empty()) {
                    failure_code = "quest_request_conflict";
                    connection.execute("ROLLBACK", control);
                    return;
                }
                if (!conditions_met(connection, player_id, *definition)) {
                    failure_code = "quest_condition_not_met";
                    connection.execute("ROLLBACK", control);
                    return;
                }
                const auto insert_quest = StatementCache::prepare(connection, 
                    "INSERT INTO player_quest(player_id, quest_id, quest_version, state, progress_version, "
                    "accept_request_id, accepted_at) VALUES (?, ?, ?, 'accepted', 0, ?, CURRENT_TIMESTAMP)");
                mysql::results result;
                connection.execute(insert_quest.bind(player_id, quest_id, definition->version, request_id), result);
                const auto insert_objective = StatementCache::prepare(connection, 
                    "INSERT INTO player_quest_objective(player_id, quest_id, objective_id, current_value, target_value) "
                    "VALUES (?, ?, ?, 0, ?)");
                for (const auto& objective : definition->objectives) {
                    connection.execute(
                        insert_objective.bind(player_id, quest_id, objective.objective_id, objective.target_value),
                        result);
                }
                accepted = find_player_quest_on(connection, player_id, quest_id, false);
                connection.execute("COMMIT", control);
            } catch (...) {
                try { connection.execute("ROLLBACK", control); } catch (...) {}
                throw;
            }
        });
        return accepted;
    }

    // event_id 先写去重表；只有第一次事件会推进与 event_type/target_id 匹配的目标。
    mf::game::QuestEventResult apply_event(
        const std::string& player_id,
        const std::string& event_id,
        const std::string& event_type,
        const std::string& target_id,
        std::int64_t amount,
        const std::string& source_id,
        std::string& failure_code) override {
        mf::game::QuestEventResult event_result;
        client_->with_connection(__func__, "quest.apply_event", [&](mysql::any_connection& connection) {
            mysql::results control;
            connection.execute("START TRANSACTION", control);
            try {
                const auto receipt_statement = StatementCache::prepare(connection, 
                    "INSERT IGNORE INTO quest_event_receipt(player_id, event_id, event_type, target_id, source_id, amount) "
                    "VALUES (?, ?, ?, ?, ?, ?)");
                mysql::results receipt_result;
                connection.execute(
                    receipt_statement.bind(player_id, event_id, event_type, target_id, source_id, amount),
                    receipt_result);
                if (receipt_result.affected_rows() == 0) {
                    event_result.duplicate_event = true;
                    event_result.snapshot = load_snapshot(connection, player_id);
                    connection.execute("COMMIT", control);
                    return;
                }

                const auto definitions = list_active_player_definitions(connection, player_id);
                bool matched = false;
                for (const auto& definition : definitions) {
                    bool quest_changed = false;
                    const auto update_objective = StatementCache::prepare(connection, 
                        "UPDATE player_quest_objective SET current_value = LEAST(target_value, current_value + ?) "
                        "WHERE player_id = ? AND quest_id = ? AND objective_id = ?");
                    for (const auto& objective : definition.objectives) {
                        if (objective.event_type != event_type ||
                            (!objective.target_id.empty() && objective.target_id != target_id)) {
                            continue;
                        }
                        mysql::results update_result;
                        connection.execute(
                            update_objective.bind(amount, player_id, definition.quest_id, objective.objective_id),
                            update_result);
                        matched = true;
                        quest_changed = true;
                    }
                    if (!quest_changed || !all_objectives_completed(connection, player_id, definition.quest_id)) {
                        if (quest_changed) increment_progress_version(connection, player_id, definition.quest_id);
                        continue;
                    }
                    increment_progress_version(connection, player_id, definition.quest_id);
                    const auto complete_statement = StatementCache::prepare(connection, 
                        "UPDATE player_quest SET state='completed', completed_at=COALESCE(completed_at, CURRENT_TIMESTAMP), "
                        "updated_at=CURRENT_TIMESTAMP WHERE player_id=? AND quest_id=? AND state='accepted'");
                    mysql::results complete_result;
                    connection.execute(complete_statement.bind(player_id, definition.quest_id), complete_result);
                    if (!definition.completion_flag.empty()) {
                        const auto flag_statement = StatementCache::prepare(connection, 
                            "INSERT IGNORE INTO player_story_flag(player_id, flag_id) VALUES (?, ?)");
                        connection.execute(flag_statement.bind(player_id, definition.completion_flag), complete_result);
                    }
                    if (definition.claim_mode == mf::game::QuestClaimMode::automatic) {
                        auto automatic = grant_quest_on(connection, player_id, definition, "auto:" + event_id, "");
                        if (automatic) {
                            event_result.automatic_grants.push_back(automatic->grant);
                            event_result.player = automatic->player;
                        }
                    }
                }
                if (!matched) {
                    failure_code = "quest_event_rejected";
                    connection.execute("ROLLBACK", control);
                    return;
                }
                event_result.snapshot = load_snapshot(connection, player_id);
                connection.execute("COMMIT", control);
            } catch (...) {
                try { connection.execute("ROLLBACK", control); } catch (...) {}
                throw;
            }
        });
        return event_result;
    }

    std::optional<mf::game::QuestClaimResult> claim(
        const std::string& player_id,
        const std::string& quest_id,
        const std::string& request_id,
        const std::string& npc_id,
        std::string& failure_code) override {
        std::optional<mf::game::QuestClaimResult> claimed;
        client_->with_connection(__func__, "quest.claim", [&](mysql::any_connection& connection) {
            mysql::results control;
            connection.execute("START TRANSACTION", control);
            try {
                const auto definition = find_definition_on(connection, quest_id, true);
                if (!definition) {
                    failure_code = "quest_not_found";
                    connection.execute("ROLLBACK", control);
                    return;
                }
                const auto player_quest = find_player_quest_on(connection, player_id, quest_id, true);
                if (!player_quest) {
                    failure_code = "quest_not_accepted";
                    connection.execute("ROLLBACK", control);
                    return;
                }
                const auto request_statement = StatementCache::prepare(connection, 
                    "SELECT quest_id FROM player_quest WHERE player_id=? AND claim_request_id=? LIMIT 1");
                mysql::results request_result;
                connection.execute(request_statement.bind(player_id, request_id), request_result);
                if (!request_result.rows().empty() && column_string(request_result.rows().at(0), 0) != quest_id) {
                    failure_code = "quest_request_conflict";
                    connection.execute("ROLLBACK", control);
                    return;
                }
                if (player_quest->state == mf::game::QuestState::claimed) {
                    claimed = load_claim_result(connection, player_id, quest_id, true);
                    connection.execute("COMMIT", control);
                    return;
                }
                if (player_quest->state != mf::game::QuestState::completed) {
                    failure_code = "quest_not_completed";
                    connection.execute("ROLLBACK", control);
                    return;
                }
                if (definition->claim_mode == mf::game::QuestClaimMode::npc &&
                    definition->turn_in_npc_id != npc_id) {
                    failure_code = "quest_npc_mismatch";
                    connection.execute("ROLLBACK", control);
                    return;
                }
                claimed = grant_quest_on(connection, player_id, *definition, request_id, npc_id);
                if (!claimed) failure_code = "player_not_found";
                if (claimed) connection.execute("COMMIT", control);
                else connection.execute("ROLLBACK", control);
            } catch (...) {
                try { connection.execute("ROLLBACK", control); } catch (...) {}
                throw;
            }
        });
        return claimed;
    }

private:
    static std::optional<mf::game::QuestDefinition> find_definition_on(
        mysql::any_connection& connection,
        const std::string& quest_id,
        bool for_update) {
        const auto statement = StatementCache::prepare(connection, 
            "SELECT quest_id, version, title, description_text, claim_mode, turn_in_npc_id, completion_flag, "
            "CAST(pre_quest_ids_json AS CHAR), CAST(required_flags_json AS CHAR), CAST(objectives_json AS CHAR), "
            "CAST(rewards_json AS CHAR), enabled, UNIX_TIMESTAMP(created_at), UNIX_TIMESTAMP(updated_at) "
            "FROM quest_definition WHERE quest_id = ?" + std::string(for_update ? " FOR UPDATE" : ""));
        mysql::results result;
        connection.execute(statement.bind(quest_id), result);
        return result.rows().empty()
                   ? std::nullopt
                   : std::optional<mf::game::QuestDefinition>{row_to_quest_definition(result.rows().at(0))};
    }

    static std::optional<mf::game::PlayerQuest> find_player_quest_on(
        mysql::any_connection& connection,
        const std::string& player_id,
        const std::string& quest_id,
        bool for_update) {
        const auto statement = StatementCache::prepare(connection, 
            "SELECT player_id, quest_id, quest_version, state, progress_version, UNIX_TIMESTAMP(accepted_at), "
            "UNIX_TIMESTAMP(completed_at), UNIX_TIMESTAMP(claimed_at) FROM player_quest "
            "WHERE player_id=? AND quest_id=?" + std::string(for_update ? " FOR UPDATE" : ""));
        mysql::results result;
        connection.execute(statement.bind(player_id, quest_id), result);
        if (result.rows().empty()) return std::nullopt;
        const auto row = result.rows().at(0);
        mf::game::PlayerQuest quest{
            .player_id = column_string(row, 0),
            .quest_id = column_string(row, 1),
            .quest_version = column_i64(row, 2),
            .state = parse_quest_state(column_string(row, 3)),
            .progress_version = column_i64(row, 4),
            .accepted_at = optional_time_point_from_row(row, 5),
            .completed_at = optional_time_point_from_row(row, 6),
            .claimed_at = optional_time_point_from_row(row, 7),
        };
        const auto objectives_statement = StatementCache::prepare(connection, 
            "SELECT objective_id, current_value, target_value FROM player_quest_objective "
            "WHERE player_id=? AND quest_id=? ORDER BY objective_id");
        mysql::results objectives_result;
        connection.execute(objectives_statement.bind(player_id, quest_id), objectives_result);
        for (const auto objective_row : objectives_result.rows()) {
            quest.objectives.push_back({
                .objective_id = column_string(objective_row, 0),
                .current_value = column_i64(objective_row, 1),
                .target_value = column_i64(objective_row, 2),
            });
        }
        return quest;
    }

    static std::vector<mf::game::PlayerQuest> load_player_quests_batch(
        mysql::any_connection& connection,
        const std::string& player_id) {
        std::vector<mf::game::PlayerQuest> quests;
        const auto main_statement = StatementCache::prepare(connection,
            "SELECT player_id, quest_id, quest_version, state, progress_version, UNIX_TIMESTAMP(accepted_at), "
            "UNIX_TIMESTAMP(completed_at), UNIX_TIMESTAMP(claimed_at) FROM player_quest "
            "WHERE player_id=? ORDER BY accepted_at, quest_id");
        mysql::results main_result;
        connection.execute(main_statement.bind(player_id), main_result);
        std::unordered_map<std::string, std::size_t> index_by_quest_id;
        index_by_quest_id.reserve(main_result.rows().size());
        for (const auto row : main_result.rows()) {
            const auto quest_id = column_string(row, 1);
            index_by_quest_id.emplace(quest_id, quests.size());
            quests.push_back({
                .player_id = column_string(row, 0),
                .quest_id = quest_id,
                .quest_version = column_i64(row, 2),
                .state = parse_quest_state(column_string(row, 3)),
                .progress_version = column_i64(row, 4),
                .accepted_at = optional_time_point_from_row(row, 5),
                .completed_at = optional_time_point_from_row(row, 6),
                .claimed_at = optional_time_point_from_row(row, 7),
            });
        }
        const auto objectives_statement = StatementCache::prepare(connection,
            "SELECT quest_id, objective_id, current_value, target_value FROM player_quest_objective "
            "WHERE player_id=? ORDER BY quest_id, objective_id");
        mysql::results objectives_result;
        connection.execute(objectives_statement.bind(player_id), objectives_result);
        for (const auto objective_row : objectives_result.rows()) {
            const auto quest_id = column_string(objective_row, 0);
            const auto it = index_by_quest_id.find(quest_id);
            if (it == index_by_quest_id.end()) {
                continue;
            }
            quests[it->second].objectives.push_back({
                .objective_id = column_string(objective_row, 1),
                .current_value = column_i64(objective_row, 2),
                .target_value = column_i64(objective_row, 3),
            });
        }
        return quests;
    }

    static mf::game::QuestSnapshot load_snapshot(mysql::any_connection& connection, const std::string& player_id) {
        mf::game::QuestSnapshot snapshot;
        snapshot.quests = load_player_quests_batch(connection, player_id);
        const auto flags_statement = StatementCache::prepare(connection, 
            "SELECT flag_id FROM player_story_flag WHERE player_id=? ORDER BY flag_id");
        mysql::results flags_result;
        connection.execute(flags_statement.bind(player_id), flags_result);
        for (const auto row : flags_result.rows()) snapshot.story_flags.push_back(column_string(row, 0));
        return snapshot;
    }

    static bool conditions_met(
        mysql::any_connection& connection,
        const std::string& player_id,
        const mf::game::QuestDefinition& definition) {
        if (!definition.pre_quest_ids.empty()) {
            boost::json::array wanted_ids;
            wanted_ids.reserve(definition.pre_quest_ids.size());
            for (const auto& pre_quest_id : definition.pre_quest_ids) {
                wanted_ids.emplace_back(pre_quest_id);
            }
            const auto wanted_ids_json = boost::json::serialize(wanted_ids);
            const auto quest_statement = StatementCache::prepare(connection,
                "SELECT p.quest_id, p.state FROM player_quest p "
                "JOIN JSON_TABLE(?, '$[*]' COLUMNS(quest_id VARCHAR(64) PATH '$')) AS ids "
                "ON ids.quest_id = p.quest_id COLLATE utf8mb4_0900_ai_ci WHERE p.player_id=?");
            mysql::results result;
            connection.execute(quest_statement.bind(wanted_ids_json, player_id), result);
            std::unordered_map<std::string, std::string> quest_states;
            quest_states.reserve(result.rows().size());
            for (const auto row : result.rows()) {
                quest_states.emplace(column_string(row, 0), column_string(row, 1));
            }
            for (const auto& pre_quest_id : definition.pre_quest_ids) {
                const auto it = quest_states.find(pre_quest_id);
                if (it == quest_states.end() || it->second != "claimed") {
                    return false;
                }
            }
        }
        if (!definition.required_flags.empty()) {
            boost::json::array wanted_flags;
            wanted_flags.reserve(definition.required_flags.size());
            for (const auto& flag_id : definition.required_flags) {
                wanted_flags.emplace_back(flag_id);
            }
            const auto wanted_flags_json = boost::json::serialize(wanted_flags);
            const auto flag_statement = StatementCache::prepare(connection,
                "SELECT f.flag_id FROM player_story_flag f "
                "JOIN JSON_TABLE(?, '$[*]' COLUMNS(flag_id VARCHAR(64) PATH '$')) AS ids "
                "ON ids.flag_id = f.flag_id COLLATE utf8mb4_0900_ai_ci WHERE f.player_id=?");
            mysql::results result;
            connection.execute(flag_statement.bind(wanted_flags_json, player_id), result);
            std::unordered_set<std::string> flags;
            flags.reserve(result.rows().size());
            for (const auto row : result.rows()) {
                flags.insert(column_string(row, 0));
            }
            for (const auto& flag_id : definition.required_flags) {
                if (flags.count(flag_id) == 0) {
                    return false;
                }
            }
        }
        return true;
    }

    static std::vector<mf::game::QuestDefinition> list_active_player_definitions(
        mysql::any_connection& connection,
        const std::string& player_id) {
        std::vector<mf::game::QuestDefinition> definitions;
        const auto ids_statement = StatementCache::prepare(connection,
            "SELECT quest_id FROM player_quest WHERE player_id=? AND state='accepted' FOR UPDATE");
        mysql::results ids_result;
        connection.execute(ids_statement.bind(player_id), ids_result);
        if (ids_result.rows().empty()) {
            return definitions;
        }
        boost::json::array wanted_ids;
        wanted_ids.reserve(ids_result.rows().size());
        for (const auto row : ids_result.rows()) {
            wanted_ids.emplace_back(column_string(row, 0));
        }
        const auto wanted_ids_json = boost::json::serialize(wanted_ids);
        const auto statement = StatementCache::prepare(connection,
            "SELECT d.quest_id, d.version, d.title, d.description_text, d.claim_mode, d.turn_in_npc_id, "
            "d.completion_flag, CAST(d.pre_quest_ids_json AS CHAR), CAST(d.required_flags_json AS CHAR), "
            "CAST(d.objectives_json AS CHAR), CAST(d.rewards_json AS CHAR), d.enabled, "
            "UNIX_TIMESTAMP(d.created_at), UNIX_TIMESTAMP(d.updated_at) FROM quest_definition d "
            "JOIN JSON_TABLE(?, '$[*]' COLUMNS(quest_id VARCHAR(64) PATH '$')) AS ids "
            "ON ids.quest_id = d.quest_id COLLATE utf8mb4_0900_ai_ci WHERE d.enabled=1");
        mysql::results result;
        connection.execute(statement.bind(wanted_ids_json), result);
        for (const auto row : result.rows()) definitions.push_back(row_to_quest_definition(row));
        return definitions;
    }

    static bool all_objectives_completed(
        mysql::any_connection& connection,
        const std::string& player_id,
        const std::string& quest_id) {
        const auto statement = StatementCache::prepare(connection, 
            "SELECT COUNT(*) FROM player_quest_objective WHERE player_id=? AND quest_id=? "
            "AND current_value < target_value");
        mysql::results result;
        connection.execute(statement.bind(player_id, quest_id), result);
        return column_i64(result.rows().at(0), 0) == 0;
    }

    static void increment_progress_version(
        mysql::any_connection& connection,
        const std::string& player_id,
        const std::string& quest_id) {
        const auto statement = StatementCache::prepare(connection, 
            "UPDATE player_quest SET progress_version=progress_version+1 WHERE player_id=? AND quest_id=?");
        mysql::results result;
        connection.execute(statement.bind(player_id, quest_id), result);
    }

    static std::optional<mf::game::QuestClaimResult> grant_quest_on(
        mysql::any_connection& connection,
        const std::string& player_id,
        const mf::game::QuestDefinition& definition,
        const std::string& request_id,
        const std::string&) {
        const auto grant_id = "quest:" + player_id + ':' + definition.quest_id;
        const auto now = std::chrono::system_clock::now();
        const auto insert_statement = StatementCache::prepare(connection, 
            "INSERT IGNORE INTO reward_grant(grant_id, player_id, source_type, source_id, rewards_json, granted_at) "
            "VALUES (?, ?, 'quest', ?, CAST(? AS JSON), FROM_UNIXTIME(?))");
        mysql::results insert_result;
        connection.execute(
            insert_statement.bind(
                grant_id,
                player_id,
                definition.quest_id,
                encode_rewards_json(definition.rewards),
                to_unix_seconds(now)),
            insert_result);
        std::optional<mf::game::PlayerProfile> player;
        if (insert_result.affected_rows() > 0) {
            player = apply_rewards_in_transaction(connection, player_id, definition.rewards);
            if (!player) return std::nullopt;
        } else {
            const auto player_statement = StatementCache::prepare(connection, 
                "SELECT player_id, account_id, nickname, gold, gem "
                "FROM player_profile WHERE player_id=?");
            mysql::results player_result;
            connection.execute(player_statement.bind(player_id), player_result);
            if (player_result.rows().empty()) return std::nullopt;
            player = row_to_profile(connection, player_result.rows().at(0));
        }
        const auto update_statement = StatementCache::prepare(connection, 
            "UPDATE player_quest SET state='claimed', claimed_at=COALESCE(claimed_at, CURRENT_TIMESTAMP), "
            "claim_request_id=COALESCE(claim_request_id, ?), progress_version=progress_version+1 "
            "WHERE player_id=? AND quest_id=?");
        mysql::results update_result;
        connection.execute(update_statement.bind(request_id, player_id, definition.quest_id), update_result);
        auto quest = find_player_quest_on(connection, player_id, definition.quest_id, false);
        if (!quest) return std::nullopt;
        return mf::game::QuestClaimResult{
            .quest = std::move(*quest),
            .grant = mf::game::RewardGrantRecord{
                .grant_id = grant_id,
                .player_id = player_id,
                .source_type = "quest",
                .source_id = definition.quest_id,
                .rewards = definition.rewards,
                .granted_at = now,
            },
            .player = std::move(*player),
            .already_claimed = insert_result.affected_rows() == 0,
        };
    }

    static std::optional<mf::game::QuestClaimResult> load_claim_result(
        mysql::any_connection& connection,
        const std::string& player_id,
        const std::string& quest_id,
        bool already_claimed) {
        const auto quest = find_player_quest_on(connection, player_id, quest_id, false);
        if (!quest) return std::nullopt;
        const auto grant_id = "quest:" + player_id + ':' + quest_id;
        const auto grant_statement = StatementCache::prepare(connection, 
            "SELECT grant_id, player_id, source_type, source_id, CAST(rewards_json AS CHAR), "
            "UNIX_TIMESTAMP(granted_at) FROM reward_grant WHERE grant_id=?");
        mysql::results grant_result;
        connection.execute(grant_statement.bind(grant_id), grant_result);
        const auto player_statement = StatementCache::prepare(connection, 
            "SELECT player_id, account_id, nickname, gold, gem "
            "FROM player_profile WHERE player_id=?");
        mysql::results player_result;
        connection.execute(player_statement.bind(player_id), player_result);
        if (grant_result.rows().empty() || player_result.rows().empty()) return std::nullopt;
        return mf::game::QuestClaimResult{
            .quest = *quest,
            .grant = row_to_reward_grant(grant_result.rows().at(0)),
            .player = row_to_profile(connection, player_result.rows().at(0)),
            .already_claimed = already_claimed,
        };
    }

    std::shared_ptr<MySqlClient> client_;
};

class MySqlRewardRepository final : public mf::game::RewardRepository {
public:
    explicit MySqlRewardRepository(std::shared_ptr<MySqlClient> client)
        : client_(std::move(client)) {}

    // 按 grant_id 查询奖励流水。
    // 这是奖励幂等的第一层锚点，重复发奖时会先查这里。
    std::optional<mf::game::RewardGrantRecord> find_by_grant_id(const std::string& grant_id) const override {
        std::optional<mf::game::RewardGrantRecord> grant;
        client_->with_connection(__func__, [&](mysql::any_connection& connection) {
            mysql::results result;
            connection.execute(statements_.reward_grant_find_by_id(connection).bind(grant_id), result);
            if (!result.rows().empty()) {
                grant = row_to_reward_grant(result.rows().at(0));
            }
        });
        return grant;
    }

    // 记录一条奖励流水。
    // 奖励真实入账由 PlayerRepository::apply_rewards 完成，这里只负责把这次发奖事实持久化。
    mf::game::RewardGrantRecord record_grant(
        std::string grant_id,
        const std::string& player_id,
        std::string source_type,
        std::string source_id,
        std::vector<mf::game::RewardEntry> rewards) override {
        const auto granted_at = std::chrono::system_clock::now();
        const auto rewards_payload = encode_rewards_json(rewards);
        client_->with_connection(__func__, [&](mysql::any_connection& connection) {
            mysql::results result;
            connection.execute(
                statements_.reward_grant_insert(connection).bind(
                    grant_id,
                    player_id,
                    source_type,
                    source_id,
                    rewards_payload,
                    to_unix_seconds(granted_at)),
                result);
        });

        return mf::game::RewardGrantRecord{
            .grant_id = std::move(grant_id),
            .player_id = player_id,
            .source_type = std::move(source_type),
            .source_id = std::move(source_id),
            .rewards = std::move(rewards),
            .granted_at = granted_at,
        };
    }

    // 查询玩家最近奖励流水。
    // 这里按 granted_at 倒序返回，方便客户端或后台直接看最近奖励。
    std::vector<mf::game::RewardGrantRecord> list_by_player_id(
        const std::string& player_id,
        std::size_t limit) const override {
        std::vector<mf::game::RewardGrantRecord> grants;
        client_->with_connection(__func__, [&](mysql::any_connection& connection) {
            mysql::results result;
            connection.execute(
                statements_.reward_grant_list_by_player(connection).bind(
                    player_id,
                    static_cast<std::uint64_t>(limit)),
                result);
            grants.reserve(result.rows().size());
            for (const auto row : result.rows()) {
                grants.push_back(row_to_reward_grant(row));
            }
        });
        return grants;
    }

    std::size_t count_filtered(
        const std::string& player_id,
        const std::optional<std::string>& source_type_filter,
        const std::optional<std::string>& source_id_filter) const override {
        std::size_t total = 0;
        client_->with_connection(__func__, [&](mysql::any_connection& connection) {
            mysql::results result;
            connection.execute(
                statements_.reward_grant_count_filtered(connection).bind(
                    player_id,
                    source_type_filter.value_or(std::string{}),
                    source_type_filter.value_or(std::string{}),
                    source_id_filter.value_or(std::string{}),
                    source_id_filter.value_or(std::string{})),
                result);
            if (!result.rows().empty()) {
                total = static_cast<std::size_t>(column_i64(result.rows().at(0), 0));
            }
        });
        return total;
    }

    std::vector<mf::game::RewardGrantRecord> list_filtered(
        const std::string& player_id,
        std::size_t limit,
        std::size_t offset,
        const std::optional<std::string>& source_type_filter,
        const std::optional<std::string>& source_id_filter) const override {
        std::vector<mf::game::RewardGrantRecord> grants;
        client_->with_connection(__func__, [&](mysql::any_connection& connection) {
            mysql::results result;
            connection.execute(
                statements_.reward_grant_list_filtered(connection).bind(
                    player_id,
                    source_type_filter.value_or(std::string{}),
                    source_type_filter.value_or(std::string{}),
                    source_id_filter.value_or(std::string{}),
                    source_id_filter.value_or(std::string{}),
                    static_cast<std::uint64_t>(limit),
                    static_cast<std::uint64_t>(offset)),
                result);
            grants.reserve(result.rows().size());
            for (const auto row : result.rows()) {
                grants.push_back(row_to_reward_grant(row));
            }
        });
        return grants;
    }

private:
    std::shared_ptr<MySqlClient> client_;
    mutable StatementCache statements_;
};

class MySqlMultiplayerRoomRepository final : public mf::multiplayer::MultiplayerRoomRepository {
public:
    explicit MySqlMultiplayerRoomRepository(std::shared_ptr<MySqlClient> client)
        : client_(std::move(client)) {}

    mf::multiplayer::RoomMutationResult create_room(
        mf::multiplayer::MultiplayerRoom room,
        mf::multiplayer::MultiplayerRoomMember owner) override {
        mf::multiplayer::RoomMutationResult outcome;
        client_->with_connection(__func__, "multiplayer.room_create", [&](mysql::any_connection& connection) {
            mysql::results control;
            connection.execute("START TRANSACTION", control);
            try {
                const auto insert_room = StatementCache::prepare(connection, 
                    "INSERT IGNORE INTO multiplayer_room(room_id, owner_player_id, room_name, max_players, status) "
                    "VALUES (?, ?, ?, ?, 'open')");
                mysql::results room_result;
                connection.execute(
                    insert_room.bind(room.room_id, room.owner_player_id, room.room_name, room.max_players),
                    room_result);
                if (room_result.affected_rows() == 0) {
                    connection.execute("ROLLBACK", control);
                    outcome.status = mf::multiplayer::RoomMutationStatus::room_exists;
                    return;
                }
                const auto insert_member = StatementCache::prepare(connection, 
                    "INSERT INTO multiplayer_room_member(room_id, player_id, display_name, member_role) "
                    "VALUES (?, ?, ?, 'leader')");
                mysql::results member_result;
                connection.execute(
                    insert_member.bind(room.room_id, owner.player_id, owner.display_name),
                    member_result);
                connection.execute("COMMIT", control);
            } catch (...) {
                try { connection.execute("ROLLBACK", control); } catch (...) {}
                throw;
            }
            outcome.status = mf::multiplayer::RoomMutationStatus::success;
        });
        if (outcome.status == mf::multiplayer::RoomMutationStatus::success) {
            outcome.room = find_room(room.room_id);
            outcome.member = find_member(room.room_id, owner.player_id);
        }
        return outcome;
    }

    mf::multiplayer::RoomMutationResult join_room(
        const std::string& room_id,
        mf::multiplayer::MultiplayerRoomMember member) override {
        mf::multiplayer::RoomMutationResult outcome;
        client_->with_connection(__func__, "multiplayer.room_join", [&](mysql::any_connection& connection) {
            mysql::results control;
            connection.execute("START TRANSACTION", control);
            try {
                const auto room_statement = StatementCache::prepare(connection, 
                    "SELECT room_id, owner_player_id, room_name, max_players, status, "
                    "UNIX_TIMESTAMP(created_at), UNIX_TIMESTAMP(updated_at) "
                    "FROM multiplayer_room WHERE room_id=? FOR UPDATE");
                mysql::results room_result;
                connection.execute(room_statement.bind(room_id), room_result);
                if (room_result.rows().empty()) {
                    connection.execute("ROLLBACK", control);
                    outcome.status = mf::multiplayer::RoomMutationStatus::room_not_found;
                    return;
                }
                auto room = row_to_room(room_result.rows().at(0));
                if (room.status != "open") {
                    connection.execute("ROLLBACK", control);
                    outcome.status = mf::multiplayer::RoomMutationStatus::room_closed;
                    return;
                }
                const auto existing_statement = StatementCache::prepare(connection, 
                    "SELECT 1 FROM multiplayer_room_member WHERE room_id=? AND player_id=? LIMIT 1");
                mysql::results existing_result;
                connection.execute(existing_statement.bind(room_id, member.player_id), existing_result);
                if (!existing_result.rows().empty()) {
                    connection.execute("ROLLBACK", control);
                    outcome.status = mf::multiplayer::RoomMutationStatus::already_member;
                    return;
                }
                const auto count_statement = StatementCache::prepare(connection, 
                    "SELECT COUNT(*) FROM multiplayer_room_member WHERE room_id=?");
                mysql::results count_result;
                connection.execute(count_statement.bind(room_id), count_result);
                if (column_i64(count_result.rows().at(0), 0) >= room.max_players) {
                    connection.execute("ROLLBACK", control);
                    outcome.status = mf::multiplayer::RoomMutationStatus::room_full;
                    return;
                }
                const auto insert_statement = StatementCache::prepare(connection, 
                    "INSERT INTO multiplayer_room_member(room_id, player_id, display_name, member_role) "
                    "VALUES (?, ?, ?, 'member')");
                mysql::results insert_result;
                connection.execute(insert_statement.bind(room_id, member.player_id, member.display_name), insert_result);
                const auto update_statement = StatementCache::prepare(connection, 
                    "UPDATE multiplayer_room SET updated_at=CURRENT_TIMESTAMP WHERE room_id=?");
                mysql::results update_result;
                connection.execute(update_statement.bind(room_id), update_result);
                connection.execute("COMMIT", control);
                outcome.status = mf::multiplayer::RoomMutationStatus::success;
                outcome.room = std::move(room);
            } catch (...) {
                try { connection.execute("ROLLBACK", control); } catch (...) {}
                throw;
            }
        });
        if (outcome.status == mf::multiplayer::RoomMutationStatus::success) {
            outcome.room = find_room(room_id);
            outcome.member = find_member(room_id, member.player_id);
        }
        return outcome;
    }

    mf::multiplayer::RoomMutationResult leave_room(
        const std::string& room_id,
        const std::string& player_id) override {
        mf::multiplayer::RoomMutationResult outcome;
        client_->with_connection(__func__, "multiplayer.room_leave", [&](mysql::any_connection& connection) {
            mysql::results control;
            connection.execute("START TRANSACTION", control);
            try {
                const auto room_statement = StatementCache::prepare(connection, 
                    "SELECT room_id, owner_player_id, room_name, max_players, status, "
                    "UNIX_TIMESTAMP(created_at), UNIX_TIMESTAMP(updated_at) "
                    "FROM multiplayer_room WHERE room_id=? FOR UPDATE");
                mysql::results room_result;
                connection.execute(room_statement.bind(room_id), room_result);
                if (room_result.rows().empty()) {
                    connection.execute("ROLLBACK", control);
                    outcome.status = mf::multiplayer::RoomMutationStatus::room_not_found;
                    return;
                }
                auto room = row_to_room(room_result.rows().at(0));
                if (room.owner_player_id == player_id) {
                    connection.execute("ROLLBACK", control);
                    outcome.status = mf::multiplayer::RoomMutationStatus::owner_must_close;
                    return;
                }
                const auto member_statement = StatementCache::prepare(connection, 
                    "SELECT room_id, player_id, display_name, member_role, UNIX_TIMESTAMP(joined_at) "
                    "FROM multiplayer_room_member WHERE room_id=? AND player_id=? FOR UPDATE");
                mysql::results member_result;
                connection.execute(member_statement.bind(room_id, player_id), member_result);
                if (member_result.rows().empty()) {
                    connection.execute("ROLLBACK", control);
                    outcome.status = mf::multiplayer::RoomMutationStatus::member_not_found;
                    return;
                }
                const auto removed = row_to_member(member_result.rows().at(0));
                const auto delete_statement = StatementCache::prepare(connection, 
                    "DELETE FROM multiplayer_room_member WHERE room_id=? AND player_id=?");
                mysql::results delete_result;
                connection.execute(delete_statement.bind(room_id, player_id), delete_result);
                const auto update_statement = StatementCache::prepare(connection, 
                    "UPDATE multiplayer_room SET updated_at=CURRENT_TIMESTAMP WHERE room_id=?");
                mysql::results update_result;
                connection.execute(update_statement.bind(room_id), update_result);
                connection.execute("COMMIT", control);
                outcome.status = mf::multiplayer::RoomMutationStatus::success;
                outcome.room = std::move(room);
                outcome.member = removed;
                outcome.affected_members = {removed};
            } catch (...) {
                try { connection.execute("ROLLBACK", control); } catch (...) {}
                throw;
            }
        });
        if (outcome.status == mf::multiplayer::RoomMutationStatus::success) outcome.room = find_room(room_id);
        return outcome;
    }

    mf::multiplayer::RoomMutationResult close_room(
        const std::string& room_id,
        const std::string& requester_player_id) override {
        mf::multiplayer::RoomMutationResult outcome;
        client_->with_connection(__func__, "multiplayer.room_close", [&](mysql::any_connection& connection) {
            mysql::results control;
            connection.execute("START TRANSACTION", control);
            try {
                const auto room_statement = StatementCache::prepare(connection, 
                    "SELECT room_id, owner_player_id, room_name, max_players, status, "
                    "UNIX_TIMESTAMP(created_at), UNIX_TIMESTAMP(updated_at) "
                    "FROM multiplayer_room WHERE room_id=? FOR UPDATE");
                mysql::results room_result;
                connection.execute(room_statement.bind(room_id), room_result);
                if (room_result.rows().empty()) {
                    connection.execute("ROLLBACK", control);
                    outcome.status = mf::multiplayer::RoomMutationStatus::room_not_found;
                    return;
                }
                auto room = row_to_room(room_result.rows().at(0));
                if (room.owner_player_id != requester_player_id) {
                    connection.execute("ROLLBACK", control);
                    outcome.status = mf::multiplayer::RoomMutationStatus::permission_denied;
                    return;
                }
                const auto members_statement = StatementCache::prepare(connection, 
                    "SELECT room_id, player_id, display_name, member_role, UNIX_TIMESTAMP(joined_at) "
                    "FROM multiplayer_room_member WHERE room_id=? FOR UPDATE");
                mysql::results members_result;
                connection.execute(members_statement.bind(room_id), members_result);
                for (const auto row : members_result.rows()) outcome.affected_members.push_back(row_to_member(row));
                const auto update_statement = StatementCache::prepare(connection, 
                    "UPDATE multiplayer_room SET status='closed', updated_at=CURRENT_TIMESTAMP WHERE room_id=?");
                mysql::results update_result;
                connection.execute(update_statement.bind(room_id), update_result);
                const auto delete_statement = StatementCache::prepare(connection, 
                    "DELETE FROM multiplayer_room_member WHERE room_id=?");
                mysql::results delete_result;
                connection.execute(delete_statement.bind(room_id), delete_result);
                connection.execute("COMMIT", control);
                room.status = "closed";
                outcome.status = mf::multiplayer::RoomMutationStatus::success;
                outcome.room = std::move(room);
            } catch (...) {
                try { connection.execute("ROLLBACK", control); } catch (...) {}
                throw;
            }
        });
        if (outcome.status == mf::multiplayer::RoomMutationStatus::success) outcome.room = find_room(room_id);
        return outcome;
    }

    std::optional<mf::multiplayer::MultiplayerRoom> find_room(const std::string& room_id) const override {
        std::optional<mf::multiplayer::MultiplayerRoom> room;
        client_->with_connection(__func__, "multiplayer.room_find", [&](mysql::any_connection& connection) {
            const auto statement = StatementCache::prepare(connection, 
                "SELECT room_id, owner_player_id, room_name, max_players, status, "
                "UNIX_TIMESTAMP(created_at), UNIX_TIMESTAMP(updated_at) "
                "FROM multiplayer_room WHERE room_id=?");
            mysql::results result;
            connection.execute(statement.bind(room_id), result);
            if (!result.rows().empty()) room = row_to_room(result.rows().at(0));
        });
        return room;
    }

    std::optional<mf::multiplayer::MultiplayerRoomMember> find_member(
        const std::string& room_id,
        const std::string& player_id) const override {
        std::optional<mf::multiplayer::MultiplayerRoomMember> member;
        client_->with_connection(__func__, "multiplayer.member_find", [&](mysql::any_connection& connection) {
            const auto statement = StatementCache::prepare(connection, 
                "SELECT room_id, player_id, display_name, member_role, UNIX_TIMESTAMP(joined_at) "
                "FROM multiplayer_room_member WHERE room_id=? AND player_id=?");
            mysql::results result;
            connection.execute(statement.bind(room_id, player_id), result);
            if (!result.rows().empty()) member = row_to_member(result.rows().at(0));
        });
        return member;
    }

    std::vector<mf::multiplayer::MultiplayerRoomMember> list_members(const std::string& room_id) const override {
        std::vector<mf::multiplayer::MultiplayerRoomMember> members;
        client_->with_connection(__func__, "multiplayer.member_list", [&](mysql::any_connection& connection) {
            const auto statement = StatementCache::prepare(connection, 
                "SELECT room_id, player_id, display_name, member_role, UNIX_TIMESTAMP(joined_at) "
                "FROM multiplayer_room_member WHERE room_id=? "
                "ORDER BY CASE WHEN member_role='leader' THEN 0 ELSE 1 END, joined_at, player_id");
            mysql::results result;
            connection.execute(statement.bind(room_id), result);
            for (const auto row : result.rows()) members.push_back(row_to_member(row));
        });
        return members;
    }

private:
    static mf::multiplayer::MultiplayerRoom row_to_room(const mysql::row_view& row) {
        return {
            .room_id = column_string(row, 0),
            .owner_player_id = column_string(row, 1),
            .room_name = column_string(row, 2),
            .max_players = column_i64(row, 3),
            .status = column_string(row, 4),
            .created_at = optional_time_point_from_row(row, 5),
            .updated_at = optional_time_point_from_row(row, 6),
        };
    }

    static mf::multiplayer::MultiplayerRoomMember row_to_member(const mysql::row_view& row) {
        return {
            .room_id = column_string(row, 0),
            .player_id = column_string(row, 1),
            .display_name = column_string(row, 2),
            .role = column_string(row, 3),
            .joined_at = optional_time_point_from_row(row, 4),
        };
    }

    std::shared_ptr<MySqlClient> client_;
};

class MySqlRewardTemplateRepository final : public mf::game::RewardTemplateRepository {
public:
    explicit MySqlRewardTemplateRepository(std::shared_ptr<MySqlClient> client)
        : client_(std::move(client)) {}

    // 新增或覆盖一个奖励模板。
    // 模板内容本身也是奖励数组，因此后续可以给邮件附件或活动配置直接复用。
    mf::game::RewardTemplate upsert_template(
        std::string template_id,
        std::string name,
        std::vector<mf::game::RewardEntry> rewards) override {
        const auto rewards_payload = encode_rewards_json(rewards);
        client_->with_connection(__func__, [&](mysql::any_connection& connection) {
            mysql::results result;
            connection.execute(
                statements_.reward_template_upsert(connection).bind(template_id, name, rewards_payload),
                result);
        });

        return find_by_template_id(template_id).value_or(mf::game::RewardTemplate{
            .template_id = std::move(template_id),
            .name = std::move(name),
            .rewards = std::move(rewards),
        });
    }

    // 按 template_id 查询奖励模板。
    std::optional<mf::game::RewardTemplate> find_by_template_id(const std::string& template_id) const override {
        std::optional<mf::game::RewardTemplate> reward_template;
        client_->with_connection(__func__, [&](mysql::any_connection& connection) {
            mysql::results result;
            connection.execute(statements_.reward_template_find_by_id(connection).bind(template_id), result);
            if (!result.rows().empty()) {
                reward_template = row_to_reward_template(result.rows().at(0));
            }
        });
        return reward_template;
    }

    // 列出全部奖励模板。
    std::vector<mf::game::RewardTemplate> list_all() const override {
        std::vector<mf::game::RewardTemplate> items;
        client_->with_connection(__func__, [&](mysql::any_connection& connection) {
            mysql::results result;
            connection.execute(statements_.reward_template_list_all(connection).bind(), result);
            items.reserve(result.rows().size());
            for (const auto row : result.rows()) {
                items.push_back(row_to_reward_template(row));
            }
        });
        return items;
    }

private:
    std::shared_ptr<MySqlClient> client_;
    mutable StatementCache statements_;
};

class MySqlAnnouncementRepository final : public mf::game::AnnouncementRepository {
public:
    explicit MySqlAnnouncementRepository(std::shared_ptr<MySqlClient> client)
        : client_(std::move(client)) {}

    mf::game::AnnouncementEntry publish(
        mf::game::ChatChannel channel,
        std::string operator_id,
        std::string title,
        std::string body,
        std::chrono::system_clock::time_point published_at,
        std::optional<std::chrono::system_clock::time_point> expires_at) override {
        mf::game::AnnouncementEntry entry{
            .announcement_id = make_audit_id("announcement", channel.channel_id + "|" + operator_id + "|" + body),
            .channel = std::move(channel),
            .operator_id = std::move(operator_id),
            .title = std::move(title),
            .body = std::move(body),
            .published_at = published_at,
            .expires_at = expires_at,
        };
        client_->with_connection(__func__, [&](mysql::any_connection& connection) {
            mysql::results result;
            connection.execute(
                statements_.announcement_insert(connection).bind(
                    entry.announcement_id,
                    entry.channel.channel_id,
                    mf::chat::to_string(entry.channel.kind),
                    entry.channel.scope_id,
                    entry.channel.display_name,
                    entry.operator_id,
                    entry.title,
                    entry.body,
                    to_unix_seconds(*entry.published_at),
                    entry.expires_at ? std::optional<std::int64_t>{to_unix_seconds(*entry.expires_at)} : std::nullopt),
                result);
        });
        return entry;
    }

    std::vector<mf::game::AnnouncementEntry> list_recent(
        std::size_t limit,
        const std::optional<mf::game::ChatChannelKind>& kind_filter,
        const std::optional<std::string>& scope_filter,
        std::chrono::system_clock::time_point now) const override {
        std::vector<mf::game::AnnouncementEntry> items;
        client_->with_connection(__func__, [&](mysql::any_connection& connection) {
            mysql::results result;
            connection.execute(
                statements_.announcement_list_recent(connection).bind(
                    kind_filter ? std::string{mf::chat::to_string(*kind_filter)} : std::string{},
                    kind_filter ? std::string{mf::chat::to_string(*kind_filter)} : std::string{},
                    scope_filter.value_or(std::string{}),
                    scope_filter.value_or(std::string{}),
                    to_unix_seconds(now),
                    static_cast<std::uint64_t>(limit)),
                result);
            items.reserve(result.rows().size());
            for (const auto row : result.rows()) {
                items.push_back(row_to_announcement(row));
            }
        });
        return items;
    }

private:
    std::shared_ptr<MySqlClient> client_;
    mutable StatementCache statements_;
};

class MySqlMailRepository final : public mf::game::MailRepository {
public:
    explicit MySqlMailRepository(std::shared_ptr<MySqlClient> client)
        : client_(std::move(client)) {}

    // 持久化一封邮件。
    // 如果 mail_id 为空，则在仓储层补一个稳定邮件 id，避免上层必须自己生成。
    mf::game::MailMessage send_mail(mf::game::MailMessage mail) override {
        if (mail.mail_id.empty()) {
            mail.mail_id = make_mail_id(mail);
        }
        if (!mail.sent_at) {
            mail.sent_at = std::chrono::system_clock::now();
        }

        const auto rewards_payload = encode_rewards_json(mail.rewards);
        const auto expires_at = mail.expires_at ? std::optional<std::int64_t>{to_unix_seconds(*mail.expires_at)} : std::nullopt;
        const auto read_at = mail.read_at ? std::optional<std::int64_t>{to_unix_seconds(*mail.read_at)} : std::nullopt;
        const auto claimed_at =
            mail.claimed_at ? std::optional<std::int64_t>{to_unix_seconds(*mail.claimed_at)} : std::nullopt;
        const auto deleted_at =
            mail.deleted_at ? std::optional<std::int64_t>{to_unix_seconds(*mail.deleted_at)} : std::nullopt;
        client_->with_connection(__func__, [&](mysql::any_connection& connection) {
            mysql::results result;
            connection.execute(
                statements_.mail_insert(connection).bind(
                    mail.mail_id,
                    mail.player_id,
                    mail.sender_name,
                    mail.title,
                    mail.body,
                    rewards_payload,
                    mail.read ? 1 : 0,
                    mail.claimed ? 1 : 0,
                    mail.deleted ? 1 : 0,
                    mail.claim_grant_id,
                    mail.sent_at ? to_unix_seconds(*mail.sent_at) : 0,
                    expires_at,
                    read_at,
                    claimed_at,
                    deleted_at),
                result);
        });
        return mail;
    }

    // 查询玩家收件箱。
    // include_deleted=false 时过滤逻辑删除邮件，贴合正常收件箱视图。
    std::vector<mf::game::MailMessage> list_inbox(
        const std::string& player_id,
        bool include_deleted) const override {
        std::vector<mf::game::MailMessage> mails;
        client_->with_connection(__func__, [&](mysql::any_connection& connection) {
            mysql::results result;
            connection.execute(statements_.mail_list_inbox(connection).bind(player_id, include_deleted ? 1 : 0), result);
            mails.reserve(result.rows().size());
            for (const auto row : result.rows()) {
                mails.push_back(row_to_mail_message(row));
            }
        });
        return mails;
    }

    std::vector<mf::game::MailMessage> list_inbox_cursor(
        const std::string& player_id,
        bool include_deleted,
        std::size_t limit,
        const std::optional<mf::game::MailPageCursor>& cursor) const override {
        if (!cursor) {
            return list_filtered(player_id, include_deleted, limit, 0, std::nullopt, std::nullopt);
        }
        std::vector<mf::game::MailMessage> mails;
        client_->with_connection(__func__, [&](mysql::any_connection& connection) {
            mysql::results result;
            connection.execute(
                statements_.mail_list_inbox_cursor(connection).bind(
                    player_id,
                    include_deleted ? 1 : 0,
                    cursor->last_sent_at,
                    cursor->last_sent_at,
                    cursor->last_mail_id,
                    static_cast<std::uint64_t>(limit)),
                result);
            mails.reserve(result.rows().size());
            for (const auto row : result.rows()) {
                mails.push_back(row_to_mail_message(row));
            }
        });
        return mails;
    }

    std::size_t count_filtered(
        const std::string& player_id,
        bool include_deleted,
        const std::optional<bool>& claimed_filter,
        const std::optional<bool>& read_filter) const override {
        std::size_t total = 0;
        client_->with_connection(__func__, [&](mysql::any_connection& connection) {
            mysql::results result;
            connection.execute(
                statements_.mail_count_filtered(connection).bind(
                    player_id,
                    include_deleted ? 1 : 0,
                    claimed_filter.has_value() ? 1 : 0,
                    claimed_filter.value_or(false) ? 1 : 0,
                    read_filter.has_value() ? 1 : 0,
                    read_filter.value_or(false) ? 1 : 0),
                result);
            if (!result.rows().empty()) {
                total = static_cast<std::size_t>(column_i64(result.rows().at(0), 0));
            }
        });
        return total;
    }

    std::vector<mf::game::MailMessage> list_filtered(
        const std::string& player_id,
        bool include_deleted,
        std::size_t limit,
        std::size_t offset,
        const std::optional<bool>& claimed_filter,
        const std::optional<bool>& read_filter) const override {
        std::vector<mf::game::MailMessage> mails;
        client_->with_connection(__func__, [&](mysql::any_connection& connection) {
            mysql::results result;
            connection.execute(
                statements_.mail_list_filtered(connection).bind(
                    player_id,
                    include_deleted ? 1 : 0,
                    claimed_filter.has_value() ? 1 : 0,
                    claimed_filter.value_or(false) ? 1 : 0,
                    read_filter.has_value() ? 1 : 0,
                    read_filter.value_or(false) ? 1 : 0,
                    static_cast<std::uint64_t>(limit),
                    static_cast<std::uint64_t>(offset)),
                result);
            mails.reserve(result.rows().size());
            for (const auto row : result.rows()) {
                mails.push_back(row_to_mail_message(row));
            }
        });
        return mails;
    }

    // 按 player_id + mail_id 查单封邮件。
    // 这里把“邮件必须属于当前玩家”也一起绑定到 SQL 条件里。
    std::optional<mf::game::MailMessage> find_mail(
        const std::string& player_id,
        const std::string& mail_id) const override {
        std::optional<mf::game::MailMessage> mail;
        client_->with_connection(__func__, [&](mysql::any_connection& connection) {
            mysql::results result;
            connection.execute(statements_.mail_find_by_id(connection).bind(player_id, mail_id), result);
            if (!result.rows().empty()) {
                mail = row_to_mail_message(result.rows().at(0));
            }
        });
        return mail;
    }

    // 标记已读后重新回读邮件。
    // 这样上层总能拿到和数据库当前状态一致的对象。
    std::optional<mf::game::MailMessage> mark_read(
        const std::string& player_id,
        const std::string& mail_id,
        std::chrono::system_clock::time_point read_at) override {
        client_->with_connection(__func__, [&](mysql::any_connection& connection) {
            mysql::results result;
            connection.execute(
                statements_.mail_mark_read(connection).bind(to_unix_seconds(read_at), player_id, mail_id),
                result);
        });
        return find_mail(player_id, mail_id);
    }

    // 标记附件已领取并写入对应奖励流水 id。
    // 领取结果最终仍通过回查邮件来返回，避免手工拼状态对象。
    std::optional<mf::game::MailMessage> mark_claimed(
        const std::string& player_id,
        const std::string& mail_id,
        const std::string& claim_grant_id,
        std::chrono::system_clock::time_point claimed_at) override {
        client_->with_connection(__func__, [&](mysql::any_connection& connection) {
            mysql::results result;
            connection.execute(
                statements_.mail_mark_claimed(connection).bind(
                    claim_grant_id,
                    to_unix_seconds(claimed_at),
                    to_unix_seconds(claimed_at),
                    player_id,
                    mail_id),
                result);
        });
        return find_mail(player_id, mail_id);
    }

    // 逻辑删除邮件。
    // 删除后仍保留事实记录，方便后续做审计、回收站或问题排查。
    std::optional<mf::game::MailMessage> mark_deleted(
        const std::string& player_id,
        const std::string& mail_id,
        std::chrono::system_clock::time_point deleted_at) override {
        client_->with_connection(__func__, [&](mysql::any_connection& connection) {
            mysql::results result;
            connection.execute(
                statements_.mail_mark_deleted(connection).bind(to_unix_seconds(deleted_at), player_id, mail_id),
                result);
        });
        return find_mail(player_id, mail_id);
    }

private:
    std::shared_ptr<MySqlClient> client_;
    mutable StatementCache statements_;
};

class MySqlAuditLogRepository final : public mf::game::AuditLogRepository {
public:
    explicit MySqlAuditLogRepository(std::shared_ptr<MySqlClient> client)
        : client_(std::move(client)) {}

    // 追加一条 GM / 后台审计日志。
    mf::game::AuditLogEntry append(mf::game::AuditLogWriteRequest request) override {
        const auto created_at = std::chrono::system_clock::now();
        const auto audit_id = make_audit_id(request.action, request.target_id);
        client_->with_connection(__func__, [&](mysql::any_connection& connection) {
            mysql::results result;
            connection.execute(
                statements_.gm_audit_insert(connection).bind(
                    audit_id,
                    request.operator_id,
                    request.action,
                    request.target_type,
                    request.target_id,
                    request.request_ip,
                    request.user_agent,
                    request.trace_id,
                    request.success ? 1 : 0,
                    request.failure_reason,
                    request.duration_ms,
                    request.before_json.empty() ? std::optional<std::string>{std::nullopt}
                                                : std::optional<std::string>{request.before_json},
                    request.after_json.empty() ? std::optional<std::string>{std::nullopt}
                                               : std::optional<std::string>{request.after_json},
                    request.detail_json.empty() ? std::string{"{}"} : request.detail_json,
                    to_unix_seconds(created_at)),
                result);
        });
        return mf::game::AuditLogEntry{
            .audit_id = audit_id,
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
            .created_at = created_at,
        };
    }

    // 查询最近的审计日志，方便后台排查管理操作。
    std::vector<mf::game::AuditLogEntry> list_recent(std::size_t limit) const override {
        std::vector<mf::game::AuditLogEntry> items;
        client_->with_connection(__func__, [&](mysql::any_connection& connection) {
            mysql::results result;
            connection.execute(
                statements_.gm_audit_list_recent(connection).bind(static_cast<std::uint64_t>(limit)),
                result);
            items.reserve(result.rows().size());
            for (const auto row : result.rows()) {
                items.push_back(row_to_audit_log(row));
            }
        });
        return items;
    }

    // 这里单独执行 COUNT(*)，
    // 让后台分页 total 直接来自数据库，而不是用当前页数据估算。
    std::size_t count_filtered(
        const std::optional<std::string>& action_filter,
        const std::optional<std::string>& operator_filter,
        const std::optional<bool>& success_filter) const override {
        std::size_t total = 0;
        client_->with_connection(__func__, [&](mysql::any_connection& connection) {
            mysql::results result;
            connection.execute(
                statements_.gm_audit_count_filtered(connection).bind(
                    action_filter.value_or(std::string{}),
                    action_filter.value_or(std::string{}),
                    operator_filter.value_or(std::string{}),
                    operator_filter.value_or(std::string{}),
                    success_filter.has_value() ? 1 : 0,
                    success_filter.value_or(false) ? 1 : 0),
                result);
            if (!result.rows().empty()) {
                total = static_cast<std::size_t>(column_i64(result.rows().at(0), 0));
            }
        });
        return total;
    }

    mf::game::AuditLogPage list_recent_filtered(
        std::size_t limit,
        const std::optional<mf::game::AuditLogCursor>& cursor,
        const std::optional<std::string>& action_filter,
        const std::optional<std::string>& operator_filter,
        const std::optional<bool>& success_filter) const override {
        mf::game::AuditLogPage page;
        const auto bounded_limit = std::clamp<std::size_t>(limit, 1, 500);
        const auto fetch_limit = bounded_limit + 1;
        client_->with_connection(__func__, [&](mysql::any_connection& connection) {
            mysql::results result;
            if (cursor) {
                connection.execute(
                    statements_.gm_audit_list_filtered_cursor(connection).bind(
                        action_filter.value_or(std::string{}),
                        action_filter.value_or(std::string{}),
                        operator_filter.value_or(std::string{}),
                        operator_filter.value_or(std::string{}),
                        success_filter.has_value() ? 1 : 0,
                        success_filter.value_or(false) ? 1 : 0,
                        cursor->last_created_at,
                        cursor->last_created_at,
                        cursor->last_audit_id,
                        static_cast<std::uint64_t>(fetch_limit)),
                    result);
            } else {
                connection.execute(
                    statements_.gm_audit_list_filtered(connection).bind(
                        action_filter.value_or(std::string{}),
                        action_filter.value_or(std::string{}),
                        operator_filter.value_or(std::string{}),
                        operator_filter.value_or(std::string{}),
                        success_filter.has_value() ? 1 : 0,
                        success_filter.value_or(false) ? 1 : 0,
                        static_cast<std::uint64_t>(fetch_limit)),
                    result);
            }
            page.items.reserve(result.rows().size());
            for (const auto row : result.rows()) {
                page.items.push_back(row_to_audit_log(row));
            }
        });

        if (page.items.size() > bounded_limit) {
            page.items.resize(bounded_limit);
            page.has_more = true;
            const auto& oldest = page.items.back();
            page.next_created_at = to_unix_seconds(oldest.created_at.value_or(std::chrono::system_clock::time_point{}));
            page.next_audit_id = oldest.audit_id;
        }
        return page;
    }

private:
    std::shared_ptr<MySqlClient> client_;
    mutable StatementCache statements_;
};

class MySqlAdminRepository final : public mf::game::AdminRepository {
public:
    explicit MySqlAdminRepository(std::shared_ptr<MySqlClient> client)
        : client_(std::move(client)) {}

    mf::game::AdminPrincipal upsert_admin(
        const std::string& account_id,
        std::string operator_name,
        bool active,
        std::vector<std::string> permissions) override {
        client_->with_connection(__func__, [&](mysql::any_connection& connection) {
            mysql::results result;
            connection.execute("START TRANSACTION", result);
            try {
                connection.execute(
                    statements_.admin_upsert(connection).bind(account_id, operator_name, active ? 1 : 0),
                    result);
                connection.execute(statements_.admin_delete_permissions(connection).bind(account_id), result);
                for (const auto& permission : permissions) {
                    connection.execute(
                        statements_.admin_insert_permission(connection).bind(account_id, permission),
                        result);
                }
                connection.execute("COMMIT", result);
            } catch (...) {
                try {
                    connection.execute("ROLLBACK", result);
                } catch (...) {
                }
                throw;
            }
        });

        return find_by_account_id(account_id).value_or(mf::game::AdminPrincipal{
            .account_id = account_id,
            .operator_name = std::move(operator_name),
            .active = active,
            .permissions = std::move(permissions),
        });
    }

    std::optional<mf::game::AdminPrincipal> find_by_account_id(const std::string& account_id) const override {
        std::optional<mf::game::AdminPrincipal> admin;
        client_->with_connection(__func__, [&](mysql::any_connection& connection) {
            mysql::results result;
            connection.execute(statements_.admin_find_by_account_id(connection).bind(account_id), result);
            if (result.rows().empty()) {
                return;
            }

            mysql::results permission_result;
            connection.execute(statements_.admin_list_permissions(connection).bind(account_id), permission_result);
            std::vector<std::string> permissions;
            permissions.reserve(permission_result.rows().size());
            for (const auto row : permission_result.rows()) {
                permissions.push_back(column_string(row, 0));
            }
            admin = row_to_admin_principal(result.rows().at(0), std::move(permissions));
        });
        return admin;
    }

    std::vector<mf::game::AdminPrincipal> list_all() const override {
        std::vector<mf::game::AdminPrincipal> items;
        client_->with_connection(__func__, [&](mysql::any_connection& connection) {
            mysql::results result;
            connection.execute(statements_.admin_list_all(connection).bind(), result);
            items.reserve(result.rows().size());
            for (const auto row : result.rows()) {
                const auto account_id = column_string(row, 0);
                mysql::results permission_result;
                connection.execute(statements_.admin_list_permissions(connection).bind(account_id), permission_result);
                std::vector<std::string> permissions;
                permissions.reserve(permission_result.rows().size());
                for (const auto permission_row : permission_result.rows()) {
                    permissions.push_back(column_string(permission_row, 0));
                }
                items.push_back(row_to_admin_principal(row, std::move(permissions)));
            }
        });
        return items;
    }

    // 管理员列表也单独做数据库计数，
    // 避免数据量上来后 total 只能靠“页大小 + offset”近似推断。
    std::size_t count_filtered(
        const std::optional<std::string>& operator_name_filter,
        const std::optional<bool>& active_filter) const override {
        std::size_t total = 0;
        client_->with_connection(__func__, [&](mysql::any_connection& connection) {
            mysql::results result;
            connection.execute(
                statements_.admin_count_filtered(connection).bind(
                    operator_name_filter.value_or(std::string{}),
                    operator_name_filter.value_or(std::string{}),
                    active_filter.has_value() ? 1 : 0,
                    active_filter.value_or(false) ? 1 : 0),
                result);
            if (!result.rows().empty()) {
                total = static_cast<std::size_t>(column_i64(result.rows().at(0), 0));
            }
        });
        return total;
    }

    std::vector<mf::game::AdminPrincipal> list_filtered(
        std::size_t limit,
        std::size_t offset,
        const std::optional<std::string>& operator_name_filter,
        const std::optional<bool>& active_filter) const override {
        std::vector<mf::game::AdminPrincipal> items;
        client_->with_connection(__func__, [&](mysql::any_connection& connection) {
            mysql::results result;
            connection.execute(
                statements_.admin_list_filtered(connection).bind(
                    operator_name_filter.value_or(std::string{}),
                    operator_name_filter.value_or(std::string{}),
                    active_filter.has_value() ? 1 : 0,
                    active_filter.value_or(false) ? 1 : 0,
                    static_cast<std::uint64_t>(limit),
                    static_cast<std::uint64_t>(offset)),
                result);
            items.reserve(result.rows().size());
            for (const auto row : result.rows()) {
                const auto account_id = column_string(row, 0);
                mysql::results permission_result;
                connection.execute(statements_.admin_list_permissions(connection).bind(account_id), permission_result);
                std::vector<std::string> permissions;
                permissions.reserve(permission_result.rows().size());
                for (const auto permission_row : permission_result.rows()) {
                    permissions.push_back(column_string(permission_row, 0));
                }
                items.push_back(row_to_admin_principal(row, std::move(permissions)));
            }
        });
        return items;
    }

    bool has_permission(const std::string& account_id, const std::string& permission) const override {
        bool allowed = false;
        client_->with_connection(__func__, [&](mysql::any_connection& connection) {
            mysql::results admin_result;
            connection.execute(statements_.admin_find_by_account_id(connection).bind(account_id), admin_result);
            if (admin_result.rows().empty() || column_i64(admin_result.rows().at(0), 2) == 0) {
                allowed = false;
                return;
            }

            mysql::results permission_result;
            connection.execute(
                statements_.admin_has_permission(connection).bind(account_id, permission),
                permission_result);
            allowed = !permission_result.rows().empty();
        });
        return allowed;
    }

private:
    std::shared_ptr<MySqlClient> client_;
    mutable StatementCache statements_;
};

class MySqlAdminTaskRepository final : public mf::game::AdminTaskRepository {
public:
    explicit MySqlAdminTaskRepository(std::shared_ptr<MySqlClient> client)
        : client_(std::move(client)) {}

    mf::game::AdminTask save(mf::game::AdminTask task) override {
        if (task.task_id.empty()) {
            task.task_id = make_audit_id("admin_task", task.trace_id.empty() ? task.target_id : task.trace_id);
        }
        const auto execute_at = task.execute_at ? std::optional<std::int64_t>{to_unix_seconds(*task.execute_at)} : std::nullopt;
        const auto executed_at = task.executed_at ? std::optional<std::int64_t>{to_unix_seconds(*task.executed_at)} : std::nullopt;
        client_->with_connection(__func__, [&](mysql::any_connection& connection) {
            mysql::results result;
            connection.execute(
                statements_.admin_task_upsert(connection).bind(
                    task.task_id,
                    admin_task_kind_to_string(task.kind),
                    task.status,
                    task.operator_id,
                    task.target_type,
                    task.target_id,
                    task.request_ip,
                    task.user_agent,
                    task.trace_id,
                    task.request_json.empty() ? std::string{"{}"} : task.request_json,
                    task.result_json.empty() ? std::optional<std::string>{std::nullopt}
                                             : std::optional<std::string>{task.result_json},
                    task.failure_reason,
                    execute_at,
                    executed_at),
                result);
        });
        return find_by_id(task.task_id).value_or(task);
    }

    mf::game::AdminTaskPage list_recent(
        std::size_t limit,
        const std::optional<mf::game::AdminTaskCursor>& cursor,
        const std::optional<mf::game::AdminTaskKind>& kind,
        const std::optional<std::string>& status) const override {
        mf::game::AdminTaskPage page;
        const auto bounded_limit = std::clamp<std::size_t>(limit, 1, 500);
        const auto fetch_limit = bounded_limit + 1;
        client_->with_connection(__func__, [&](mysql::any_connection& connection) {
            mysql::results result;
            if (cursor) {
                connection.execute(
                    statements_.admin_task_list_filtered_cursor(connection).bind(
                        kind ? std::string{admin_task_kind_to_string(*kind)} : std::string{},
                        kind ? std::string{admin_task_kind_to_string(*kind)} : std::string{},
                        status.value_or(std::string{}),
                        status.value_or(std::string{}),
                        cursor->last_created_at,
                        cursor->last_created_at,
                        cursor->last_task_id,
                        static_cast<std::uint64_t>(fetch_limit)),
                    result);
            } else {
                connection.execute(
                    statements_.admin_task_list_filtered(connection).bind(
                        kind ? std::string{admin_task_kind_to_string(*kind)} : std::string{},
                        kind ? std::string{admin_task_kind_to_string(*kind)} : std::string{},
                        status.value_or(std::string{}),
                        status.value_or(std::string{}),
                        static_cast<std::uint64_t>(fetch_limit)),
                    result);
            }
            page.items.reserve(result.rows().size());
            for (const auto row : result.rows()) {
                page.items.push_back(row_to_admin_task(row));
            }
        });

        if (page.items.size() > bounded_limit) {
            page.items.resize(bounded_limit);
            page.has_more = true;
            const auto& oldest = page.items.back();
            page.next_created_at = to_unix_seconds(oldest.created_at.value_or(std::chrono::system_clock::time_point{}));
            page.next_task_id = oldest.task_id;
        }
        return page;
    }

    std::size_t count_filtered(
        const std::optional<mf::game::AdminTaskKind>& kind,
        const std::optional<std::string>& status) const override {
        std::size_t total = 0;
        client_->with_connection(__func__, [&](mysql::any_connection& connection) {
            mysql::results result;
            connection.execute(
                statements_.admin_task_count_filtered(connection).bind(
                    kind ? std::string{admin_task_kind_to_string(*kind)} : std::string{},
                    kind ? std::string{admin_task_kind_to_string(*kind)} : std::string{},
                    status.value_or(std::string{}),
                    status.value_or(std::string{})),
                result);
            if (!result.rows().empty()) {
                total = static_cast<std::size_t>(column_i64(result.rows().at(0), 0));
            }
        });
        return total;
    }

    std::optional<mf::game::AdminTask> find_by_id(const std::string& task_id) const override {
        std::optional<mf::game::AdminTask> task;
        client_->with_connection(__func__, [&](mysql::any_connection& connection) {
            mysql::results result;
            connection.execute(statements_.admin_task_find_by_id(connection).bind(task_id), result);
            if (!result.rows().empty()) {
                task = row_to_admin_task(result.rows().at(0));
            }
        });
        return task;
    }

    std::vector<mf::game::AdminTask> claim_due_tasks(
        mf::game::AdminTaskKind kind,
        std::chrono::system_clock::time_point now,
        std::size_t limit) override {
        std::vector<mf::game::AdminTask> items;
        client_->with_connection(__func__, [&](mysql::any_connection& connection) {
            mysql::results result;
            connection.execute(
                statements_.admin_task_claim_due(connection).bind(
                    admin_task_kind_to_string(kind),
                    to_unix_seconds(now),
                    static_cast<std::uint64_t>(limit)),
                result);
            for (const auto row : result.rows()) {
                const auto task = row_to_admin_task(row);
                mysql::results update_result;
                connection.execute(statements_.admin_task_mark_running(connection).bind(task.task_id), update_result);
                items.push_back(task);
            }
        });
        return items;
    }

    std::optional<mf::game::AdminTask> mark_completed(
        const std::string& task_id,
        std::string result_json,
        std::chrono::system_clock::time_point executed_at) override {
        client_->with_connection(__func__, [&](mysql::any_connection& connection) {
            mysql::results result;
            connection.execute(
                statements_.admin_task_mark_completed(connection).bind(
                    result_json.empty() ? std::string{"{}"} : result_json,
                    to_unix_seconds(executed_at),
                    task_id),
                result);
        });
        return find_by_id(task_id);
    }

    std::optional<mf::game::AdminTask> mark_failed(
        const std::string& task_id,
        std::string failure_reason,
        std::string result_json,
        std::chrono::system_clock::time_point executed_at) override {
        client_->with_connection(__func__, [&](mysql::any_connection& connection) {
            mysql::results result;
            connection.execute(
                statements_.admin_task_mark_failed(connection).bind(
                    failure_reason,
                    result_json.empty() ? std::string{"{}"} : result_json,
                    to_unix_seconds(executed_at),
                    task_id),
                result);
        });
        return find_by_id(task_id);
    }

private:
    std::shared_ptr<MySqlClient> client_;
    mutable StatementCache statements_;
};

}  // 匿名命名空间

void configure_mysql_observability(
    std::shared_ptr<mf::core::Logger> logger,
    std::shared_ptr<mf::core::MetricsRegistry> metrics,
    std::int64_t slow_operation_threshold_ms) {
    std::scoped_lock lock{g_mysql_observability_mutex};
    g_mysql_observability.logger = std::move(logger);
    g_mysql_observability.metrics = std::move(metrics);
    g_mysql_observability.slow_operation_threshold_ms = std::max<std::int64_t>(1, slow_operation_threshold_ms);
}

std::shared_ptr<mf::game::AccountRepository> make_mysql_account_repository(const MySqlOptions& options) {
    return std::make_shared<MySqlAccountRepository>(shared_mysql_client(options));
}

std::shared_ptr<mf::agent::AgentRepository> make_mysql_agent_repository(const MySqlOptions& options) {
    return std::make_shared<MySqlAgentRepository>(shared_mysql_client(options));
}

std::shared_ptr<mf::game::PlayerRepository> make_mysql_player_repository(const MySqlOptions& options) {
    return std::make_shared<MySqlPlayerRepository>(shared_mysql_client(options));
}

std::shared_ptr<mf::game::ItemCatalogRepository> make_mysql_item_catalog_repository(const MySqlOptions& options) {
    return std::make_shared<MySqlItemCatalogRepository>(shared_mysql_client(options));
}

std::shared_ptr<mf::game::GachaRepository> make_mysql_gacha_repository(const MySqlOptions& options) {
    return std::make_shared<MySqlGachaRepository>(shared_mysql_client(options));
}

std::shared_ptr<mf::game::GachaTransactionRepository> make_mysql_gacha_transaction_repository(
    const MySqlOptions& options) {
    return std::make_shared<MySqlGachaTransactionRepository>(shared_mysql_client(options));
}

std::shared_ptr<mf::game::ShopRepository> make_mysql_shop_repository(const MySqlOptions& options) {
    return std::make_shared<MySqlShopRepository>(shared_mysql_client(options));
}

std::shared_ptr<mf::game::LootRepository> make_mysql_loot_repository(const MySqlOptions& options) {
    return std::make_shared<MySqlLootRepository>(shared_mysql_client(options));
}

std::shared_ptr<mf::game::RewardTransactionRepository> make_mysql_reward_transaction_repository(
    const MySqlOptions& options) {
    return std::make_shared<MySqlRewardTransactionRepository>(shared_mysql_client(options));
}

std::shared_ptr<mf::game::LootTransactionRepository> make_mysql_loot_transaction_repository(
    const MySqlOptions& options) {
    return std::make_shared<MySqlLootTransactionRepository>(shared_mysql_client(options));
}

std::shared_ptr<mf::game::ShopTransactionRepository> make_mysql_shop_transaction_repository(
    const MySqlOptions& options) {
    return std::make_shared<MySqlShopTransactionRepository>(shared_mysql_client(options));
}

std::shared_ptr<mf::game::QuestRepository> make_mysql_quest_repository(const MySqlOptions& options) {
    return std::make_shared<MySqlQuestRepository>(shared_mysql_client(options));
}

std::shared_ptr<mf::multiplayer::MultiplayerRoomRepository> make_mysql_multiplayer_room_repository(
    const MySqlOptions& options) {
    return std::make_shared<MySqlMultiplayerRoomRepository>(shared_mysql_client(options));
}

std::shared_ptr<mf::voice::VoiceRepository> make_mysql_voice_repository(const MySqlOptions& options) {
    return std::make_shared<MySqlVoiceRepository>(shared_mysql_client(options));
}

std::shared_ptr<mf::game::FriendRepository> make_mysql_friend_repository(const MySqlOptions& options) {
    return std::make_shared<MySqlFriendRepository>(shared_mysql_client(options));
}

std::shared_ptr<mf::game::ChatRepository> make_mysql_chat_repository(const MySqlOptions& options) {
    return std::make_shared<MySqlChatRepository>(shared_mysql_client(options));
}

std::shared_ptr<mf::game::ChatModerationRepository> make_mysql_chat_moderation_repository(const MySqlOptions& options) {
    return std::make_shared<MySqlChatModerationRepository>(shared_mysql_client(options));
}

std::shared_ptr<mf::game::RewardRepository> make_mysql_reward_repository(const MySqlOptions& options) {
    return std::make_shared<MySqlRewardRepository>(shared_mysql_client(options));
}

std::shared_ptr<mf::game::RewardTemplateRepository> make_mysql_reward_template_repository(const MySqlOptions& options) {
    return std::make_shared<MySqlRewardTemplateRepository>(shared_mysql_client(options));
}

std::shared_ptr<mf::game::AnnouncementRepository> make_mysql_announcement_repository(const MySqlOptions& options) {
    return std::make_shared<MySqlAnnouncementRepository>(shared_mysql_client(options));
}

std::shared_ptr<mf::game::MailRepository> make_mysql_mail_repository(const MySqlOptions& options) {
    return std::make_shared<MySqlMailRepository>(shared_mysql_client(options));
}

std::shared_ptr<mf::game::AuditLogRepository> make_mysql_audit_log_repository(const MySqlOptions& options) {
    return std::make_shared<MySqlAuditLogRepository>(shared_mysql_client(options));
}

std::shared_ptr<mf::game::AdminRepository> make_mysql_admin_repository(const MySqlOptions& options) {
    return std::make_shared<MySqlAdminRepository>(shared_mysql_client(options));
}

std::shared_ptr<mf::game::AdminTaskRepository> make_mysql_admin_task_repository(const MySqlOptions& options) {
    return std::make_shared<MySqlAdminTaskRepository>(shared_mysql_client(options));
}

}  // namespace mf::storage 命名空间
