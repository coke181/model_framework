-- 文件职责：
-- 定义当前游戏业务后端的 MySQL 初始表结构。
-- 调用流程：
-- 部署时先执行本文件初始化 schema ->
-- 关系型仓储把账号、玩家、好友、聊天消息持久化到这些表 ->
-- Redis 只承担会话和热点缓存，不替代这些事实表。

CREATE TABLE IF NOT EXISTS account (
    account_id      VARCHAR(64) PRIMARY KEY,
    login_name      VARCHAR(128) NOT NULL UNIQUE,
    display_name    VARCHAR(128) NOT NULL,
    email           VARCHAR(256) NOT NULL DEFAULT '',
    phone_number    VARCHAR(64) NOT NULL DEFAULT '',
    platform_channel VARCHAR(64) NOT NULL DEFAULT 'native',
    register_source VARCHAR(64) NOT NULL DEFAULT 'direct_register',
    auth_mode       VARCHAR(32) NOT NULL DEFAULT 'quick',
    status          VARCHAR(32) NOT NULL DEFAULT 'active',
    status_reason   VARCHAR(255) NOT NULL DEFAULT '',
    failed_login_attempts BIGINT NOT NULL DEFAULT 0,
    locked_until    TIMESTAMP NULL DEFAULT NULL,
    last_login_at   TIMESTAMP NULL DEFAULT NULL,
    last_login_ip   VARCHAR(64) NOT NULL DEFAULT '',
    password_hash   TEXT NULL,
    password_scheme VARCHAR(32) NULL,
    password_updated_at TIMESTAMP NULL DEFAULT NULL,
    reset_token     VARCHAR(128) NULL,
    reset_token_expires_at TIMESTAMP NULL DEFAULT NULL,
    created_at      TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at      TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP
);

SET @account_add_auth_mode = (
    SELECT IF(
        EXISTS(
            SELECT 1
            FROM information_schema.columns
            WHERE table_schema = DATABASE() AND table_name = 'account' AND column_name = 'auth_mode'
        ),
        'SELECT 1',
        'ALTER TABLE account ADD COLUMN auth_mode VARCHAR(32) NOT NULL DEFAULT ''quick'''
    )
);
PREPARE stmt FROM @account_add_auth_mode;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

SET @account_add_password_hash = (
    SELECT IF(
        EXISTS(
            SELECT 1
            FROM information_schema.columns
            WHERE table_schema = DATABASE() AND table_name = 'account' AND column_name = 'password_hash'
        ),
        'SELECT 1',
        'ALTER TABLE account ADD COLUMN password_hash TEXT NULL'
    )
);
PREPARE stmt FROM @account_add_password_hash;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

SET @account_add_email = (
    SELECT IF(
        EXISTS(
            SELECT 1 FROM information_schema.columns
            WHERE table_schema = DATABASE() AND table_name = 'account' AND column_name = 'email'
        ),
        'SELECT 1',
        'ALTER TABLE account ADD COLUMN email VARCHAR(256) NOT NULL DEFAULT '''''
    )
);
PREPARE stmt FROM @account_add_email;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

SET @account_add_phone_number = (
    SELECT IF(
        EXISTS(
            SELECT 1 FROM information_schema.columns
            WHERE table_schema = DATABASE() AND table_name = 'account' AND column_name = 'phone_number'
        ),
        'SELECT 1',
        'ALTER TABLE account ADD COLUMN phone_number VARCHAR(64) NOT NULL DEFAULT '''''
    )
);
PREPARE stmt FROM @account_add_phone_number;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

SET @account_add_platform_channel = (
    SELECT IF(
        EXISTS(
            SELECT 1 FROM information_schema.columns
            WHERE table_schema = DATABASE() AND table_name = 'account' AND column_name = 'platform_channel'
        ),
        'SELECT 1',
        'ALTER TABLE account ADD COLUMN platform_channel VARCHAR(64) NOT NULL DEFAULT ''native'''
    )
);
PREPARE stmt FROM @account_add_platform_channel;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

SET @account_add_register_source = (
    SELECT IF(
        EXISTS(
            SELECT 1 FROM information_schema.columns
            WHERE table_schema = DATABASE() AND table_name = 'account' AND column_name = 'register_source'
        ),
        'SELECT 1',
        'ALTER TABLE account ADD COLUMN register_source VARCHAR(64) NOT NULL DEFAULT ''direct_register'''
    )
);
PREPARE stmt FROM @account_add_register_source;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

SET @account_add_password_scheme = (
    SELECT IF(
        EXISTS(
            SELECT 1
            FROM information_schema.columns
            WHERE table_schema = DATABASE() AND table_name = 'account' AND column_name = 'password_scheme'
        ),
        'SELECT 1',
        'ALTER TABLE account ADD COLUMN password_scheme VARCHAR(32) NULL'
    )
);
PREPARE stmt FROM @account_add_password_scheme;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

SET @account_add_password_updated_at = (
    SELECT IF(
        EXISTS(
            SELECT 1
            FROM information_schema.columns
            WHERE table_schema = DATABASE() AND table_name = 'account' AND column_name = 'password_updated_at'
        ),
        'SELECT 1',
        'ALTER TABLE account ADD COLUMN password_updated_at TIMESTAMP NULL DEFAULT NULL'
    )
);
PREPARE stmt FROM @account_add_password_updated_at;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

SET @account_add_status = (
    SELECT IF(
        EXISTS(
            SELECT 1 FROM information_schema.columns
            WHERE table_schema = DATABASE() AND table_name = 'account' AND column_name = 'status'
        ),
        'SELECT 1',
        'ALTER TABLE account ADD COLUMN status VARCHAR(32) NOT NULL DEFAULT ''active'''
    )
);
PREPARE stmt FROM @account_add_status;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

SET @account_add_status_reason = (
    SELECT IF(
        EXISTS(
            SELECT 1 FROM information_schema.columns
            WHERE table_schema = DATABASE() AND table_name = 'account' AND column_name = 'status_reason'
        ),
        'SELECT 1',
        'ALTER TABLE account ADD COLUMN status_reason VARCHAR(255) NOT NULL DEFAULT '''''
    )
);
PREPARE stmt FROM @account_add_status_reason;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

SET @account_add_failed_login_attempts = (
    SELECT IF(
        EXISTS(
            SELECT 1 FROM information_schema.columns
            WHERE table_schema = DATABASE() AND table_name = 'account' AND column_name = 'failed_login_attempts'
        ),
        'SELECT 1',
        'ALTER TABLE account ADD COLUMN failed_login_attempts BIGINT NOT NULL DEFAULT 0'
    )
);
PREPARE stmt FROM @account_add_failed_login_attempts;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

SET @account_add_locked_until = (
    SELECT IF(
        EXISTS(
            SELECT 1 FROM information_schema.columns
            WHERE table_schema = DATABASE() AND table_name = 'account' AND column_name = 'locked_until'
        ),
        'SELECT 1',
        'ALTER TABLE account ADD COLUMN locked_until TIMESTAMP NULL DEFAULT NULL'
    )
);
PREPARE stmt FROM @account_add_locked_until;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

SET @account_add_last_login_at = (
    SELECT IF(
        EXISTS(
            SELECT 1 FROM information_schema.columns
            WHERE table_schema = DATABASE() AND table_name = 'account' AND column_name = 'last_login_at'
        ),
        'SELECT 1',
        'ALTER TABLE account ADD COLUMN last_login_at TIMESTAMP NULL DEFAULT NULL'
    )
);
PREPARE stmt FROM @account_add_last_login_at;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

SET @account_add_last_login_ip = (
    SELECT IF(
        EXISTS(
            SELECT 1 FROM information_schema.columns
            WHERE table_schema = DATABASE() AND table_name = 'account' AND column_name = 'last_login_ip'
        ),
        'SELECT 1',
        'ALTER TABLE account ADD COLUMN last_login_ip VARCHAR(64) NOT NULL DEFAULT '''''
    )
);
PREPARE stmt FROM @account_add_last_login_ip;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

SET @account_add_reset_token = (
    SELECT IF(
        EXISTS(
            SELECT 1 FROM information_schema.columns
            WHERE table_schema = DATABASE() AND table_name = 'account' AND column_name = 'reset_token'
        ),
        'SELECT 1',
        'ALTER TABLE account ADD COLUMN reset_token VARCHAR(128) NULL'
    )
);
PREPARE stmt FROM @account_add_reset_token;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

SET @account_add_reset_token_expires_at = (
    SELECT IF(
        EXISTS(
            SELECT 1 FROM information_schema.columns
            WHERE table_schema = DATABASE() AND table_name = 'account' AND column_name = 'reset_token_expires_at'
        ),
        'SELECT 1',
        'ALTER TABLE account ADD COLUMN reset_token_expires_at TIMESTAMP NULL DEFAULT NULL'
    )
);
PREPARE stmt FROM @account_add_reset_token_expires_at;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

CREATE TABLE IF NOT EXISTS player_profile (
    player_id       VARCHAR(64) PRIMARY KEY,
    account_id      VARCHAR(64) NOT NULL UNIQUE,
    nickname        VARCHAR(128) NOT NULL,
    gold            BIGINT NOT NULL DEFAULT 0,
    gem             BIGINT NOT NULL DEFAULT 0,
    created_at      TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at      TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    CONSTRAINT fk_player_account FOREIGN KEY (account_id) REFERENCES account(account_id)
);

SET @player_profile_add_created_index = (
    SELECT IF(
        EXISTS(
            SELECT 1 FROM information_schema.statistics
            WHERE table_schema = DATABASE() AND table_name = 'player_profile' AND index_name = 'idx_player_created_time'
        ),
        'SELECT 1',
        'ALTER TABLE player_profile ADD INDEX idx_player_created_time (created_at, player_id)'
    )
);
PREPARE stmt FROM @player_profile_add_created_index;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

CREATE TABLE IF NOT EXISTS player_currency (
    player_id       VARCHAR(64) NOT NULL,
    currency_id     VARCHAR(64) NOT NULL,
    amount          BIGINT NOT NULL DEFAULT 0,
    updated_at      TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (player_id, currency_id),
    CONSTRAINT fk_player_currency_player FOREIGN KEY (player_id) REFERENCES player_profile(player_id) ON DELETE CASCADE
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS player_inventory (
    player_id       VARCHAR(64) NOT NULL,
    item_id         VARCHAR(96) NOT NULL,
    instance_id     VARCHAR(128) NOT NULL,
    quantity        BIGINT NOT NULL DEFAULT 0,
    updated_at      TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (player_id, item_id),
    CONSTRAINT fk_player_inventory_player FOREIGN KEY (player_id) REFERENCES player_profile(player_id) ON DELETE CASCADE
) ENGINE=InnoDB;

SET @player_profile_add_gem = (
    SELECT IF(
        EXISTS(
            SELECT 1 FROM information_schema.columns
            WHERE table_schema = DATABASE() AND table_name = 'player_profile' AND column_name = 'gem'
        ),
        'SELECT 1',
        'ALTER TABLE player_profile ADD COLUMN gem BIGINT NOT NULL DEFAULT 0'
    )
);
PREPARE stmt FROM @player_profile_add_gem;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

-- 从旧 currencies_json / inventory_json 迁移到行表；仅当旧列仍存在时执行，保证幂等。
SET @player_profile_backfill_currency = (
    SELECT IF(
        EXISTS(
            SELECT 1 FROM information_schema.columns
            WHERE table_schema = DATABASE() AND table_name = 'player_profile' AND column_name = 'currencies_json'
        ),
        'INSERT INTO player_currency(player_id, currency_id, amount) '
        'SELECT p.player_id, jt.currency_id, jt.amount FROM player_profile p '
        'JOIN JSON_TABLE(p.currencies_json, ''$[*]'' COLUMNS(currency_id VARCHAR(64) PATH ''$.currency_id'', amount BIGINT PATH ''$.amount'')) jt '
        'WHERE jt.currency_id IS NOT NULL AND jt.currency_id NOT IN (''gold'', ''gem'') '
        'ON DUPLICATE KEY UPDATE amount = VALUES(amount)',
        'SELECT 1'
    )
);
PREPARE stmt FROM @player_profile_backfill_currency;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

SET @player_profile_backfill_inventory = (
    SELECT IF(
        EXISTS(
            SELECT 1 FROM information_schema.columns
            WHERE table_schema = DATABASE() AND table_name = 'player_profile' AND column_name = 'inventory_json'
        ),
        'INSERT INTO player_inventory(player_id, item_id, instance_id, quantity) '
        'SELECT p.player_id, '
        'CASE WHEN JSON_TYPE(jt.raw) = ''STRING'' THEN JSON_UNQUOTE(jt.raw) ELSE JSON_UNQUOTE(JSON_EXTRACT(jt.raw, ''$.item_id'')) END, '
        'COALESCE(JSON_UNQUOTE(JSON_EXTRACT(jt.raw, ''$.instance_id'')), CONCAT(''legacy_'', CASE WHEN JSON_TYPE(jt.raw) = ''STRING'' THEN JSON_UNQUOTE(jt.raw) ELSE JSON_UNQUOTE(JSON_EXTRACT(jt.raw, ''$.item_id'')) END)), '
        'CASE WHEN JSON_TYPE(jt.raw) = ''STRING'' THEN 1 ELSE COALESCE(JSON_EXTRACT(jt.raw, ''$.quantity''), 1) END '
        'FROM player_profile p '
        'JOIN JSON_TABLE(p.inventory_json, ''$[*]'' COLUMNS(raw JSON PATH ''$'')) jt '
        'WHERE JSON_TYPE(jt.raw) = ''STRING'' OR JSON_EXTRACT(jt.raw, ''$.item_id'') IS NOT NULL '
        'ON DUPLICATE KEY UPDATE quantity = VALUES(quantity)',
        'SELECT 1'
    )
);
PREPARE stmt FROM @player_profile_backfill_inventory;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

SET @player_profile_drop_currencies_json = (
    SELECT IF(
        EXISTS(
            SELECT 1 FROM information_schema.columns
            WHERE table_schema = DATABASE() AND table_name = 'player_profile' AND column_name = 'currencies_json'
        ),
        'ALTER TABLE player_profile DROP COLUMN currencies_json',
        'SELECT 1'
    )
);
PREPARE stmt FROM @player_profile_drop_currencies_json;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

SET @player_profile_drop_inventory_json = (
    SELECT IF(
        EXISTS(
            SELECT 1 FROM information_schema.columns
            WHERE table_schema = DATABASE() AND table_name = 'player_profile' AND column_name = 'inventory_json'
        ),
        'ALTER TABLE player_profile DROP COLUMN inventory_json',
        'SELECT 1'
    )
);
PREPARE stmt FROM @player_profile_drop_inventory_json;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

CREATE TABLE IF NOT EXISTS item_definition (
    item_id      VARCHAR(96) PRIMARY KEY,
    name         VARCHAR(128) NOT NULL,
    description  TEXT NOT NULL,
    icon         VARCHAR(255) NOT NULL DEFAULT '',
    rarity       VARCHAR(32) NOT NULL DEFAULT '3星',
    max_stack    BIGINT NOT NULL DEFAULT 1,
    item_type    VARCHAR(32) NOT NULL DEFAULT 'material',
    enabled      TINYINT(1) NOT NULL DEFAULT 1,
    created_at   TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at   TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    INDEX idx_item_rarity (rarity),
    INDEX idx_item_enabled (enabled),
    INDEX idx_item_name (name)
);

CREATE TABLE IF NOT EXISTS friend_link (
    player_id          VARCHAR(64) NOT NULL,
    friend_player_id   VARCHAR(64) NOT NULL,
    status             VARCHAR(32) NOT NULL DEFAULT 'accepted',
    last_message_at    TIMESTAMP NULL DEFAULT NULL,
    last_message_epoch BIGINT NOT NULL DEFAULT 0,
    created_at         TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (player_id, friend_player_id),
    INDEX idx_friend_last_message_epoch (player_id, status, last_message_epoch DESC, friend_player_id)
);

CREATE TABLE IF NOT EXISTS chat_block (
    player_id          VARCHAR(64) NOT NULL,
    blocked_player_id  VARCHAR(64) NOT NULL,
    created_at         TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (player_id, blocked_player_id)
);

CREATE TABLE IF NOT EXISTS chat_mute (
    channel_id            VARCHAR(128) NOT NULL,
    muted_player_id       VARCHAR(64) NOT NULL,
    muted_by_player_id    VARCHAR(64) NOT NULL,
    reason                VARCHAR(255) NOT NULL DEFAULT '',
    muted_until           TIMESTAMP NULL DEFAULT NULL,
    created_at            TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at            TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (channel_id, muted_player_id)
);

CREATE TABLE IF NOT EXISTS chat_message (
    message_id         VARCHAR(64) PRIMARY KEY,
    channel_id         VARCHAR(128) NOT NULL,
    channel_kind       VARCHAR(32) NOT NULL,
    scope_id           VARCHAR(128) NOT NULL,
    message_kind       VARCHAR(32) NOT NULL,
    sender_player_id   VARCHAR(64) NOT NULL,
    sender_nickname    VARCHAR(128) NOT NULL,
    text_body          TEXT NOT NULL,
    recalled           TINYINT(1) NOT NULL DEFAULT 0,
    recalled_by_player_id VARCHAR(64) NULL,
    recalled_at        TIMESTAMP NULL DEFAULT NULL,
    sent_at            TIMESTAMP NOT NULL,
    INDEX idx_chat_channel_time (channel_id, sent_at),
    INDEX idx_chat_channel_latest (channel_id, sent_at DESC, message_id DESC)
);

-- 聊天 Outbox：先保存完整消息，再由聊天 worker 可恢复地写入 chat_message。
-- message_id 唯一，允许 worker 在网络超时或进程重启后安全重试。
CREATE TABLE IF NOT EXISTS chat_message_outbox (
    outbox_id          BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    message_id         VARCHAR(64) NOT NULL,
    channel_id         VARCHAR(128) NOT NULL,
    channel_kind       VARCHAR(32) NOT NULL,
    scope_id           VARCHAR(128) NOT NULL,
    message_kind       VARCHAR(32) NOT NULL,
    sender_player_id   VARCHAR(64) NOT NULL,
    sender_nickname    VARCHAR(128) NOT NULL,
    text_body          TEXT NOT NULL,
    sent_at            TIMESTAMP NOT NULL,
    attempts           INT UNSIGNED NOT NULL DEFAULT 0,
    status             VARCHAR(16) NOT NULL DEFAULT 'pending',
    next_attempt_at    TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    last_error         VARCHAR(512) NULL,
    created_at         TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (outbox_id),
    UNIQUE KEY uq_chat_outbox_message (message_id),
    INDEX idx_chat_outbox_pending (status, next_attempt_at, outbox_id)
);

-- 兼容已经提前创建过 Outbox 表的旧部署：CREATE TABLE 不会自动补列/索引。
SET @chat_outbox_add_status = (
    SELECT IF(
        EXISTS(
            SELECT 1 FROM information_schema.columns
            WHERE table_schema = DATABASE() AND table_name = 'chat_message_outbox' AND column_name = 'status'
        ),
        'SELECT 1',
        'ALTER TABLE chat_message_outbox ADD COLUMN status VARCHAR(16) NOT NULL DEFAULT ''pending'' AFTER attempts'
    )
);
PREPARE stmt FROM @chat_outbox_add_status;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

SET @chat_outbox_add_pending_index = (
    SELECT IF(
        EXISTS(
            SELECT 1 FROM information_schema.statistics
            WHERE table_schema = DATABASE() AND table_name = 'chat_message_outbox' AND index_name = 'idx_chat_outbox_pending'
        ),
        'SELECT 1',
        'ALTER TABLE chat_message_outbox ADD INDEX idx_chat_outbox_pending (status, next_attempt_at, outbox_id)'
    )
);
PREPARE stmt FROM @chat_outbox_add_pending_index;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

CREATE TABLE IF NOT EXISTS reward_grant (
    grant_id            VARCHAR(96) PRIMARY KEY,
    player_id           VARCHAR(64) NOT NULL,
    source_type         VARCHAR(64) NOT NULL,
    source_id           VARCHAR(128) NOT NULL,
    rewards_json        JSON NOT NULL,
    granted_at          TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    INDEX idx_reward_player_time (player_id, granted_at)
);

CREATE TABLE IF NOT EXISTS quest_definition (
    quest_id             VARCHAR(96) PRIMARY KEY,
    version              BIGINT NOT NULL DEFAULT 1,
    title                VARCHAR(255) NOT NULL,
    description_text     TEXT NOT NULL,
    claim_mode           VARCHAR(16) NOT NULL DEFAULT 'manual',
    turn_in_npc_id       VARCHAR(96) NOT NULL DEFAULT '',
    completion_flag      VARCHAR(96) NOT NULL DEFAULT '',
    pre_quest_ids_json   JSON NOT NULL,
    required_flags_json  JSON NOT NULL,
    objectives_json      JSON NOT NULL,
    rewards_json         JSON NOT NULL,
    enabled              TINYINT(1) NOT NULL DEFAULT 1,
    created_at           TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at           TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    INDEX idx_quest_enabled (enabled, updated_at)
);

CREATE TABLE IF NOT EXISTS player_quest (
    player_id            VARCHAR(64) NOT NULL,
    quest_id             VARCHAR(96) NOT NULL,
    quest_version        BIGINT NOT NULL,
    state                VARCHAR(16) NOT NULL DEFAULT 'accepted',
    progress_version     BIGINT NOT NULL DEFAULT 0,
    accept_request_id    VARCHAR(96) NOT NULL DEFAULT '',
    claim_request_id     VARCHAR(96) NULL DEFAULT NULL,
    accepted_at          TIMESTAMP NULL DEFAULT NULL,
    completed_at         TIMESTAMP NULL DEFAULT NULL,
    claimed_at           TIMESTAMP NULL DEFAULT NULL,
    updated_at           TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (player_id, quest_id),
    UNIQUE KEY uq_player_quest_accept_request (player_id, accept_request_id),
    UNIQUE KEY uq_player_quest_claim_request (player_id, claim_request_id),
    INDEX idx_player_quest_state (player_id, state, updated_at)
);

CREATE TABLE IF NOT EXISTS player_quest_objective (
    player_id            VARCHAR(64) NOT NULL,
    quest_id             VARCHAR(96) NOT NULL,
    objective_id         VARCHAR(96) NOT NULL,
    current_value        BIGINT NOT NULL DEFAULT 0,
    target_value         BIGINT NOT NULL DEFAULT 0,
    updated_at           TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (player_id, quest_id, objective_id)
);

CREATE TABLE IF NOT EXISTS player_story_flag (
    player_id            VARCHAR(64) NOT NULL,
    flag_id              VARCHAR(96) NOT NULL,
    created_at           TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (player_id, flag_id)
);

CREATE TABLE IF NOT EXISTS quest_event_receipt (
    player_id            VARCHAR(64) NOT NULL,
    event_id             VARCHAR(96) NOT NULL,
    event_type           VARCHAR(64) NOT NULL,
    target_id            VARCHAR(96) NOT NULL DEFAULT '',
    source_id            VARCHAR(128) NOT NULL DEFAULT '',
    amount               BIGINT NOT NULL,
    created_at           TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (player_id, event_id)
);

CREATE TABLE IF NOT EXISTS agent_conversation (
    conversation_id     VARCHAR(96) PRIMARY KEY,
    player_id            VARCHAR(64) NOT NULL,
    scene_name           VARCHAR(32) NOT NULL DEFAULT 'unknown',
    title                VARCHAR(128) NOT NULL DEFAULT '',
    status_name          VARCHAR(16) NOT NULL DEFAULT 'active',
    created_at           TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at           TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    INDEX idx_agent_conversation_player (player_id, status_name, updated_at)
);

CREATE TABLE IF NOT EXISTS agent_message (
    message_id           VARCHAR(96) PRIMARY KEY,
    conversation_id      VARCHAR(96) NOT NULL,
    player_id            VARCHAR(64) NOT NULL,
    client_request_id    VARCHAR(128) NOT NULL,
    role_name            VARCHAR(16) NOT NULL,
    content_text         TEXT NOT NULL,
    model_name           VARCHAR(96) NOT NULL DEFAULT '',
    prompt_tokens        BIGINT NOT NULL DEFAULT 0,
    completion_tokens    BIGINT NOT NULL DEFAULT 0,
    created_at           TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    UNIQUE KEY uq_agent_message_request_role (player_id, client_request_id, role_name),
    INDEX idx_agent_message_conversation (conversation_id, created_at)
);

CREATE TABLE IF NOT EXISTS agent_tool_call (
    call_id              VARCHAR(128) PRIMARY KEY,
    conversation_id      VARCHAR(96) NOT NULL,
    message_id           VARCHAR(96) NOT NULL,
    player_id            VARCHAR(64) NOT NULL,
    tool_name            VARCHAR(96) NOT NULL,
    arguments_json       JSON NOT NULL,
    risk_level           VARCHAR(16) NOT NULL,
    status_name          VARCHAR(32) NOT NULL,
    display_text         VARCHAR(255) NOT NULL DEFAULT '',
    result_json          JSON NOT NULL,
    error_code           VARCHAR(96) NOT NULL DEFAULT '',
    idempotency_key      VARCHAR(128) NOT NULL,
    created_at           TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    confirmed_at         TIMESTAMP NULL DEFAULT NULL,
    finished_at          TIMESTAMP NULL DEFAULT NULL,
    UNIQUE KEY uq_agent_tool_idempotency (player_id, idempotency_key, tool_name),
    INDEX idx_agent_tool_conversation (conversation_id, created_at),
    INDEX idx_agent_tool_player (player_id, status_name, created_at)
);

CREATE TABLE IF NOT EXISTS agent_usage_daily (
    player_id            VARCHAR(64) NOT NULL,
    usage_date           DATE NOT NULL,
    request_count        BIGINT NOT NULL DEFAULT 0,
    prompt_tokens        BIGINT NOT NULL DEFAULT 0,
    completion_tokens    BIGINT NOT NULL DEFAULT 0,
    failed_count         BIGINT NOT NULL DEFAULT 0,
    PRIMARY KEY (player_id, usage_date)
);

CREATE TABLE IF NOT EXISTS multiplayer_room (
    room_id              VARCHAR(96) PRIMARY KEY,
    owner_player_id      VARCHAR(64) NOT NULL,
    room_name            VARCHAR(128) NOT NULL,
    max_players          INT NOT NULL,
    status               VARCHAR(16) NOT NULL DEFAULT 'open',
    created_at           TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at           TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    INDEX idx_multiplayer_room_owner (owner_player_id, updated_at),
    INDEX idx_multiplayer_room_status (status, updated_at)
);

CREATE TABLE IF NOT EXISTS multiplayer_room_member (
    room_id              VARCHAR(96) NOT NULL,
    player_id            VARCHAR(64) NOT NULL,
    display_name         VARCHAR(128) NOT NULL,
    member_role          VARCHAR(16) NOT NULL DEFAULT 'member',
    joined_at            TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (room_id, player_id),
    INDEX idx_multiplayer_member_player (player_id, joined_at)
);

CREATE TABLE IF NOT EXISTS voice_room_member (
    room_id              VARCHAR(160) NOT NULL,
    scope_type           VARCHAR(16) NOT NULL,
    scope_id             VARCHAR(96) NOT NULL,
    player_id            VARCHAR(64) NOT NULL,
    display_name         VARCHAR(128) NOT NULL,
    member_role          VARCHAR(16) NOT NULL DEFAULT 'member',
    is_muted_by_server   TINYINT(1) NOT NULL DEFAULT 0,
    is_banned            TINYINT(1) NOT NULL DEFAULT 0,
    joined_at            TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at           TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (room_id, player_id),
    INDEX idx_voice_scope (scope_type, scope_id),
    INDEX idx_voice_player (player_id, updated_at)
);

CREATE TABLE IF NOT EXISTS voice_moderation_event (
    event_id             VARCHAR(96) PRIMARY KEY,
    room_id              VARCHAR(160) NOT NULL,
    target_player_id     VARCHAR(64) NOT NULL,
    operator_id          VARCHAR(96) NOT NULL,
    action_name          VARCHAR(16) NOT NULL,
    reason               VARCHAR(255) NOT NULL DEFAULT '',
    created_at           TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    INDEX idx_voice_moderation_room (room_id, created_at),
    INDEX idx_voice_moderation_target (target_player_id, created_at)
);

CREATE TABLE IF NOT EXISTS reward_template (
    template_id         VARCHAR(96) PRIMARY KEY,
    template_name       VARCHAR(128) NOT NULL,
    rewards_json        JSON NOT NULL,
    created_at          TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at          TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP
);

CREATE TABLE IF NOT EXISTS announcement (
    announcement_id     VARCHAR(96) PRIMARY KEY,
    channel_id          VARCHAR(128) NOT NULL,
    channel_kind        VARCHAR(32) NOT NULL,
    scope_id            VARCHAR(128) NOT NULL,
    display_name        VARCHAR(128) NOT NULL,
    operator_id         VARCHAR(96) NOT NULL,
    title               VARCHAR(255) NOT NULL,
    body_text           TEXT NOT NULL,
    published_at        TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    expires_at          TIMESTAMP NULL DEFAULT NULL,
    created_at          TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    INDEX idx_announcement_published (published_at),
    INDEX idx_announcement_channel (channel_kind, scope_id, published_at)
);

CREATE TABLE IF NOT EXISTS mail_message (
    mail_id             VARCHAR(64) PRIMARY KEY,
    player_id           VARCHAR(64) NOT NULL,
    sender_name         VARCHAR(128) NOT NULL,
    title               VARCHAR(255) NOT NULL,
    body_text           TEXT NOT NULL,
    rewards_json        JSON NOT NULL,
    is_read             TINYINT(1) NOT NULL DEFAULT 0,
    is_claimed          TINYINT(1) NOT NULL DEFAULT 0,
    is_deleted          TINYINT(1) NOT NULL DEFAULT 0,
    claim_grant_id      VARCHAR(96) NOT NULL DEFAULT '',
    sent_at             TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    expires_at          TIMESTAMP NULL DEFAULT NULL,
    read_at             TIMESTAMP NULL DEFAULT NULL,
    claimed_at          TIMESTAMP NULL DEFAULT NULL,
    deleted_at          TIMESTAMP NULL DEFAULT NULL,
    created_at          TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at          TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    INDEX idx_mail_player_time (player_id, sent_at),
    INDEX idx_mail_player_deleted (player_id, is_deleted, sent_at)
);

CREATE TABLE IF NOT EXISTS gm_audit_log (
    audit_id            VARCHAR(96) PRIMARY KEY,
    operator_id         VARCHAR(96) NOT NULL,
    action_name         VARCHAR(96) NOT NULL,
    target_type         VARCHAR(64) NOT NULL,
    target_id           VARCHAR(128) NOT NULL,
    request_ip          VARCHAR(64) NOT NULL DEFAULT '',
    user_agent          VARCHAR(255) NOT NULL DEFAULT '',
    trace_id            VARCHAR(96) NOT NULL DEFAULT '',
    success_flag        TINYINT(1) NOT NULL DEFAULT 1,
    failure_reason      VARCHAR(255) NOT NULL DEFAULT '',
    duration_ms         BIGINT NOT NULL DEFAULT 0,
    before_json         JSON NULL,
    after_json          JSON NULL,
    detail_json         JSON NOT NULL,
    created_at          TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    INDEX idx_gm_audit_time (created_at),
    INDEX idx_gm_audit_operator (operator_id, created_at)
);

CREATE TABLE IF NOT EXISTS admin_account (
    account_id           VARCHAR(64) PRIMARY KEY,
    operator_name        VARCHAR(128) NOT NULL,
    is_active            TINYINT(1) NOT NULL DEFAULT 1,
    created_at           TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at           TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    CONSTRAINT fk_admin_account FOREIGN KEY (account_id) REFERENCES account(account_id)
);

CREATE TABLE IF NOT EXISTS admin_permission (
    account_id           VARCHAR(64) NOT NULL,
    permission_code      VARCHAR(96) NOT NULL,
    created_at           TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (account_id, permission_code),
    CONSTRAINT fk_admin_permission_account FOREIGN KEY (account_id) REFERENCES admin_account(account_id)
);

CREATE TABLE IF NOT EXISTS admin_task (
    task_id              VARCHAR(96) PRIMARY KEY,
    task_kind            VARCHAR(32) NOT NULL,
    status               VARCHAR(32) NOT NULL,
    operator_id          VARCHAR(96) NOT NULL,
    target_type          VARCHAR(64) NOT NULL,
    target_id            VARCHAR(128) NOT NULL,
    request_ip           VARCHAR(64) NOT NULL DEFAULT '',
    user_agent           VARCHAR(255) NOT NULL DEFAULT '',
    trace_id             VARCHAR(96) NOT NULL DEFAULT '',
    request_json         JSON NOT NULL,
    result_json          JSON NULL,
    failure_reason       VARCHAR(255) NOT NULL DEFAULT '',
    execute_at           TIMESTAMP NULL DEFAULT NULL,
    executed_at          TIMESTAMP NULL DEFAULT NULL,
    created_at           TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at           TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    INDEX idx_admin_task_due (task_kind, status, execute_at),
    INDEX idx_admin_task_created (created_at),
    INDEX idx_admin_task_trace (trace_id)
);

SET @gm_audit_add_cursor_index = (
    SELECT IF(
        EXISTS(
            SELECT 1 FROM information_schema.statistics
            WHERE table_schema = DATABASE() AND table_name = 'gm_audit_log' AND index_name = 'idx_gm_audit_time_id'
        ),
        'SELECT 1',
        'ALTER TABLE gm_audit_log ADD INDEX idx_gm_audit_time_id (created_at, audit_id)'
    )
);
PREPARE stmt FROM @gm_audit_add_cursor_index;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

SET @admin_task_add_cursor_index = (
    SELECT IF(
        EXISTS(
            SELECT 1 FROM information_schema.statistics
            WHERE table_schema = DATABASE() AND table_name = 'admin_task' AND index_name = 'idx_admin_task_created_id'
        ),
        'SELECT 1',
        'ALTER TABLE admin_task ADD INDEX idx_admin_task_created_id (created_at, task_id)'
    )
);
PREPARE stmt FROM @admin_task_add_cursor_index;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

SET @gm_audit_add_request_ip = (
    SELECT IF(
        EXISTS(
            SELECT 1 FROM information_schema.columns
            WHERE table_schema = DATABASE() AND table_name = 'gm_audit_log' AND column_name = 'request_ip'
        ),
        'SELECT 1',
        'ALTER TABLE gm_audit_log ADD COLUMN request_ip VARCHAR(64) NOT NULL DEFAULT '''' AFTER target_id'
    )
);
PREPARE stmt FROM @gm_audit_add_request_ip;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

SET @gm_audit_add_user_agent = (
    SELECT IF(
        EXISTS(
            SELECT 1 FROM information_schema.columns
            WHERE table_schema = DATABASE() AND table_name = 'gm_audit_log' AND column_name = 'user_agent'
        ),
        'SELECT 1',
        'ALTER TABLE gm_audit_log ADD COLUMN user_agent VARCHAR(255) NOT NULL DEFAULT '''' AFTER request_ip'
    )
);
PREPARE stmt FROM @gm_audit_add_user_agent;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

SET @gm_audit_add_trace_id = (
    SELECT IF(
        EXISTS(
            SELECT 1 FROM information_schema.columns
            WHERE table_schema = DATABASE() AND table_name = 'gm_audit_log' AND column_name = 'trace_id'
        ),
        'SELECT 1',
        'ALTER TABLE gm_audit_log ADD COLUMN trace_id VARCHAR(96) NOT NULL DEFAULT '''' AFTER user_agent'
    )
);
PREPARE stmt FROM @gm_audit_add_trace_id;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

SET @gm_audit_add_success_flag = (
    SELECT IF(
        EXISTS(
            SELECT 1 FROM information_schema.columns
            WHERE table_schema = DATABASE() AND table_name = 'gm_audit_log' AND column_name = 'success_flag'
        ),
        'SELECT 1',
        'ALTER TABLE gm_audit_log ADD COLUMN success_flag TINYINT(1) NOT NULL DEFAULT 1 AFTER trace_id'
    )
);
PREPARE stmt FROM @gm_audit_add_success_flag;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

SET @gm_audit_add_failure_reason = (
    SELECT IF(
        EXISTS(
            SELECT 1 FROM information_schema.columns
            WHERE table_schema = DATABASE() AND table_name = 'gm_audit_log' AND column_name = 'failure_reason'
        ),
        'SELECT 1',
        'ALTER TABLE gm_audit_log ADD COLUMN failure_reason VARCHAR(255) NOT NULL DEFAULT '''' AFTER success_flag'
    )
);
PREPARE stmt FROM @gm_audit_add_failure_reason;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

SET @gm_audit_add_duration_ms = (
    SELECT IF(
        EXISTS(
            SELECT 1 FROM information_schema.columns
            WHERE table_schema = DATABASE() AND table_name = 'gm_audit_log' AND column_name = 'duration_ms'
        ),
        'SELECT 1',
        'ALTER TABLE gm_audit_log ADD COLUMN duration_ms BIGINT NOT NULL DEFAULT 0 AFTER failure_reason'
    )
);
PREPARE stmt FROM @gm_audit_add_duration_ms;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

SET @gm_audit_add_before_json = (
    SELECT IF(
        EXISTS(
            SELECT 1 FROM information_schema.columns
            WHERE table_schema = DATABASE() AND table_name = 'gm_audit_log' AND column_name = 'before_json'
        ),
        'SELECT 1',
        'ALTER TABLE gm_audit_log ADD COLUMN before_json JSON NULL AFTER duration_ms'
    )
);
PREPARE stmt FROM @gm_audit_add_before_json;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

SET @gm_audit_add_after_json = (
    SELECT IF(
        EXISTS(
            SELECT 1 FROM information_schema.columns
            WHERE table_schema = DATABASE() AND table_name = 'gm_audit_log' AND column_name = 'after_json'
        ),
        'SELECT 1',
        'ALTER TABLE gm_audit_log ADD COLUMN after_json JSON NULL AFTER before_json'
    )
);
PREPARE stmt FROM @gm_audit_add_after_json;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

SET @chat_message_add_recalled = (
    SELECT IF(
        EXISTS(
            SELECT 1 FROM information_schema.columns
            WHERE table_schema = DATABASE() AND table_name = 'chat_message' AND column_name = 'recalled'
        ),
        'SELECT 1',
        'ALTER TABLE chat_message ADD COLUMN recalled TINYINT(1) NOT NULL DEFAULT 0'
    )
);
PREPARE stmt FROM @chat_message_add_recalled;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

SET @chat_message_add_recalled_by = (
    SELECT IF(
        EXISTS(
            SELECT 1 FROM information_schema.columns
            WHERE table_schema = DATABASE() AND table_name = 'chat_message' AND column_name = 'recalled_by_player_id'
        ),
        'SELECT 1',
        'ALTER TABLE chat_message ADD COLUMN recalled_by_player_id VARCHAR(64) NULL'
    )
);
PREPARE stmt FROM @chat_message_add_recalled_by;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

SET @chat_message_add_recalled_at = (
    SELECT IF(
        EXISTS(
            SELECT 1 FROM information_schema.columns
            WHERE table_schema = DATABASE() AND table_name = 'chat_message' AND column_name = 'recalled_at'
        ),
        'SELECT 1',
        'ALTER TABLE chat_message ADD COLUMN recalled_at TIMESTAMP NULL DEFAULT NULL'
    )
);
PREPARE stmt FROM @chat_message_add_recalled_at;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

SET @friend_link_add_last_message_at = (
    SELECT IF(
        EXISTS(
            SELECT 1 FROM information_schema.columns
            WHERE table_schema = DATABASE() AND table_name = 'friend_link' AND column_name = 'last_message_at'
        ),
        'SELECT 1',
        'ALTER TABLE friend_link ADD COLUMN last_message_at TIMESTAMP NULL DEFAULT NULL AFTER status'
    )
);
PREPARE stmt FROM @friend_link_add_last_message_at;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

SET @friend_link_add_last_message_epoch = (
    SELECT IF(
        EXISTS(
            SELECT 1 FROM information_schema.columns
            WHERE table_schema = DATABASE() AND table_name = 'friend_link' AND column_name = 'last_message_epoch'
        ),
        'SELECT 1',
        'ALTER TABLE friend_link ADD COLUMN last_message_epoch BIGINT NOT NULL DEFAULT 0 AFTER last_message_at'
    )
);
PREPARE stmt FROM @friend_link_add_last_message_epoch;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

UPDATE friend_link
SET last_message_epoch = UNIX_TIMESTAMP(last_message_at)
WHERE last_message_at IS NOT NULL AND last_message_epoch = 0;

SET @friend_link_add_last_message_index = (
    SELECT IF(
        EXISTS(
            SELECT 1 FROM information_schema.statistics
            WHERE table_schema = DATABASE() AND table_name = 'friend_link' AND index_name = 'idx_friend_last_message_epoch'
        ),
        'SELECT 1',
        'ALTER TABLE friend_link ADD INDEX idx_friend_last_message_epoch (player_id, status, last_message_epoch DESC, friend_player_id)'
    )
);
PREPARE stmt FROM @friend_link_add_last_message_index;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

SET @chat_message_add_latest_index = (
    SELECT IF(
        EXISTS(
            SELECT 1 FROM information_schema.statistics
            WHERE table_schema = DATABASE() AND table_name = 'chat_message' AND index_name = 'idx_chat_channel_latest'
        ),
        'SELECT 1',
        'ALTER TABLE chat_message ADD INDEX idx_chat_channel_latest (channel_id, sent_at DESC, message_id DESC)'
    )
);
PREPARE stmt FROM @chat_message_add_latest_index;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

CREATE TABLE IF NOT EXISTS gacha_pool (
    pool_id VARCHAR(96) PRIMARY KEY,
    pool_type VARCHAR(32) NOT NULL DEFAULT 'standard',
    name VARCHAR(128) NOT NULL,
    description TEXT NOT NULL,
    banner_icon VARCHAR(255) NOT NULL DEFAULT '',
    cost_currency_id VARCHAR(64) NOT NULL DEFAULT 'gem',
    cost_amount_per_draw BIGINT NOT NULL DEFAULT 160,
    pity_5 BIGINT NOT NULL DEFAULT 90,
    pity_4 BIGINT NOT NULL DEFAULT 10,
    enabled TINYINT(1) NOT NULL DEFAULT 1,
    starts_at TIMESTAMP NULL DEFAULT NULL,
    ends_at TIMESTAMP NULL DEFAULT NULL,
    created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    INDEX idx_gacha_pool_enabled_time(enabled, starts_at, ends_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

ALTER TABLE item_definition ALTER COLUMN rarity SET DEFAULT '3星';

UPDATE item_definition
SET rarity = CASE
    WHEN rarity IN ('common', 'uncommon', 'rare') THEN '3星'
    WHEN rarity = 'epic' THEN '4星'
    WHEN rarity = 'legendary' THEN '5星'
    ELSE rarity
END
WHERE rarity IN ('common', 'uncommon', 'rare', 'epic', 'legendary');

SET @gacha_pool_add_pool_type = (
    SELECT IF(
        EXISTS(
            SELECT 1 FROM information_schema.columns
            WHERE table_schema = DATABASE() AND table_name = 'gacha_pool' AND column_name = 'pool_type'
        ),
        'SELECT 1',
        'ALTER TABLE gacha_pool ADD COLUMN pool_type VARCHAR(32) NOT NULL DEFAULT ''standard'' AFTER pool_id'
    )
);
PREPARE stmt FROM @gacha_pool_add_pool_type;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

CREATE TABLE IF NOT EXISTS gacha_pool_item (
    pool_id VARCHAR(96) NOT NULL,
    item_key VARCHAR(128) NOT NULL,
    rarity BIGINT NOT NULL DEFAULT 3,
    weight BIGINT NOT NULL DEFAULT 1,
    is_up TINYINT(1) NOT NULL DEFAULT 0,
    reward_kind VARCHAR(32) NOT NULL,
    reward_resource_id VARCHAR(128) NOT NULL DEFAULT '',
    reward_amount BIGINT NOT NULL DEFAULT 1,
    created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY(pool_id, item_key),
    INDEX idx_gacha_pool_item_pool_rarity(pool_id, rarity),
    CONSTRAINT fk_gacha_pool_item_pool FOREIGN KEY(pool_id) REFERENCES gacha_pool(pool_id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS gacha_player_state (
    player_id VARCHAR(64) NOT NULL,
    pool_id VARCHAR(96) NOT NULL,
    pity_5_count BIGINT NOT NULL DEFAULT 0,
    pity_4_count BIGINT NOT NULL DEFAULT 0,
    total_draws BIGINT NOT NULL DEFAULT 0,
    updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY(player_id, pool_id),
    INDEX idx_gacha_state_pool(pool_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS gacha_history (
    history_id VARCHAR(160) PRIMARY KEY,
    player_id VARCHAR(64) NOT NULL,
    pool_id VARCHAR(96) NOT NULL,
    item_key VARCHAR(128) NOT NULL,
    rarity BIGINT NOT NULL DEFAULT 3,
    is_up TINYINT(1) NOT NULL DEFAULT 0,
    reward_kind VARCHAR(32) NOT NULL,
    reward_resource_id VARCHAR(128) NOT NULL DEFAULT '',
    reward_amount BIGINT NOT NULL DEFAULT 1,
    pity_5_before BIGINT NOT NULL DEFAULT 0,
    pity_4_before BIGINT NOT NULL DEFAULT 0,
    drawn_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    INDEX idx_gacha_history_player_time(player_id, drawn_at),
    INDEX idx_gacha_history_pool_time(pool_id, drawn_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS shop_product (
    product_id VARCHAR(96) PRIMARY KEY,
    category VARCHAR(64) NOT NULL DEFAULT 'default',
    name VARCHAR(128) NOT NULL,
    description TEXT NOT NULL,
    icon VARCHAR(255) NOT NULL DEFAULT '',
    price_currency_id VARCHAR(64) NOT NULL DEFAULT 'gem',
    price_amount BIGINT NOT NULL DEFAULT 0,
    rewards_json JSON NOT NULL,
    purchase_limit BIGINT NOT NULL DEFAULT 0,
    enabled TINYINT(1) NOT NULL DEFAULT 1,
    starts_at TIMESTAMP NULL DEFAULT NULL,
    ends_at TIMESTAMP NULL DEFAULT NULL,
    created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    INDEX idx_shop_product_enabled_time(enabled, starts_at, ends_at),
    INDEX idx_shop_product_category(category)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS shop_purchase (
    purchase_id VARCHAR(180) PRIMARY KEY,
    player_id VARCHAR(64) NOT NULL,
    product_id VARCHAR(96) NOT NULL,
    price_currency_id VARCHAR(64) NOT NULL,
    price_amount BIGINT NOT NULL DEFAULT 0,
    rewards_json JSON NOT NULL,
    purchased_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    INDEX idx_shop_purchase_player_time(player_id, purchased_at),
    INDEX idx_shop_purchase_player_product(player_id, product_id),
    INDEX idx_shop_purchase_product_time(product_id, purchased_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS shop_purchase_limit (
    player_id VARCHAR(64) NOT NULL,
    product_id VARCHAR(96) NOT NULL,
    purchase_count BIGINT NOT NULL DEFAULT 0,
    PRIMARY KEY (player_id, product_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- 从历史购买流水回填限购计数，保证“先无限购、后配置限购”的商品也能正确计数；幂等。
INSERT INTO shop_purchase_limit(player_id, product_id, purchase_count)
SELECT player_id, product_id, COUNT(*)
FROM shop_purchase
GROUP BY player_id, product_id
ON DUPLICATE KEY UPDATE purchase_count = GREATEST(shop_purchase_limit.purchase_count, VALUES(purchase_count));

CREATE TABLE IF NOT EXISTS loot_drop (
    drop_id VARCHAR(180) PRIMARY KEY,
    owner_player_id VARCHAR(64) NOT NULL DEFAULT '',
    scene_id VARCHAR(96) NOT NULL DEFAULT 'default',
    rewards_json JSON NOT NULL,
    picked TINYINT(1) NOT NULL DEFAULT 0,
    picked_by_player_id VARCHAR(64) NOT NULL DEFAULT '',
    spawned_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    expires_at TIMESTAMP NULL DEFAULT NULL,
    picked_at TIMESTAMP NULL DEFAULT NULL,
    INDEX idx_loot_owner_picked_time(owner_player_id, picked, spawned_at),
    INDEX idx_loot_scene_time(scene_id, spawned_at),
    INDEX idx_loot_expires(expires_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
