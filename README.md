# model_framework

[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue)]()
[![Boost](https://img.shields.io/badge/Boost-Asio%2FBeast-green)]()
[![Storage](https://img.shields.io/badge/Storage-MySQL%20%7C%20Redis-orange)]()
[![Observability](https://img.shields.io/badge/OpenTelemetry-OTLP-purple)]()

面向 UE 等游戏客户端、基于现代 C++20 和 Boost 的可复用游戏业务后端骨架。核心目标是让网络层、业务层和存储层可拆分、可替换、可观测，而不是只服务单个 demo。

## 项目亮点

- **网络与背压**：Boost.Asio 线程池 + Boost.Beast HTTP/WebSocket，限制 `max_inflight`、`max_pending`，写请求按 key 串行化和限流。
- **存储抽象**：Repository 接口统一 `in_memory` 与 `MySQL + Redis`，账号、会话、好友、聊天、邮件按数据性质分工。
- **安全链路**：Argon2id、注册 / 登录 / 改密 / 会话刷新、封禁黑名单、验证码、登录限流。
- **可靠消息**：聊天 MySQL Outbox + `SKIP LOCKED` + 主键幂等，支持进程重启恢复与失败重试。
- **可观测性**：OpenTelemetry OTLP tracing/metrics，响应返回 `X-Trace-Id` 与 `X-Response-Time-Ms`，保留本地 `/metrics`。
- **性能与正确性**：定位并修复并发背包写覆盖、MySQL Prepared Statement 泄漏，详见 [性能基线](docs/performance-baseline-20260924.md)。

## 项目定位

这不是“只给当前 demo 用的一次性服务”，而是一个面向大多数联机游戏项目的通用后端骨架。当前设计目标是：

- 网络层和业务层分离，HTTP / WebSocket / 后续实时协议可以并存
- 业务模块按领域拆分，避免把账号、社交、聊天、商城全堆在一个巨型服务里
- 存储层先抽象为仓储接口，便于按数据性质切到 `MySQL`、`PostgreSQL`、`Redis`
- 保留 `in_memory` 实现，方便先 review 业务语义，再切真实中间件
- 面向 UE 等客户端长期协作，强调稳定协议、可读注释、可扩展错误码

这套骨架更适合承载下面这类通用游戏后端能力：

- 账号、角色、背包、货币、邮件、商城、活动、配置
- 当前已落地邮件与奖励发放基础链路，可给后续活动、商城、补偿复用
- 好友、聊天、在线状态、未读、离线消息
- GM / 后台管理、封禁、公告、审计、风控
- 热点状态与实时信令

## 当前能力

- 基于 `Boost.Asio` 的事件循环和线程池
- 基于 `Boost.Beast` 的 HTTP 服务
- 基于 `Boost.Beast` 的 WebSocket 聊天网关
- 简单路由分发：`method + path -> handler`
- 中间件链：可统一接日志、鉴权、异常边界
- JSON 响应辅助函数，基于 `Boost.JSON`
- 环境变量配置加载
- 简单结构化日志接口
- 官方 `opentelemetry-cpp` metrics exporter：支持 OTLP HTTP 导出，同时保留 `GET /metrics` 便于本地 Prometheus 文本抓取
- 请求 tracing 基础：每个 HTTP 响应都会回 `X-Trace-Id` 和 `X-Response-Time-Ms`
- 官方 `opentelemetry-cpp + OTLP HTTP exporter` tracing：支持 `traceparent` 传播、最近 span 调试窗口和 HTTP/任务/MySQL 串联
- 游戏业务骨架：登录、玩家资料、好友、聊天历史
- 正式账号链路：注册、密码登录、Argon2id 密码哈希
- 账号安全能力：改密、找回重置、账号冻结/封禁、黑名单、验证码、登录限流
- 会话管理：`access token + refresh token`、会话列表、单端策略、踢下线、登出
- 明确的聊天频道模型：大厅、房间、好友私聊
- 明确的消息类型：`text`、`system`
- 好友申请、好友私聊权限校验、聊天黑名单
- 聊天治理：频道禁言、消息撤回、GM 公告
- 聊天状态：在线状态、频道成员、好友未读、好友离线消息
- 邮件与奖励：系统邮件、批量邮件、附件领取、奖励流水
- GM/后台：管理员账号、权限表、操作审计日志、奖励模板
- 管理员后台接口：管理员登录、管理员列表/详情、后台查询、批量发奖、批量邮件、定时公告
- 后台任务表：定时公告、批量发奖、批量邮件都会形成后台任务记录，可按任务类型/状态查询
- 后台任务补偿入口：支持把已有任务按“整单重试”或“仅失败项重试”重新投递
- 持久化任务系统：任务定义、Objective 进度、StoryFlags、事件幂等、自动/手动/NPC 领奖
- 原子奖励与拾取：MySQL 模式下奖励流水、玩家资产、掉落状态在同一事务提交
- LiveKit 语音鉴权第一阶段：成员资格、短期 JWT、服务端禁言/封禁和审核记录
- 仓储接口和内存实现，结构上已经按 `MySQL + Redis` 分工整理
- `Application` 作为统一启动入口

## 本地配置与安全

仓库只保留 `change_me` 占位值，不包含真实开发/生产密码。运行前请通过环境变量注入：

```powershell
$env:HELLO_SERVICE_MYSQL_PASSWORD = "..."
$env:HELLO_SERVICE_DEV_ADMIN_PASSWORD = "..."
$env:HELLO_SERVICE_GM_TOKEN = "..."
```

不要提交 `.env`、`.pem`、`.key` 或真实 token；这些文件已在 `.gitignore` 中排除。

## 当前已完成 / 还缺什么

聊天异步持久化的重试、幂等语义和当前可靠性边界见 [工程化加固说明](docs/engineering-hardening.md)。

当前已经完成的，主要是“接入层 + 社交层 + 基础安全层”：

- HTTP 服务、WebSocket 聊天网关、配置加载、日志、中间件链
- 正式账号注册 / 登录 / 改密 / 找回 / 冻结封禁 / 黑名单 / 验证码 / 登录限流
- 会话签发、刷新、会话列表、单端策略、踢下线
- 玩家资料基础链路
- 邮件系统与奖励发放基础链路
- 好友申请、好友关系、好友在线状态聚合
- 大厅 / 房间 / 好友私聊三类聊天频道
- 好友私聊未读、离线消息、聊天黑名单、禁言、撤回、GM 公告
- `MySQL + Redis` 双存储模式，以及 `in_memory` review 模式
- 聊天 MySQL Outbox：消息先可靠写入 Outbox，后台事务 worker 可在进程重启后恢复投递

当前还缺的，主要是“游戏业务核心层 + 运维治理层”：

1. 角色系统
   还没有完整的创建角色、角色列表、选角、角色扩展属性模型。
2. 背包与货币
   已有基础货币与背包入账链路，背包已升级为“道具实例/堆叠格”模型；
   但仍缺更完整的非堆叠装备属性、消耗/拆分、货币流水明细、背包容量规则。
3. 商城与订单
   还没有商品配置、订单、支付回调、补单、发货审计。
4. 邮件与奖励
   已有系统邮件、附件奖励、奖励模板、领取入账、奖励流水；
   但还缺更完整的批量运营投放、失效策略、补发和异步任务编排。
5. 配置与活动
   还没有配置版本、热加载、活动开关、定时生效、灰度策略。
6. GM / 后台
   已有管理员账号、权限表、Bearer 会话鉴权、操作审计；
   但还缺更完整的后台登录界面、查号工具、分页筛选、权限组与菜单体系。
7. 更完整的聊天治理
   还缺敏感词、举报、聊天审计、GM 强制撤回、全局禁言、发言频率限制。
8. 监控与运维
   已补 `GET /metrics`、请求 trace header、任务执行指标、任务失败项重试入口；
   但还缺真正接外部告警通道、全量慢 SQL 统计、跨进程 tracing 后端和自动补偿策略。
9. 协议治理
   还需要更系统的错误码规范、分页规范、版本兼容策略和客户端协作文档。

如果目标是“适用于大部分游戏”，建议把后续建设重点放在：

1. `角色 + 背包 + 货币流水`
2. `邮件 + 奖励发放`
3. `商城 + 订单`
4. `配置 + 活动`
5. `GM / 审计 / 风控`
6. `监控 / tracing / 自动化测试`

## 目录结构

```text
include/mf/core     应用生命周期和顶层接口
include/mf/http     HTTP 类型、路由、服务接口
include/mf/auth     Token 认证
include/mf/game     领域模型、仓储接口、JSON 映射
include/mf/chat     聊天服务和 WebSocket 网关
include/mf/storage  存储模式配置和 review 导读
src/                具体实现
examples/hello      最小示例服务
db/mysql            MySQL schema
docs/review         review 导读文档
```

## 构建

要求：

- CMake 3.21+

## 自动化测试

跨平台的接口自动化测试，用 `pytest + requests` 编写，不依赖 PowerShell：

```bash
python -m pip install -r tests/requirements.txt
python -m pytest tests -v
```

- 默认用 `in_memory` 内存模式启动服务，不依赖 MySQL / Redis，任何装了 Python 的机器都能跑
- 覆盖系统/健康检查、认证、任务、邮件、掉落、奖励、商城、抽卡、好友、聊天、后台管理查询等模块
- 快速回归 **38 项全部通过**（另有浏览器 4 项、MySQL/Redis/WebSocket 隔离集成 19 项，见 tests/README.md）
- 用例与模块说明见 [tests/README.md](tests/README.md)

## Tracy CPU 性能分析（VS Code）

### 自动采样函数热点（Windows）

在 VS Code 的“运行任务”中执行 `profile hello_service CPU functions`，或运行以下命令。首次会弹出 Windows 管理员权限确认（WPR 的 CPU 采样需要该权限）：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\profile-cpu.ps1
```

脚本首次构建 `RelWithDebInfo`（保留 PDB），用独立端口 18081 启动内存版服务，使用 WPR 录制 CPU 采样，自动完成注册/登录及 500 次资料、玩家列表、配置读取。结束后在终端显示摘要，并生成 `build-tracy/cpu-profiles/hello_service_*.report.md`：先看请求失败数与客户端 P95/P99 耗时，再看进程模块和项目函数采样占比。逐次请求数据在同名 `.requests.csv`，另有 `.project-functions.csv` 和 `.top-functions.csv`。`-OpenViewer` 才额外打开 WPA。已有 ETL 无须重新采集：运行任务 `show latest CPU function hotspots` 或运行 `powershell -ExecutionPolicy Bypass -File .\scripts\export-cpu-hotspots.ps1` 重新生成报告；旧 ETL 没有 `.requests.csv` 时只显示 CPU 数据。`Weight` 是采样 CPU 权重，**不是函数耗时**；请求毫秒数是客户端计时，包含 PowerShell 和本机 HTTP 开销。想检查完整调用链再使用 WPA。需要 Windows 10/11 ADK 的 Windows Performance Toolkit（WPA/xperf 10.x 或更高）。采样仅覆盖执行到的函数，内存版的串行请求不代表并发性能，也不会测试 MySQL、Redis 或语音业务。`-Requests 2000` 可增加采样量，`-Port 18082` 可更换端口，`-Build` 可强制重新编译。脚本不会停止已有的 WPR 录制。

新采样另存 `.workload.json`（整轮串行请求耗时），报告同时显示串行请求数/秒；这个数不能当作并发 QPS。项目函数即使排第一，只要占进程采样比例很低，报告也会提示尚无可靠热点。业务采样可运行 VS Code 任务 `profile hello_service business CPU functions`，或执行 `powershell -ExecutionPolicy Bypass -File .\scripts\profile-cpu.ps1 -Business -Requests 600`：它在隔离的内存版服务中注册两个账号，建立好友关系并发送邮件，再请求资料、好友、邮件、聊天未读、任务、商城和抽卡读取接口。这个模式不访问线上数据库，空的卡池、任务定义等不能代表生产业务；精确的单函数耗时需要针对目标函数使用 Tracy 埋点，CPU 采样只能显示样本权重与占比。

Tracy 与现有 OpenTelemetry 请求追踪独立；默认 `MF_ENABLE_TRACY=OFF`。当前 CPU 区域覆盖 HTTP 业务线程/路由、MySQL 连接池和查询、Redis 同步调用。长区间也可能是网络/数据库等待，并不代表 CPU 一直繁忙。

本地并发业务延迟测试：先执行 `cmake --build build-tracy --config RelWithDebInfo --target hello_service`，然后在 VS Code 中运行 `benchmark business latency (local in-memory)`，或执行 `powershell -ExecutionPolicy Bypass -File .\scripts\profile-business-latency.ps1 -DurationSeconds 10 -Concurrency 24`。报告保存在 `build-tracy/cpu-profiles/business_latency_*.txt`。`server_p95/p99` 来自服务端 `X-Response-Time-Ms`，只覆盖 HTTP 业务中间件，不包含排队、传输或客户端耗时；`client_p95/p99` 包含本机 HTTP 开销。Tracy 构建额外返回 `queue_p95/p99`（入队到业务线程开始）及 `processing_p95/p99`（读完请求到开始写回）；均不包括网络发送。报告中的 `server_cpu_cores` 和 `client_cpu_cores` 是各进程 CPU 时间除以压测墙钟时间。此脚本只在独立端口启动内存存储，建立两个账号、一位好友、一封邮件并并发访问 12 条读接口；未填充大量任务、商品、抽卡池或消息，不能当作 MySQL/Redis 生产环境吞吐。Tracy 中可查看 `Auth authenticate bearer`、`Memory session lookup`、`Memory friend list`、`Memory mail list`、`Memory gacha pools/history`、`Memory shop products/history` 等区间；区间显示墙钟耗时（含锁等待），不是全项目每个函数的自动 CPU 时间统计。阶梯测试结果和下一步见 `docs/performance-baseline-20260924.md`。

Windows 安装依赖（仅首次）：

```powershell
D:\vcpkg\vcpkg.exe install tracy:x64-windows
```

这一步需要能访问 GitHub；若提示 `WinHttpSendRequest 12002`，先检查系统代理或网络连接，依赖未安装前 Tracy 构建会在 `find_package(Tracy)` 阶段失败。

在 VS Code 的“运行任务”中执行 `build Tracy hello_service (Release)`，或在终端执行：

```powershell
cmake -S . -B build-tracy -A x64 -DMF_ENABLE_OTEL=OFF -DMF_ENABLE_TRACY=ON -DCMAKE_PREFIX_PATH=D:/vcpkg/installed/x64-windows
cmake --build build-tracy --config Release --target hello_service
```

一键启动：在 VS Code 运行任务 `start Tracy service + profiler`，或者运行：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\start-tracy-session.ps1 -Build
```

脚本先构建 Tracy Release 服务，确认 `127.0.0.1:8080/health` 可用，然后打开 Tracy Profiler（v0.11.1）并自动连接 `127.0.0.1:8086`。已经编译时可以省略 `-Build`；默认使用 `in_memory` 存储，需分析 MySQL/Redis 时传 `-StorageMode mysql_redis` 并自行提供相应连接环境变量。若 8080 已被其他服务占用，脚本会报错而不会停止现有进程。Profiler 可通过 `D:\vcpkg\vcpkg.exe install 'tracy[gui-tools]:x64-windows' --recurse` 安装。`build-tracy` 独立于原 `build`。仅在本机可信环境运行启用了 Tracy 的服务。

## 本地 OTel Collector 联调

如果你想把当前工程的官方 `opentelemetry-cpp` trace / metrics 导出到本地 collector，可以直接用仓库里的两个脚本：

1. 把 GitHub 下载的 collector 解压到 `D:\otelcol`
2. 启动 collector：
   `powershell -ExecutionPolicy Bypass -File .\scripts\start-otelcol.ps1`
3. 跑联调验证：
   `powershell -ExecutionPolicy Bypass -File .\scripts\test-otel-pipeline.ps1`

脚本做的事情分别是：

- `scripts/start-otelcol.ps1`
  自动生成 `D:\otelcol\config.yaml`，启 `OTLP gRPC(4317)` 和 `OTLP HTTP(4318)`，并把收到的 trace / metrics 通过 `debug exporter` 打到 `D:\otelcol\collector.log`
- `scripts/test-otel-pipeline.ps1`
  自动补齐 Debug 运行时 `PATH` 和 `OTEL_EXPORTER_OTLP_*` 环境变量，启动 `build\Debug\hello_service.exe`，请求 `/health`、`/metrics`、`/debug/traces`，最后检查 `collector.log` 里是否已经出现 trace / metrics

如果 `collector.exe` 路径不是默认位置，也可以给 `start-otelcol.ps1` 传：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\start-otelcol.ps1 `
  -CollectorExePath 'D:\otelcol\otelcol-contrib.exe'
```
- 支持 C++20 的编译器
- Boost 1.81+，包含 `system` 和 `json`

```powershell
cmake -S . -B build
cmake --build build
```

## 运行

```powershell
./build/hello_service
```

然后访问：

- `GET /health`
- `GET /config`
- `POST /auth/register`
- `POST /auth/login`
- `POST /auth/quick-login`
- `POST /auth/captcha`
- `POST /auth/change-password`
- `POST /auth/forgot-password`
- `POST /auth/reset-password`
- `POST /auth/refresh`
- `POST /auth/logout`
- `GET /auth/sessions`
- `POST /auth/sessions/revoke`
- `POST /auth/admin/account-status`
- `POST /auth/admin/blacklist`
- `POST /admin/bootstrap`
- `POST /admin/login`
- `GET /admin/me`
- `GET /admin/dashboard`
- `POST /admin/users/upsert`
- `GET /admin/users`
- `GET /admin/users/detail`
- `POST /admin/users/detail`
- `GET /players/me`
- `GET /players`
- `POST /rewards/grant`
- `GET /rewards/grants`
- `POST /admin/reward-templates`
- `GET /admin/reward-templates`
- `GET /admin/audit-logs`
- `GET /admin/query/account`
- `POST /admin/query/account`
- `GET /admin/query/player`
- `POST /admin/query/player`
- `GET /admin/query/inventory`
- `POST /admin/query/inventory`
- `GET /admin/query/mail`
- `POST /admin/query/mail`
- `GET /admin/query/reward-grants`
- `POST /admin/query/reward-grants`
- `GET /admin/query/blacklists`
- `POST /admin/query/blacklists`
- `POST /admin/batch/rewards`
- `POST /admin/batch/mails`
- `POST /admin/tasks/announcements`
- `GET /admin/tasks/announcements`
- `POST /admin/tasks/query`
- `POST /admin/tasks/retry`
- `GET /debug/traces`
- `POST /mail/send`
- `GET /mail`
- `POST /mail/read`
- `POST /mail/claim`
- `POST /mail/delete`
- `POST /friends/add`
- `POST /friends/request`
- `GET /friends`
- `GET /friends/requests`
- `POST /friends/respond`
- `GET /friends/presence`
- `GET /chat/history/lobby`
- `GET /chat/channels/examples`
- `GET /chat/unread`
- `POST /chat/read`
- `POST /chat/block`
- `GET /chat/blocks`
- `POST /chat/mute`
- `POST /chat/recall`
- `POST /chat/gm/announce`
- `POST /chat/offline/pull`
- `POST /chat/presence/player`
- `POST /chat/presence/channel`

也可以通过环境变量覆盖配置：

```powershell
$env:HELLO_SERVICE_HOST='127.0.0.1'
$env:HELLO_SERVICE_PORT='9090'
$env:HELLO_SERVICE_THREADS='4'
$env:HELLO_SERVICE_NAME='hello-from-env'
$env:HELLO_SERVICE_ENV='development'
$env:HELLO_SERVICE_GM_TOKEN='change_me_gm_token'
$env:HELLO_SERVICE_STORAGE_MODE='mysql_redis'
$env:HELLO_SERVICE_MYSQL_HOST='127.0.0.1'
$env:HELLO_SERVICE_MYSQL_PORT='3306'
$env:HELLO_SERVICE_MYSQL_USER='game_backend'
$env:HELLO_SERVICE_MYSQL_PASSWORD='change_me_mysql_password'
$env:HELLO_SERVICE_MYSQL_DATABASE='game_backend'
$env:HELLO_SERVICE_MYSQL_AUTO_MIGRATE='1'
$env:HELLO_SERVICE_MYSQL_SCHEMA_FILE='db/mysql/001_game_backend.sql'
$env:HELLO_SERVICE_REDIS_HOST='127.0.0.1'
$env:HELLO_SERVICE_REDIS_PORT='6379'
$env:HELLO_SERVICE_REDIS_DB='0'
$env:HELLO_SERVICE_AUTO_BOOTSTRAP_DEV_ADMIN='true'
$env:HELLO_SERVICE_DEV_ADMIN_LOGIN='admin_demo'
$env:HELLO_SERVICE_DEV_ADMIN_PASSWORD='change_me_admin_password'
```

注意：

- MySQL 用户名环境变量名是 `HELLO_SERVICE_MYSQL_USER`，不是 `HELLO_SERVICE_MYSQL_USERNAME`。
- 本地开发建议 `HELLO_SERVICE_ENV=development`；服务器部署建议 `HELLO_SERVICE_ENV=production`。
- `production` 会禁用 `/auth/quick-login`，并强制关闭开发管理员自动创建。
- `HELLO_SERVICE_MYSQL_AUTO_MIGRATE=1` 会在启动时自动执行 `db/mysql/001_game_backend.sql`，用于补齐缺失表和列。
- 服务启动时即使还没访问 MySQL，也可能先显示监听成功；真正第一次打到数据库接口时才会暴露账号或连接配置错误。
- `HELLO_SERVICE_SERIALIZE_MUTATING_REQUESTS=true`（默认）时，普通写请求仍按会话串行，但以下已由数据库行锁/幂等写保证原子性的接口会直接并发：`/players/me/inventory/item`、`/shop/buy`、`/loot/pickup`、`/quests/accept`、`/quests/events`、`/quests/progress`、`/quests/claim`、`/mail/read`、`/mail/claim`、`/mail/delete`、`/gacha/draw`，以及只读但走 POST 的 `/shop/history`、`/gacha/history`。

其中管理员体系现在分成两层：

```text
1. POST /admin/bootstrap
   使用启动时配置的 X-GM-Token 做一次性管理员引导

2. 其他 GM / 后台接口
   使用普通账号先登录拿到 Bearer token
   再根据 session.account_id -> admin_account/admin_permission 做管理员身份与权限校验

说明：

- 当前后台查询接口同时保留了 `GET` 和 `POST` 两种风格。
- 由于示例路由器对带 query string 的精确匹配不够稳定，实际联调建议优先走 `POST` 版本。
- `GET /admin/query/mail`、`GET /admin/query/reward-grants`、`GET /admin/tasks/announcements` 现在也统一返回 `{ total, items }` 分页结构。
- `GET/POST /admin/audit-logs*`、`GET /admin/tasks/announcements`、`POST /admin/tasks/query` 已改为 keyset 游标分页，返回 `{ total, items, has_more, next_created_at, next_audit_id/next_task_id }`，不再使用 `offset` 深分页。
- 当前“定时公告任务”已经升级为 `MySQL admin_task` 任务表 + 进程内 worker 轮询执行。
- 这样任务记录可以跨请求保留，worker 会把状态从 `scheduled -> running -> completed/failed` 回写到任务表。
- 当前 worker 仍然和 `hello_service` 进程同生共死；如果后续要做多实例部署，建议再拆成独立 worker 进程。
- `GET /metrics` 当前先暴露进程内 HTTP 请求数/耗时、后台任务执行数/耗时，适合本地 Prometheus 或 curl 直接查看。
- `GET /debug/traces` 会返回最近完成的 span，便于本地 review 同一条 trace 上的 HTTP 请求、后台任务和 MySQL 仓储操作。
- `POST /admin/tasks/retry` 当前已支持 `announce`、`batch_mute`、`batch_account_status` 三类任务重投。
- 对批量任务传 `failed_only=true` 时，会优先把上次结果里的失败 `player_id` 子集重新生成新任务，这就是当前最小补偿闭环。
- 这轮已经切到官方 `opentelemetry-cpp` SDK，并通过 `vcpkg` 接了 `opentelemetry-cpp[otlp-http]`。
- 现在导出链路是：`BatchSpanProcessor -> OTLP HTTP exporter`，同时保留一个本地调试 exporter，把最近 span 暴露给 `GET /debug/traces` 和后台 tracing 页面。
- metrics 现在导出链路是：`MeterProvider -> PeriodicExportingMetricReader -> OTLP HTTP metric exporter`，同时继续保留进程内快照给 `GET /metrics`。
- 本地 F5 调试环境已默认注入：
  - `OTEL_EXPORTER_OTLP_ENDPOINT=http://127.0.0.1:4318`
  - `OTEL_EXPORTER_OTLP_TRACES_ENDPOINT=http://127.0.0.1:4318/v1/traces`
  - `OTEL_EXPORTER_OTLP_METRICS_ENDPOINT=http://127.0.0.1:4318/v1/metrics`
- 本地 Debug 运行时如果提示缺 DLL，需要把 `D:\vcpkg\installed\x64-windows\debug\bin` 和 `D:\OpenSSL-Win64\bin` 放进 `PATH`。
```

## MySQL + Redis 本地联调

当前代码已经支持：

- `AccountRepository -> MySQL`
- `PlayerRepository -> MySQL`
- `FriendRepository -> MySQL`
- `RewardRepository -> MySQL`
- `RewardTemplateRepository -> MySQL`
- `MailRepository -> MySQL`
- `AuditLogRepository -> MySQL`
- `AdminRepository -> MySQL`
- `ChatRepository -> MySQL + Redis 最近窗口缓存`
- `SessionRepository -> Redis`
- `AuthSecurityRepository -> Redis`

仓库里还附带了 3 个 PowerShell 脚本，方便你本地联调：

- [scripts/start-local-mysql.ps1](/d:/code/model/scripts/start-local-mysql.ps1)
  启动 `D:\mysql\bin\mysqld.exe`
- [scripts/start-local-redis.ps1](/d:/code/model/scripts/start-local-redis.ps1)
  启动 `D:\redis\redis-server.exe`
- [scripts/run-hello-mysql-redis.ps1](/d:/code/model/scripts/run-hello-mysql-redis.ps1)
  配好 `HELLO_SERVICE_*` 环境变量并启动服务
- [scripts/smoke-http.ps1](/d:/code/model/scripts/smoke-http.ps1)
  对 `/config`、`/auth/register`、`/auth/login`、`/auth/quick-login`、`/players/me`、`/players` 做最小冒烟验证

建议流程：

```powershell
cmake --build build --config Debug
.\scripts\start-local-mysql.ps1
.\scripts\start-local-redis.ps1
.\scripts\run-hello-mysql-redis.ps1
```

如果你希望让脚本顺手拉起 MySQL / Redis，并重新执行 schema：

```powershell
.\scripts\run-hello-mysql-redis.ps1 `
  -StartMySql `
  -StartRedis `
  -ApplySchema `
  -MySqlCliPath 'D:\mysql\bin\mysql.exe'
```

服务起来后，再开一个终端执行：

```powershell
.\scripts\smoke-http.ps1
```

注意：

- 当前机器上已经检测到 `D:\redis\redis-server.exe`
- 当前机器上已经安装 `D:\mysql\bin\mysql.exe`
- 当前本地开发数据库默认信息如下：
- `root` 密码：`D_mysql_root_2026!`
- 应用账号：`game_backend`
- 应用密码：`change_me_mysql_password`

### Redis 主从 + Sentinel 高可用

单机直连之外，Redis 支持通过 Sentinel 发现并连接当前 master，master 故障时自动切换，服务侧无需重启：

- 环境变量：`HELLO_SERVICE_REDIS_SENTINELS`（逗号分隔的 `host:port`，如 `127.0.0.1:26379,127.0.0.1:26380,127.0.0.1:26381`）
- 环境变量：`HELLO_SERVICE_REDIS_SENTINEL_MASTER_NAME`（如 `mymaster`）
- 两者同时配置后，`HELLO_SERVICE_REDIS_HOST/PORT` 会被忽略，连接池与 Pub/Sub 订阅器都会通过 Sentinel 解析 master。
- 启动脚本参数：`start-hello-service.ps1` / `start-hello-service-cluster.ps1` 已支持 `-RedisSentinels` 与 `-RedisSentinelMasterName`。
- 一键拉起/停止本地 HA 拓扑：`scripts/start-local-redis-ha.ps1`（默认 1 主 6380 + 1 从 6381 + 3 哨兵 26379~26381；`-Stop` 停止，`-StopExisting` 重启）。

```powershell
.\scripts\start-hello-service.ps1 `
  -ServiceExePath 'D:\code\model\build-tracy\RelWithDebInfo\hello_service.exe' `
  -RedisSentinels '127.0.0.1:26379,127.0.0.1:26380,127.0.0.1:26381' `
  -RedisSentinelMasterName 'mymaster'
```

注意事项：

- `Boost.Redis` 原生支持 Sentinel，但不支持 Cluster；主从 + Sentinel 能保留 Lua、多 key 原子操作、Pub/Sub 语义，最适合当前会话撤销（`session:revocation`）与单会话原子写。
- 连接带健康检查（默认 2s 心跳）与自动重连，重连时会重新向 Sentinel 解析 master，因此 master 切换后服务会自动连到新 master。
- 已验证：一主一从 + 3 哨兵（quorum=2）下，杀掉 master 后从库被提升，登录/注册/会话写与多 key 撤销脚本均在新 master 上正常执行。

## HTTP 示例

正式注册：

```powershell
$register = Invoke-RestMethod -Method Post -Uri http://127.0.0.1:8080/auth/register -ContentType 'application/json' -Body '{"login_name":"alice","display_name":"Alice","password":"change_me"}'
```

正式登录：

```powershell
$login = Invoke-RestMethod -Method Post -Uri http://127.0.0.1:8080/auth/login -ContentType 'application/json' -Body '{"login_name":"alice","password":"change_me"}'
$token = $login.data.session.token
$refreshToken = $login.data.session.refresh_token
```

开发快速登录：

```powershell
$quick = Invoke-RestMethod -Method Post -Uri http://127.0.0.1:8080/auth/quick-login -ContentType 'application/json' -Body '{"login_name":"alice_dev","display_name":"AliceDev"}'
```

带 token 访问玩家资料：

```powershell
Invoke-RestMethod -Uri http://127.0.0.1:8080/players/me -Headers @{ Authorization = "Bearer $token" }
```

刷新会话：

```powershell
Invoke-RestMethod -Method Post -Uri http://127.0.0.1:8080/auth/refresh -ContentType 'application/json' -Body "{""refresh_token"":""$refreshToken""}"
```

修改密码：

```powershell
Invoke-RestMethod -Method Post -Uri http://127.0.0.1:8080/auth/change-password -Headers @{ Authorization = "Bearer $token" } -ContentType 'application/json' -Body '{"current_password":"change_me","new_password":"change_me_2"}'
```

## WebSocket 聊天协议

聊天网关默认监听 `ws://127.0.0.1:8090`。

客户端连接后发送：

```json
{"type":"auth","token":"<login token>"}
{"type":"join","channel_kind":"lobby","scope_id":"global"}
{"type":"join","channel_kind":"room","scope_id":"room_1001"}
{"type":"join","channel_kind":"friend_direct","friend_player_id":"player_2"}
{"type":"say","message_kind":"text","text":"hello lobby"}
{"type":"recall","message_id":"msg_1001"}
{"type":"history","channel_kind":"lobby","scope_id":"global","limit":20}
```

## 聊天模型

现在聊天不再只靠一个裸字符串频道名，而是显式区分：

- `lobby`
  大厅频道，例如 `lobby:global`
- `room`
  局内房间频道，例如 `room:room_1001`
- `friend_direct`
  好友私聊频道，例如 `friend:player_1:player_2`

## 聊天状态

当前聊天状态层已经支持：

- 在线状态
- 频道成员列表
- 好友私聊未读计数
- 好友私聊离线消息

好友接口说明：

- `GET /friends` 现在已经直接合并 `online`、`connection_count`、`unread_count`、`last_message`
- `GET /friends` 还会带 `friend_nickname`，并按最近私聊时间倒序返回
- `GET /friends/presence` 仍然保留，方便单独调试批量在线态查询

说明：

- 当前“未读计数 / 离线消息”先聚焦 `friend_direct`
- `lobby` / `room` 如果要做精确未读，后续还需要再补“已读游标”语义

## 背包模型

当前 `PlayerProfile.inventory` 不再是 `vector<string>`，而是明确的堆叠模型：

- `instance_id`
  代表背包里这个格子的稳定实例标识，后续可继续挂耐久、绑定、词条等属性。
- `item_id`
  代表道具类型，例如 `potion_small`、`rare_ticket`。
- `quantity`
  代表这个格子当前堆叠数量。

当前默认策略是：

- 同一个 `item_id` 优先叠加到已有格子
- 新类型道具会创建新的 `InventoryItem`
- MySQL 里的 `inventory_json` 已兼容旧字符串数组读法，但新写入统一落成对象数组

## MySQL + Redis 分工

- `MySQL`
  负责账号、密码哈希、重置口令、账号状态、玩家资料、背包事实、好友关系、聊天事实持久化、奖励模板、邮件、审计日志、管理员权限表
- `Redis`
  负责 Session、刷新 token 索引、会话列表索引、验证码、登录限流、黑名单、聊天最近窗口缓存、好友私聊未读/离线消息、频道在线状态

对应 schema 在 [db/mysql/001_game_backend.sql](/d:/code/model/db/mysql/001_game_backend.sql)，
review 导读在 [docs/review/mysql_redis_architecture.md](/d:/code/model/docs/review/mysql_redis_architecture.md)。

## 框架演进建议

这个骨架刻意保持小而硬，不建议一开始就做成“大而全中台”。更稳的路线是：

1. 先把横切层做稳：网络、生命周期、配置、日志、错误码、存储抽象
2. 再把通用业务层做厚：账号、社交、聊天、角色、背包、邮件、商城
3. 最后补治理层：风控、审计、监控、异步任务、自动化测试

如果未来你要做成适用于大部分游戏项目的通用后端框架，建议长期保持这几个原则：

- 核心层只沉淀高复用能力，不把某个项目的特殊玩法直接写进框架
- 协议模型先稳定，再追求功能数量
- `MySQL` 存事实，`Redis` 存热点，避免职责混乱
- 每个业务模块都保留可 review 的内存实现，方便先验证语义
- 示例服务始终能独立跑通，避免框架演进后没人敢升级
