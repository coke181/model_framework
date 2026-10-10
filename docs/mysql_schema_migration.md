# MySQL Schema 自动迁移说明

## 文档目的

这份文档说明后端如何在启动时自动创建或补齐 MySQL 表结构。

目标是：

- 新服务器首次部署时，不需要手动执行完整建表 SQL。
- 后续新增商城、拾取、语音房间、活动、配置等表时，只改一份 schema 文件。
- 当前任务系统的 `quest_definition`、`player_quest`、`player_quest_objective`、`player_story_flag`、`quest_event_receipt` 也由该文件自动创建。
- 语音第一阶段的 `voice_room_member` 和 `voice_moderation_event` 同样会在服务启动时自动补齐。
- 多个后端进程同时启动时，不会同时抢着改表。
- 生产环境仍然可控，可以关闭自动迁移后手动执行 SQL。

## 当前实现结论

后端在 `mysql_redis` 存储模式下启动时，会自动执行：

```text
db/mysql/001_game_backend.sql
```

执行入口：

```text
main.cpp / chat_gateway_main.cpp / task_worker_main.cpp
  -> load_storage_options_from_env("HELLO_SERVICE")
  -> make_repositories(...)
  -> run_mysql_schema_migration_if_enabled(...)
  -> 创建 MySQL 仓储
```

也就是说，HTTP 服务、聊天网关、后台 worker 只要进入 MySQL + Redis 模式，都会先确认表结构存在，再开始处理业务。

## 相关配置

Linux 默认配置在：

```text
scripts/linux/deploy.env
```

关键环境变量：

```env
HELLO_SERVICE_STORAGE_MODE=mysql_redis
HELLO_SERVICE_MYSQL_AUTO_MIGRATE=1
HELLO_SERVICE_MYSQL_SCHEMA_FILE=db/mysql/001_game_backend.sql
```

含义：

- `HELLO_SERVICE_STORAGE_MODE=mysql_redis`：启用真实 MySQL + Redis 仓储。
- `HELLO_SERVICE_MYSQL_AUTO_MIGRATE=1`：启动时自动执行 schema migration。
- `HELLO_SERVICE_MYSQL_SCHEMA_FILE=...`：指定要执行的 schema SQL 文件。

如果要关闭自动迁移：

```env
HELLO_SERVICE_MYSQL_AUTO_MIGRATE=0
```

关闭后，服务不会自动建表或补字段，你需要自己执行 SQL。

## 启动时做了什么

自动迁移流程：

1. 连接 MySQL 服务端。
2. 执行 `CREATE DATABASE IF NOT EXISTS game_backend ...`。
3. 连接目标数据库 `game_backend`。
4. 获取 MySQL 命名锁 `GET_LOCK('mf_game_backend_schema_migration', 30)`。
5. 读取 `db/mysql/001_game_backend.sql`。
6. 按 SQL 语句逐条执行。
7. 释放 MySQL 命名锁。
8. 仓储层开始正常创建和使用。

为什么要加命名锁：

```text
hello_service
hello_chat_gateway
hello_task_worker
```

这三个进程可能被 systemd 同时拉起。如果它们同时 `ALTER TABLE`，MySQL 可能出现锁等待或冲突。命名锁可以保证同一时间只有一个进程执行 migration，其他进程会等待。

## SQL 文件必须怎么写

`db/mysql/001_game_backend.sql` 必须保持幂等。

幂等的意思是：

```text
执行 1 次可以成功
执行 10 次也必须成功
数据库已经有表、有字段时也不能报错
```

新表写法：

```sql
CREATE TABLE IF NOT EXISTS voice_room (
    room_id VARCHAR(96) PRIMARY KEY,
    owner_player_id VARCHAR(64) NOT NULL,
    scene_id VARCHAR(96) NOT NULL DEFAULT 'default',
    max_members BIGINT NOT NULL DEFAULT 8,
    status VARCHAR(32) NOT NULL DEFAULT 'open',
    created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    INDEX idx_voice_room_scene_status(scene_id, status)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
```

老表补字段不要直接写：

```sql
ALTER TABLE player_profile ADD COLUMN level BIGINT NOT NULL DEFAULT 1;
```

因为第二次执行会报“字段已存在”。

应该写成条件 ALTER：

```sql
SET @player_profile_add_level = (
    SELECT IF(
        EXISTS(
            SELECT 1
            FROM information_schema.columns
            WHERE table_schema = DATABASE()
              AND table_name = 'player_profile'
              AND column_name = 'level'
        ),
        'SELECT 1',
        'ALTER TABLE player_profile ADD COLUMN level BIGINT NOT NULL DEFAULT 1'
    )
);
PREPARE stmt FROM @player_profile_add_level;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;
```

## 后续新增功能时怎么做

例如你要新增“语音房间”：

1. 在 `db/mysql/001_game_backend.sql` 末尾追加 `CREATE TABLE IF NOT EXISTS voice_room ...`。
2. 如果还需要成员表，追加 `CREATE TABLE IF NOT EXISTS voice_room_member ...`。
3. C++ 里新增对应模型、Repository、Service、Route。
4. 本地编译。
5. 把代码和 `001_game_backend.sql` 同步到服务器。
6. 重启服务。
7. 看日志确认 migration 成功。

服务器验证命令：

```bash
journalctl -u hello-service -n 100 --no-pager | grep "mysql schema migration"
```

查看表是否创建：

```bash
mysql -ugame_backend -p game_backend -e "SHOW TABLES;"
```

查看具体表结构：

```bash
mysql -ugame_backend -p game_backend -e "DESC voice_room;"
```

## 生产环境注意事项

开发服可以默认开启：

```env
HELLO_SERVICE_MYSQL_AUTO_MIGRATE=1
```

正式服早期也可以开启，方便快速迭代。

但如果后面玩家数据量变大，需要更谨慎：

- 大表 `ALTER TABLE` 可能锁表。
- 加索引可能耗时较长。
- 字段类型变更可能影响线上请求。
- 删除字段、重命名字段这类破坏性变更不应该自动执行。

更稳的生产做法：

1. 保留自动创建新表。
2. 大表变更先在测试服跑。
3. 生产服低峰期执行。
4. 破坏性 SQL 单独写 migration 文件，人工确认后执行。

如果要完全手动控制生产库：

```env
HELLO_SERVICE_MYSQL_AUTO_MIGRATE=0
```

然后手动执行：

```bash
cd /opt/ue-game-backend/model
mysql -ugame_backend -p game_backend < db/mysql/001_game_backend.sql
```

## 常见问题

### 1. 启动时报 cannot open mysql schema file

原因通常是工作目录不对，导致相对路径找不到：

```text
db/mysql/001_game_backend.sql
```

systemd 里要保证：

```ini
WorkingDirectory=/opt/ue-game-backend/model
```

### 2. 启动时报 CREATE DATABASE 权限不足

服务用户没有创建数据库权限。

处理方式：

```bash
mysql -uroot -p -e "CREATE DATABASE IF NOT EXISTS game_backend CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;"
```

然后只给业务库权限：

```sql
GRANT ALL PRIVILEGES ON game_backend.* TO 'game_backend'@'127.0.0.1';
FLUSH PRIVILEGES;
```

### 3. 三个服务都打印 migration 日志正常吗

正常。

HTTP 服务、聊天网关、后台 worker 都会调用 `make_repositories()`。它们都会进入 migration 检查，但因为 SQL 是幂等的，并且有 MySQL 命名锁，所以不会重复破坏数据。

### 4. 可以把 seed 数据也放进 001 吗

不建议把大量测试数据放进 `001_game_backend.sql`。

推荐规则：

- 表结构放 `001_game_backend.sql`。
- 初始正式配置可以写成 `INSERT ... ON DUPLICATE KEY UPDATE`。
- 临时测试数据、清理脚本、修复脚本单独放新 SQL 文件。

当前已有类似文件：

```text
db/mysql/002_seed_gacha_standard_pool.sql
db/mysql/003_seed_gacha_all_items_pool.sql
db/mysql/007_seed_shop_default_products.sql
```

这些 seed 文件不会被自动执行，除非后续明确把 seed migration 也接入启动流程。
