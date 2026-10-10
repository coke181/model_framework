# MySQL + Redis 架构导读

## 目标

这套框架现在把“事实存储”和“热点状态”分开建模，目标是让你以后把：

- `AccountRepository` 主要落到 `MySQL`
  现在不仅存账号基础资料，也负责正式密码账号的哈希串持久化
- `PlayerRepository` 主要落到 `MySQL`
- `FriendRepository` 主要落到 `MySQL`
- `SessionRepository` 主要落到 `Redis`
- `AuthSecurityRepository` 主要落到 `Redis`
- `ChatRepository` 走 `MySQL + Redis 最近窗口`

## 为什么这样拆

- `MySQL`
  负责账号、密码哈希、玩家资料、好友关系、聊天事实持久化。这些数据需要稳定、可追溯、可补偿。
- `Redis`
  负责 Session、刷新 token 索引、会话列表索引、验证码、登录限流、黑名单、在线状态、最近消息窗口、频道成员缓存、好友私聊未读/离线消息。这些数据更强调低延迟和过期控制。

## 这次代码改动里最值得 review 的文件

- [include/mf/game/models.hpp](/d:/code/model/include/mf/game/models.hpp)
  现在频道和消息类型已经显式建模，不再把所有聊天都塞进一个字符串。
- [include/mf/chat/channels.hpp](/d:/code/model/include/mf/chat/channels.hpp)
  统一定义大厅、房间、好友私聊频道的构造规则。
- [include/mf/game/repositories.hpp](/d:/code/model/include/mf/game/repositories.hpp)
  已经把仓储职责按 MySQL / Redis 场景写清楚。
- [src/mysql_repositories.cpp](/d:/code/model/src/mysql_repositories.cpp)
  这里已经包含账号、玩家、好友、聊天四类真实 MySQL 实现，重点看预处理语句、正式密码账号与 quick 账号的分流、稳定 ID、库存 JSON 解析、好友双向写入、聊天历史查询。
- [include/mf/auth/password_hasher.hpp](/d:/code/model/include/mf/auth/password_hasher.hpp)
  这里定义了密码哈希服务接口，重点看为什么要把 Argon2id 参数集中封装。
- [src/password_hasher.cpp](/d:/code/model/src/password_hasher.cpp)
  这里实现了基于 OpenSSL 3 Argon2id KDF 的密码哈希和校验，重点看哈希串编码、参数解析、常量时间比较。
- [include/mf/auth/account_service.hpp](/d:/code/model/include/mf/auth/account_service.hpp)
  这里定义了账号正式流程的服务接口，重点看为什么把注册、登录、改密、找回、风控从 handler 里抽出来。
- [src/account_service.cpp](/d:/code/model/src/account_service.cpp)
  这里串起了账号仓储、玩家仓储、Redis 风控、密码哈希和会话签发，重点看失败路径和错误码映射。
- [src/redis_repositories.cpp](/d:/code/model/src/redis_repositories.cpp)
  这里已经包含 SessionRepository、AuthSecurityRepository、聊天最近窗口缓存、在线状态、频道成员、好友私聊未读/离线消息，重点看序列化、TTL、Redis 列表缓存、回源回填、Hash/List/Set 状态维护。
- [include/mf/chat/state.hpp](/d:/code/model/include/mf/chat/state.hpp)
  这里是聊天状态层的抽象接口，重点看为什么它和 ChatRepository 分开。
- [src/chat_state.cpp](/d:/code/model/src/chat_state.cpp)
  这里是内存版状态实现，最适合先理解在线、成员、未读、离线消息的基本语义。
- [db/mysql/001_game_backend.sql](/d:/code/model/db/mysql/001_game_backend.sql)
  给出了当前后端骨架对应的第一版表结构。
- [scripts/run-hello-mysql-redis.ps1](/d:/code/model/scripts/run-hello-mysql-redis.ps1)
  这里是本地联调入口，review 时能顺着看到环境变量和启动依赖关系。

## 当前已经落地的部分

- 真实 `AccountRepository -> MySQL`
  已区分正式密码账号和开发 quick 账号
- 真实 `PlayerRepository -> MySQL`
- 真实 `FriendRepository -> MySQL`
- 真实 `RewardRepository -> MySQL`
- 真实 `MailRepository -> MySQL`
- 真实 `ChatRepository -> MySQL + Redis 最近窗口缓存`
- 真实 `SessionRepository -> Redis`
- 真实 `AuthSecurityRepository -> Redis`
- 真实聊天状态层：在线状态、频道成员、好友私聊未读/离线消息 -> Redis
- 真实聊天治理：好友申请、聊天黑名单、频道禁言、消息撤回、GM 公告
- 真实邮件与奖励链：系统邮件、附件领取、奖励流水、玩家资产发放
- `in_memory` 和 `mysql_redis` 两套模式共存，可切换 review

## 从“通用游戏后端框架”视角还缺什么

当前这套代码更像“接入层 + 社交层 + 基础安全层”已经成型，但离“大部分游戏都能直接复用”的通用框架还差下面几块：

1. 角色层
   还没有完整的角色创建、角色列表、选角、角色扩展字段模型。
2. 背包与货币层
   还没有物品实例化、叠加规则、消耗校验、发奖流水、货币流水和幂等保障。
3. 商城与订单层
   还没有商品配置、订单状态机、支付回调校验、补单与发货审计。
4. 邮件与奖励层
   还没有系统邮件、附件领取、运营补偿、批量发奖。
5. 配置与活动层
   还没有配置版本管理、热加载、活动开关、定时生效、灰度发布。
6. GM / 后台治理层
   还没有完整的管理员身份体系、权限模型、操作审计、查号、发奖后台。
7. 聊天治理增强
   还缺敏感词、举报、GM 强制撤回、全局禁言、发言频率限制、聊天审计检索。
8. 运维与可观测性
   还缺 metrics、tracing、告警、慢 SQL 观测、异步任务和补偿。
9. 协议治理
   还缺更系统的错误码、分页、版本兼容和前后端协作文档。

## 现在还没做的事

如果只从当前已经实现的账号 / 社交 / 聊天范围看，主要还缺：

- `lobby` / `room` 的已读游标与精确未读
- GM / 管理接口的真实权限校验
- 更严格的好友申请去重、状态流转和 affected rows 校验
- 更细的后台管理鉴权与审计日志
- MySQL / Redis 联机状态监控和更完整的自动化测试

## 建议的下一阶段落地方向

如果目标是逐步变成“适用于大部分游戏”的通用后端，我建议按下面顺序继续：

1. 角色、背包、货币流水
2. 邮件、奖励投递、发奖幂等
3. 商城、订单、支付回调
4. 配置、活动、热更新加载
5. GM 后台、审计、权限体系
6. metrics、tracing、自动化测试

当前最适合你的 review 顺序是：

1. 先看 `examples/hello/main.cpp` 如何通过 `make_repositories()` 选择存储模式
2. 再看 `src/storage_factory.cpp` 如何把接口绑定到 MySQL / Redis 实现
3. 再看 `src/mysql_repositories.cpp` 和 `src/redis_repositories.cpp` 的真实存储细节
4. 最后用 `scripts/smoke-http.ps1` 跑一遍，把代码理解和运行结果对上
