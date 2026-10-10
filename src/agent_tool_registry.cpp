#include "mf/agent/tool_registry.hpp"

#include "mf/game/json.hpp"

#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>

#include <algorithm>
#include <array>

// 文件职责：构造只读权威上下文，并把自然语言中的低风险意图转换成受控客户端动作。

namespace mf::agent {
namespace {

bool contains_any(const std::string& text, std::initializer_list<std::string_view> values) {
    return std::any_of(values.begin(), values.end(), [&](std::string_view value) {
        return text.find(value) != std::string::npos;
    });
}

std::string make_call_id(const std::string& conversation_id, const std::string& request_id, const std::string& tool_name) {
    return "agent_call_" + std::to_string(std::hash<std::string>{}(conversation_id + '|' + request_id + '|' + tool_name));
}

}  // namespace

AgentToolRegistry::AgentToolRegistry(
    mf::game::RepositoryBundle repositories,
    std::shared_ptr<mf::game::QuestService> quest_service)
    : repositories_(std::move(repositories)), quest_service_(std::move(quest_service)) {}

// 只读取业务仓储，压缩成模型可理解的 JSON；不把整个数据库或原始 Token 交给模型。
std::string AgentToolRegistry::build_authoritative_context(const std::string& player_id) const {
    boost::json::object context;
    if (const auto player = repositories_.players->find_by_player_id(player_id)) {
        context["player"] = mf::game::to_json(*player);
    }
    if (quest_service_) context["quests"] = mf::game::to_json(quest_service_->get_snapshot(player_id));

    boost::json::array unread_mail;
    for (const auto& mail : repositories_.mails->list_inbox(player_id, false)) {
        if (!mail.read && unread_mail.size() < 10) {
            unread_mail.push_back(boost::json::object{{"mail_id", mail.mail_id}, {"title", mail.title}});
        }
    }
    context["unread_mail"] = std::move(unread_mail);

    boost::json::array friends;
    for (const auto& item : repositories_.friends->list_friends(player_id)) {
        if (friends.size() >= 30) break;
        friends.push_back(mf::game::to_json(item));
    }
    context["friends"] = std::move(friends);
    return boost::json::serialize(context);
}

std::optional<AgentToolCall> AgentToolRegistry::suggest_tool(
    const std::string& player_id,
    const std::string& conversation_id,
    const std::string& message_id,
    const std::string& client_request_id,
    const std::string& user_message) const {
    std::string tool_name;
    boost::json::object arguments;
    std::string display_text;

    if (contains_any(user_message, {"打开背包", "查看背包", "open inventory"})) {
        tool_name = "ui.open_panel";
        arguments = {{"panel", "inventory"}};
        display_text = "打开背包";
    } else if (contains_any(user_message, {"打开任务", "任务面板", "open quest"})) {
        tool_name = "ui.open_panel";
        arguments = {{"panel", "quest"}};
        display_text = "打开任务面板";
    } else if (contains_any(user_message, {"打开邮箱", "查看邮件", "open mail"})) {
        tool_name = "ui.open_panel";
        arguments = {{"panel", "mail"}};
        display_text = "打开邮箱";
    } else if (contains_any(user_message, {"打开地图", "open map"})) {
        tool_name = "ui.open_panel";
        arguments = {{"panel", "map"}};
        display_text = "打开地图";
    } else if (contains_any(user_message, {"开火", "fire"})) {
        tool_name = "fps.command_squad";
        arguments = {{"command", "Fire"}};
        display_text = "命令 AI 队友开火";
    } else if (contains_any(user_message, {"撤退", "retreat"})) {
        tool_name = "fps.command_squad";
        arguments = {{"command", "Retreat"}};
        display_text = "命令 AI 队友撤退";
    } else if (contains_any(user_message, {"跟随", "follow"})) {
        tool_name = "fps.command_squad";
        arguments = {{"command", "Follow"}};
        display_text = "命令 AI 队友跟随";
    } else {
        return std::nullopt;
    }

    return AgentToolCall{
        .call_id = make_call_id(conversation_id, client_request_id, tool_name),
        .conversation_id = conversation_id,
        .message_id = message_id,
        .player_id = player_id,
        .tool_name = tool_name,
        .arguments_json = boost::json::serialize(arguments),
        .risk_level = tool_name.starts_with("fps.") ? "medium" : "low",
        .status = "requires_confirmation",
        .display_text = std::move(display_text),
        .idempotency_key = client_request_id,
    };
}

std::optional<boost::json::object> AgentToolRegistry::execute_client_tool(
    const AgentToolCall& tool_call,
    std::string& result_text,
    std::string& error_code) const {
    if (!is_registered(tool_call.tool_name)) {
        error_code = "agent_tool_forbidden";
        return std::nullopt;
    }
    boost::json::object arguments;
    try {
        arguments = boost::json::parse(tool_call.arguments_json).as_object();
    } catch (...) {
        error_code = "agent_invalid_tool_arguments";
        return std::nullopt;
    }
    if (tool_call.tool_name == "ui.open_panel") {
        const auto* panel = arguments.if_contains("panel");
        if (!panel || !panel->is_string()) {
            error_code = "agent_invalid_tool_arguments";
            return std::nullopt;
        }
        result_text = "已批准打开界面";
        return boost::json::object{{"type", "open_panel"}, {"payload", arguments}};
    }
    if (tool_call.tool_name == "fps.command_squad") {
        const auto* command = arguments.if_contains("command");
        if (!command || !command->is_string()) {
            error_code = "agent_invalid_tool_arguments";
            return std::nullopt;
        }
        result_text = "已批准队伍战术命令";
        return boost::json::object{{"type", "fps_command_squad"}, {"payload", arguments}};
    }
    error_code = "agent_tool_forbidden";
    return std::nullopt;
}

bool AgentToolRegistry::is_registered(std::string_view tool_name) const {
    return tool_name == "ui.open_panel" || tool_name == "fps.command_squad";
}

}  // namespace mf::agent
