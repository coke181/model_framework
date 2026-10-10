# Hello Routes Review Guide

## 文档目的

这份文档用于帮助 review `examples/hello` 示例服务当前的路由拆分结构。

重点不是逐条解释每个 HTTP 接口，而是回答下面几个问题：

1. `main.cpp` 现在是怎么装配各模块的。
2. 每个 `hello_*.cpp` 文件分别负责什么。
3. 共享辅助函数放在哪里。
4. review 时建议先看什么，再看什么。

## 总体调用流程

入口文件：`[main.cpp](/d:/code/model/examples/hello/main.cpp:1)`

当前启动流程可以按下面顺序理解：

1. 读取环境变量，构造 `StorageOptions`、`AppOptions`。
2. 通过 `make_repositories(...)` 构造所有仓储。
3. 构造 `TokenService`、`AccountService`、`ChatService`、`RewardService`、`MailService`、`QuestService`。
4. 构造 `AdminAnnouncementScheduler`。
5. 启动 `ChatGateway` 线程。
6. 调用一组 `register_*_routes(...)`，把 HTTP 路由挂到 `Application`。
7. 进入 `app.run()`。

换句话说，`main.cpp` 现在只负责“装配”，业务细节已经下沉到各模块路由文件。

## 模块拆分

### 1. 系统路由

文件：`[hello_routes.cpp](/d:/code/model/examples/hello/hello_routes.cpp:1)`

职责：

- 注册 `/health`
- 注册 `/config`
- 注册 `/debug/load`
- 注册 `/debug/traces`
- 注册 `/debug/announcement-cache`
- 注册 `/system/announcements`

说明：

- 这是最薄的一层。
- 现在这个文件基本可以看作“系统只读路由模块”。
- 其中 `/debug/announcement-cache` 专门用来检查公告 Redis 最近窗口是否已经预热、当前 key 名是什么、窗口里有哪些公告。

### 2. 认证路由

文件：`[hello_auth_routes.cpp](/d:/code/model/examples/hello/hello_auth_routes.cpp:1)`

职责：

- 注册
- 登录
- 快速登录
- 验证码
- 改密码
- 忘记密码
- 重置密码
- 刷新 token
- 注销
- 会话列表
- 撤销指定会话

主要依赖：

- `AccountService`
- `TokenService`
- `require_session(...)`

### 3. 玩家侧路由

文件：`[hello_player_routes.cpp](/d:/code/model/examples/hello/hello_player_routes.cpp:1)`

职责：

- 玩家资料
- 邮件
- 好友
- 聊天

这是用户侧接口最多的一个模块，内部又可以这样看：

- `register_player_routes(...)`
  - 玩家资料读取
- `register_mail_routes(...)`
  - 邮箱收件箱、已读、领取、删除
- `register_friend_routes(...)`
  - 好友申请、接受、好友列表、presence 聚合
- `register_chat_routes(...)`
  - 历史消息、未读、拉离线、禁言、撤回、聊天黑名单

### 4. 奖励与投递路由

文件：`[hello_reward_routes.cpp](/d:/code/model/examples/hello/hello_reward_routes.cpp:1)`

职责：

- 单发奖励
- 奖励模板维护
- 奖励流水查询入口
- 批量发奖
- 单发邮件
- 批量邮件

主要依赖：

- `RewardService`
- `MailService`
- `AdminTaskRepository`
- `AuditLogRepository`

这层的特点是：

- 既有玩家可见结果
- 也有后台运营投放能力
- 很多接口会同时写审计日志和后台任务记录

### 5. 后台查询路由

文件：`[hello_admin_routes.cpp](/d:/code/model/examples/hello/hello_admin_routes.cpp:1)`

职责：

- 后台各种查询接口
- 账号、玩家、邮件、奖励、聊天等后台查阅入口

特点：

- 以读接口为主
- 注重分页和筛选
- 适合后台页面直接消费

### 6. 后台入口 / 用户 / 审计 / GM 聊天

文件：`[hello_admin_more_routes.cpp](/d:/code/model/examples/hello/hello_admin_more_routes.cpp:1)`

职责：

- 管理员登录入口
- bootstrap 初始管理员
- 管理员信息
- 管理员账户与权限维护
- 风控与黑名单相关接口
- 后台审计查询
- GM 公告聊天

可以把它理解成：

- “后台身份入口”
- “后台用户管理”
- “后台审计和 GM 运维控制”

### 7. 后台任务调度

文件：`[hello_admin_task_routes.cpp](/d:/code/model/examples/hello/hello_admin_task_routes.cpp:1)`

职责：

- 定时公告任务
- 公告调度 worker
- 后台任务相关接口

特点：

- 这里不只是纯 HTTP 路由
- 还包含一个后台工作线程模型 `AdminAnnouncementScheduler`

### 8. 共享辅助函数

头文件：`[hello_route_support.hpp](/d:/code/model/examples/hello/hello_route_support.hpp:1)`

实现文件：`[hello_route_support.cpp](/d:/code/model/examples/hello/hello_route_support.cpp:1)`

职责：

- session 鉴权
- admin 权限校验
- JSON 字段读取
- 请求 IP / UA / trace id 提取
- 审计日志追加
- 批量目标玩家解析
- 好友列表聚合
- 聊天频道解析
- rewards 数组解析

设计目的：

- 避免每个路由文件复制同一套辅助逻辑
- 让路由模块只关注业务分发，不反复写样板代码

## 当前 main.cpp 装配顺序

`main.cpp` 里现在按下面顺序注册路由：

1. `register_system_routes(...)`
2. `register_auth_routes(...)`
3. `register_player_routes(...)`
4. `register_mail_routes(...)`
5. `register_friend_routes(...)`
6. `register_chat_routes(...)`
7. `register_reward_routes(...)`
8. `register_admin_entry_routes(...)`
9. `register_admin_user_routes(...)`
10. `register_admin_audit_routes(...)`
11. `register_admin_query_routes(...)`
12. `register_admin_chat_routes(...)`
13. `register_admin_task_routes(...)`

这个顺序更多是“读起来方便”，不是严格业务依赖顺序。

## 推荐 review 顺序

如果你是第一次 review 这套代码，建议按下面顺序看：

1. `[main.cpp](/d:/code/model/examples/hello/main.cpp:1)`
   - 先理解服务怎么装起来。
2. `[hello_routes.hpp](/d:/code/model/examples/hello/hello_routes.hpp:1)`
   - 看清有哪些模块入口函数。
3. `[hello_route_support.hpp](/d:/code/model/examples/hello/hello_route_support.hpp:1)` 和 `[hello_route_support.cpp](/d:/code/model/examples/hello/hello_route_support.cpp:1)`
   - 先把公共样板理解掉。
4. `[hello_auth_routes.cpp](/d:/code/model/examples/hello/hello_auth_routes.cpp:1)`
   - 认证链路最基础。
5. `[hello_player_routes.cpp](/d:/code/model/examples/hello/hello_player_routes.cpp:1)`
   - 玩家侧接口最多。
6. `[hello_reward_routes.cpp](/d:/code/model/examples/hello/hello_reward_routes.cpp:1)`
   - 看运营发奖和邮件投递。
7. `[hello_admin_routes.cpp](/d:/code/model/examples/hello/hello_admin_routes.cpp:1)` 和 `[hello_admin_more_routes.cpp](/d:/code/model/examples/hello/hello_admin_more_routes.cpp:1)`
   - 最后看后台管理面。
8. `[hello_admin_task_routes.cpp](/d:/code/model/examples/hello/hello_admin_task_routes.cpp:1)`
   - 再看异步任务部分。

## 当前结构的好处

- `main.cpp` 已经不再臃肿，职责明确。
- 路由按领域拆开后，更容易按模块 review。
- 公共辅助函数集中后，后续改鉴权或审计时改动面更小。
- 每个文件顶部都有中文职责说明和调用流程，适合边看边学。

## 公告缓存调试

这次公告读取走的是：

1. GM 公告或定时公告先写 MySQL `announcement` 事实表。
2. `RedisCachedAnnouncementRepository::publish(...)` 再把同一条公告写进 Redis 最近窗口。
3. `/system/announcements` 先按 `channel_kind + scope_id` 读 Redis 最近窗口。
4. 如果 Redis 没热过或窗口为空，再回源 MySQL，并把最近窗口回填到 Redis。

Redis key 结构如下：

- 最近窗口列表：`announcement:recent:{channel_kind}:{scope_id}`
- 预热标记：`announcement:recent:warm:{channel_kind}:{scope_id}`

你现在可以直接访问：

- `GET /debug/announcement-cache?channel_kind=lobby&scope_id=global&limit=20`

这个接口会返回：

- 当前是不是 `mysql_redis` 模式
- 公告缓存是否启用
- 实际使用的 `recent_key` / `warm_key`
- `warm` 预热标记是否存在
- Redis 最近窗口当前有哪些公告对象

review 这个链路时，建议对照看这几个位置：

1. `[examples/hello/hello_routes.cpp](/d:/code/model/examples/hello/hello_routes.cpp:1)`
2. `[src/redis_repositories.cpp](/d:/code/model/src/redis_repositories.cpp:1)`
3. `[src/mysql_repositories.cpp](/d:/code/model/src/mysql_repositories.cpp:1)`
4. `[examples/hello/hello_admin_more_routes.cpp](/d:/code/model/examples/hello/hello_admin_more_routes.cpp:1)`
5. `[examples/hello/hello_admin_tasks.cpp](/d:/code/model/examples/hello/hello_admin_tasks.cpp:1)`

## 后续还可以继续优化的点

- 把 `main.cpp` 里的 `register_*_routes(...)` 注册顺序按“系统 / 认证 / 玩家 / 运营 / 后台 / 任务”再整理一次。
- 给每个模块再补一份“接口列表总览”文档。
- 如果后续继续增长，可以把后台模块再细拆成：
  - admin entry
  - admin user
  - admin audit
  - admin query
  - admin task
