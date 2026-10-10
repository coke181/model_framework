# 工程化加固说明

## 当前边界

MySQL/Redis 模式下，聊天消息现在是“先写 MySQL Outbox，再广播；独立 worker 批量写入聊天事实表”。成功写入 Outbox 后才广播，因此网关进程崩溃时，尚未处理的消息仍会留在数据库中，重启后可以恢复。

Outbox 不是无限容量的消息系统。MySQL 不可用时，新的聊天消息会被拒绝而不会广播；Outbox 已经写入但 worker 尚未处理的消息会等待恢复。进程被 `kill -9` 或机器断电时，已经提交到 Outbox 的消息不会丢失。

## 本次加固

### 1. 消息流程

```text
WebSocket 收到消息
  -> 校验好友关系、黑名单和禁言
  -> MySQL/Redis 模式：INSERT chat_message_outbox
  -> Outbox INSERT 成功后广播给在线订阅者
  -> persistence worker 开启事务
  -> SELECT 待处理 Outbox 行 FOR UPDATE SKIP LOCKED
  -> INSERT chat_message
  -> 更新好友最后消息时间
  -> DELETE chat_message_outbox
  -> COMMIT
```

`RedisCachedChatRepository` 只在 Outbox 消费成功后刷新最近消息缓存。Redis 写入失败不会影响已经提交到 MySQL 的聊天事实。

HTTP 服务、聊天网关和任务 worker 可能同时启动 Outbox worker；MySQL 8 的 `SKIP LOCKED` 让多个进程并行消费不同消息，不会反复等待同一批行。

### 2. MySQL 写入失败重试

`ChatService::persist_batch()` 捕获所有异常，内存 review 模式的 worker 不会因为数据库异常退出。MySQL Outbox worker 由数据库中的待处理行承担恢复，事务失败后行仍然保留，下一轮继续处理。

配置项：

```text
HELLO_CHAT_PERSISTENCE_MAX_RETRIES=5
```

超过上限后记录错误指标和日志，并结束当前内存批次处理。MySQL Outbox 不依赖这个内存重试上限，未完成行会继续保留在数据库中。

### 3. 幂等写入

`chat_message.message_id` 是主键。MySQL 插入使用 `ON DUPLICATE KEY UPDATE message_id = VALUES(message_id)`，网络超时后重试同一批不会生成重复聊天记录。

好友最后消息时间使用 `GREATEST`，旧批次重试不会把较新的最后消息时间覆盖回去。

Outbox 消费顺序是：锁定待处理行 -> 插入 `chat_message` -> 更新好友最后消息时间 -> 删除 Outbox 行 -> 提交事务。事务中断时全部回滚；事务提交后进程崩溃时，重复消费仍由 `message_id` 幂等。

### 4. 队列指标含义

`mf_chat_persistence_queue_pending` 同时包含等待中的消息和正在写 MySQL 的消息。不要只根据它是否为零判断业务完全稳定，还要查看：

```text
mf_chat_persistence_retry_total
mf_chat_persistence_failed_after_retries_total
mf_chat_persistence_batches_total{status="ok"}
mf_chat_persistence_batches_total{status="failed"}
mf_chat_persistence_outbox_enqueue_failed_total
mf_chat_persistence_outbox_retry_total
mf_chat_persistence_outbox_drained_total
```

管理员还可以调用：

```http
GET /admin/query/chat-outbox
Authorization: Bearer <admin-access-token>
```

返回示例：

```json
{
  "pending_count": 0,
  "failed_count": 0,
  "oldest_pending_age_seconds": 0,
  "healthy": true
}
```

该接口需要 `admin.query` 权限，只返回队列健康信息，不返回聊天正文。`failed_count > 0` 表示有消息达到失败阈值，需要运营或开发人员根据 `message_id`、`last_error` 查询数据库后补偿。

人工重试接口：

```http
POST /admin/query/chat-outbox/retry
Authorization: Bearer <admin-access-token>
Content-Type: application/json

{"limit": 10}
```

该接口需要 `admin.chat_outbox_retry` 权限，最多一次重试 100 条失败记录，并写入管理员审计日志。重试只把记录恢复为 `pending`，不会直接绕过正常的事务消费流程。

### 5. 发布和停机语义

发布流程是：校验 -> 生成 `message_id` -> 写入 Outbox 或进入内存队列 -> 更新实时状态 -> 广播。服务析构时会停止接收新的 worker 任务并等待当前队列刷完；MySQL Outbox 中已经提交的消息会在下次启动时继续恢复。

因此正式发布前应配合：

- systemd `Restart=on-failure` 和启动限速；
- MySQL/Redis 健康检查；
- 失败指标告警；
- outbox/死信补偿方案；
- 发布前的优雅停机测试。

## 工程化待办

1. 增加 Outbox 的失败次数后台查询、告警和死信归档，避免只依赖日志排查。
2. 增加服务级 readiness/liveness，区分“进程活着”和“MySQL/Redis 可用”。
3. 为 HTTP、聊天网关、任务 worker 分别设置结构化日志、trace id、版本号和构建信息。
4. 把 schema migration、配置校验、密钥检查和回滚步骤纳入部署脚本。
5. 增加 MySQL 中断恢复、Redis 中断恢复、重复消息 ID、优雅停机和队列满的自动化测试。
