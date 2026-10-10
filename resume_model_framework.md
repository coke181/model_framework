# 个人简历（基于 model_framework 项目）

> 使用说明：以下内容已按求职简历整理，方括号 `[]` 内为需要你替换的占位信息。  
> 项目定位是“可复用的游戏业务后端骨架”，简历措辞保持真实，不夸大成已上线的商业级集群系统。

## 一、基本信息

- 姓名：[你的姓名]
- 求职意向：C++ 游戏后端开发工程师 / 服务端开发工程师
- 电话：[你的电话]
- 邮箱：[你的邮箱]
- 所在城市：[你的城市]
- GitHub / 项目地址：[仓库地址]

## 二、教育背景

- [学校名称]，[专业]，[学历]，[起止时间]
- 主要课程或经历：[可选补充]

## 三、专业技能

- 熟悉 C++20 与 Boost 生态，使用 Boost.Asio / Boost.Beast / Boost.JSON 开发 HTTP 与 WebSocket 服务。
- 熟悉 MySQL 表设计、事务、索引、SQL 优化与 schema migration，了解 Redis 常用数据结构与缓存/会话设计。
- 了解游戏后端常见模块：账号认证、会话管理、好友、聊天、邮件、奖励、任务、商城、抽卡、多人房间、语音鉴权。
- 熟悉 CMake + vcpkg 构建流程，具备 Windows / Linux 双平台构建与 systemd、Nginx 部署经验。
- 了解 OpenTelemetry OTLP HTTP 链路追踪与 Metrics 导出，能搭建 Prometheus 抓取链路。
- 了解性能压测方法：吞吐、成功率、P50/P95/P99、连接池等待、CPU 与依赖耗时分析。

## 四、项目经历

### 项目名称：model_framework —— 可复用游戏业务后端框架

**项目时间：** [开始时间] 至 [结束时间 / 至今]  
**项目角色：** 独立开发  
**项目简介：** 基于 C++20、Boost.Asio / Boost.Beast、MySQL、Redis 和 OpenTelemetry 开发的通用游戏后端骨架，面向 Unreal Engine 等游戏客户端。工程拆分为 HTTP API 服务、WebSocket 聊天网关和后台任务 Worker，覆盖账号认证、玩家资料、好友、聊天、邮件奖励、任务、商城、抽卡、掉落、多人房间和 LiveKit 语音鉴权等业务链路。

**技术栈：** C++20、Boost.Asio、Boost.Beast、Boost.JSON、Boost.MySQL、Redis、MySQL、OpenTelemetry C++、OpenSSL、CMake、vcpkg、Prometheus、systemd、Nginx。

**主要工作：**

- 设计分层架构，Router 负责路径匹配、Handler 组织响应、Service 承载业务规则、Repository 隔离数据访问，同一 Service 可被 HTTP、Worker 与测试复用。
- 实现账号安全链路：注册、登录、Argon2id 密码哈希、Access Token / Refresh Token 会话管理、单端登录、踢下线、冻结封禁、黑名单、验证码与登录限流。
- 实现聊天与社交链路：大厅 / 房间 / 私聊频道、好友申请与关系校验、在线状态、未读与离线消息、禁言、撤回和 GM 公告。
- 实现邮件奖励、任务与运营后台：系统邮件、批量发奖、奖励流水、任务目标进度、事件幂等、后台任务记录与失败重试补偿。
- 基于 Repository 抽象支持 `in_memory`、`MySQL`、`Redis` 三种存储模式，按数据性质拆分 MySQL 业务事实与 Redis 缓存/会话。
- 使用 OpenTelemetry 接入 OTLP HTTP 链路追踪与 Metrics 导出，响应返回 `X-Trace-Id`、`X-Response-Time-Ms`，并提供 `/metrics`、最近 span 调试窗口。

**技术亮点：**

- 聊天消息采用 MySQL Outbox + `FOR UPDATE SKIP LOCKED` 消费模式，先写 Outbox 再广播，配合 `message_id` 主键和 `ON DUPLICATE KEY UPDATE` 实现崩溃恢复与幂等写入。
- 登录热路径使用 32 路分片缓存锁，同一账号请求合并、不同账号并行回源，降低冷缓存登录的相互阻塞。
- 修复 MySQL `Prepared_stmt_count` 持续增长问题，通过按连接缓存预处理语句将计数从近万个降到不足一百，压测中 500 错误清零。
- 使用 Redis Lua 脚本原子实现注册限流计数，避免 `INCR` 与 `EXPIRE` 分离导致的竞态。
- 搭建本地 MySQL / Redis 联调、冒烟测试、性能压测、备份恢复与生产配置检查脚本，支持 Linux systemd / Nginx 部署。

**量化结果（本机基准，非生产容量）：**

- HTTP `/health` 短请求：Bombardier 同机压测约 `15.3k~15.8k RPS`，成功率 100%，128 连接 P99 约 `11ms`。
- 真实 MySQL + Redis 登录：16 并发约 `753~845 RPS`，P99 约 `25~34ms`。
- 关闭登录校验缓存的冷密码校验：单进程 16 个哈希线程约 `227 RPS`，P99 约 `138ms`。
- 真实 MySQL + Redis 注册：32 并发约 `155~175 RPS`，P99 约 `295~310ms`。
- 真实 MySQL + Redis 读取类接口：同机测试约 `4200 RPS`。

> 注意：以上数字是开发机同机测试结果，用于证明性能优化与观测能力，不应描述成生产容量或线上 SLA。

## 五、可直接粘贴的“项目经历”精简版

**model_framework（C++ 游戏后端框架）｜独立开发**

- 用 C++20 与 Boost.Asio/Beast/JSON 实现可复用游戏后端骨架，拆分为 HTTP API 服务、WebSocket 聊天网关、后台任务 Worker 三个进程，服务 Unreal Engine 等游戏客户端。
- 完成账号认证与安全链路：注册登录、Argon2id 密码哈希、双 Token 会话、单端登录、踢下线、冻结封禁、黑名单、验证码与登录限流。
- 完成社交与业务链路：好友、大厅/房间/私聊、未读与离线消息、邮件奖励、任务目标进度、商城、抽卡、掉落、多人房间与 LiveKit 语音鉴权。
- 用 Repository 抽象实现 `in_memory / MySQL / Redis` 三种存储切换，MySQL 存业务事实、Redis 存会话与热点缓存。
- 聊天采用 MySQL Outbox + `SKIP LOCKED` + 主键幂等实现崩溃恢复；登录热路径使用 32 路分片锁，不同账号并行回源。
- 接入 OpenTelemetry OTLP HTTP 链路追踪与 Metrics，提供 `X-Trace-Id`、`X-Response-Time-Ms`、`/metrics` 与最近 span 调试。
- 修复 MySQL 预处理语句泄漏，压测中 500 错误清零；HTTP 同机短请求约 15k RPS，真实 MySQL+Redis 登录 16 并发约 750+ RPS。

## 六、面试提示

- 介绍时强调“分层架构 + 可复用 + 可靠性”，不要只说堆了哪些接口。
- 技术亮点优先讲 Outbox 幂等、登录分片锁、预处理语句泄漏修复、OTel 可观测性。
- 性能数据一律标注“本机基准、非生产容量”，展示的是优化方法和观测能力。
- 主动说明还缺什么：角色系统、完整背包货币规则、支付订单、跨区与大规模集群治理仍在规划中。
