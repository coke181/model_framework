#pragma once

#include "hello_admin_tasks.hpp"

#include "mf/agent/service.hpp"
#include "mf/agent/task_service.hpp"
#include "mf/auth/account_service.hpp"
#include "mf/auth/token_service.hpp"
#include "mf/chat/chat_service.hpp"
#include "mf/chat/gateway.hpp"
#include "mf/core/application.hpp"
#include "mf/core/env.hpp"
#include "mf/core/metrics.hpp"
#include "mf/core/tracing.hpp"
#include "mf/game/gacha_service.hpp"
#include "mf/game/loot_service.hpp"
#include "mf/game/quest_service.hpp"
#include "mf/game/mail_service.hpp"
#include "mf/game/repositories.hpp"
#include "mf/game/services.hpp"
#include "mf/game/shop_service.hpp"
#include "mf/multiplayer/service.hpp"
#include "mf/storage/options.hpp"
#include "mf/voice/service.hpp"

#include <memory>
#include <string>

void register_system_routes(
    mf::core::Application& app,
    const mf::game::RepositoryBundle& repositories,
    const mf::chat::GatewayOptions& chat_options,
    const mf::storage::StorageOptions& storage_options,
    const std::shared_ptr<mf::core::MetricsRegistry>& metrics,
    const std::shared_ptr<mf::core::TracerProvider>& tracer,
    const std::shared_ptr<mf::http::LoadProbe>& load_probe,
    mf::core::RuntimeEnvironment runtime_environment);

void register_agent_routes(
    mf::core::Application& app,
    const std::shared_ptr<mf::auth::TokenService>& token_service,
    const std::shared_ptr<mf::agent::AgentService>& agent_service,
    const std::shared_ptr<mf::agent::AgentTaskService>& agent_task_service);

void register_admin_task_routes(
    mf::core::Application& app,
    const mf::game::RepositoryBundle& repositories,
    const std::shared_ptr<mf::auth::TokenService>& token_service,
    const std::shared_ptr<AdminTaskScheduler>& admin_task_scheduler);

void register_auth_routes(
    mf::core::Application& app,
    const mf::game::RepositoryBundle& repositories,
    const std::shared_ptr<mf::auth::TokenService>& token_service,
    const std::shared_ptr<mf::auth::AccountService>& account_service,
    mf::core::RuntimeEnvironment runtime_environment);

void register_admin_entry_routes(
    mf::core::Application& app,
    const mf::game::RepositoryBundle& repositories,
    const std::shared_ptr<mf::auth::TokenService>& token_service,
    const std::shared_ptr<mf::auth::AccountService>& account_service,
    const std::string& bootstrap_token);

void register_admin_user_routes(
    mf::core::Application& app,
    const mf::game::RepositoryBundle& repositories,
    const std::shared_ptr<mf::auth::TokenService>& token_service,
    const std::shared_ptr<mf::auth::AccountService>& account_service);

void register_reward_routes(
    mf::core::Application& app,
    const mf::game::RepositoryBundle& repositories,
    const std::shared_ptr<mf::auth::TokenService>& token_service,
    const std::shared_ptr<mf::game::RewardService>& reward_service,
    const std::shared_ptr<mf::game::MailService>& mail_service,
    const std::shared_ptr<AdminTaskScheduler>& admin_task_scheduler);

void register_gacha_routes(
    mf::core::Application& app,
    const mf::game::RepositoryBundle& repositories,
    const std::shared_ptr<mf::auth::TokenService>& token_service,
    const std::shared_ptr<mf::game::GachaService>& gacha_service);

void register_shop_routes(
    mf::core::Application& app,
    const mf::game::RepositoryBundle& repositories,
    const std::shared_ptr<mf::auth::TokenService>& token_service,
    const std::shared_ptr<mf::game::ShopService>& shop_service);

void register_loot_routes(
    mf::core::Application& app,
    const mf::game::RepositoryBundle& repositories,
    const std::shared_ptr<mf::auth::TokenService>& token_service,
    const std::shared_ptr<mf::game::LootService>& loot_service);

void register_quest_routes(
    mf::core::Application& app,
    const mf::game::RepositoryBundle& repositories,
    const std::shared_ptr<mf::auth::TokenService>& token_service,
    const std::shared_ptr<mf::game::QuestService>& quest_service);

void register_voice_routes(
    mf::core::Application& app,
    const mf::game::RepositoryBundle& repositories,
    const std::shared_ptr<mf::auth::TokenService>& token_service,
    const std::shared_ptr<mf::voice::VoiceService>& voice_service);

void register_multiplayer_routes(
    mf::core::Application& app,
    const std::shared_ptr<mf::auth::TokenService>& token_service,
    const std::shared_ptr<mf::multiplayer::MultiplayerRoomService>& multiplayer_service);

void register_admin_audit_routes(
    mf::core::Application& app,
    const mf::game::RepositoryBundle& repositories,
    const std::shared_ptr<mf::auth::TokenService>& token_service);

void register_admin_query_routes(
    mf::core::Application& app,
    const mf::game::RepositoryBundle& repositories,
    const std::shared_ptr<mf::auth::TokenService>& token_service,
    const std::shared_ptr<mf::auth::AccountService>& account_service,
    const std::shared_ptr<mf::game::RewardService>& reward_service,
    const std::shared_ptr<mf::game::MailService>& mail_service,
    const std::shared_ptr<mf::chat::ChatService>& chat_service);

void register_admin_chat_routes(
    mf::core::Application& app,
    const mf::game::RepositoryBundle& repositories,
    const std::shared_ptr<mf::auth::TokenService>& token_service,
    const std::shared_ptr<mf::chat::ChatService>& chat_service);

void register_player_routes(
    mf::core::Application& app,
    const mf::game::RepositoryBundle& repositories,
    const std::shared_ptr<mf::auth::TokenService>& token_service);

void register_mail_routes(
    mf::core::Application& app,
    const mf::game::RepositoryBundle& repositories,
    const std::shared_ptr<mf::game::MailService>& mail_service,
    const std::shared_ptr<mf::auth::TokenService>& token_service);

void register_friend_routes(
    mf::core::Application& app,
    const mf::game::RepositoryBundle& repositories,
    const std::shared_ptr<mf::chat::ChatService>& chat_service,
    const std::shared_ptr<mf::chat::ChatStateService>& chat_state_service,
    const std::shared_ptr<mf::auth::TokenService>& token_service);

void register_chat_routes(
    mf::core::Application& app,
    const mf::game::RepositoryBundle& repositories,
    const std::shared_ptr<mf::chat::ChatService>& chat_service,
    const std::shared_ptr<mf::chat::ChatStateService>& chat_state_service,
    const std::shared_ptr<mf::auth::TokenService>& token_service);
