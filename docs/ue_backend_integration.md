# UE 接入后端说明

## 文档目的

这份文档是写给 UE 客户端侧的。

目标不是解释后端源码，而是回答下面几个问题：

1. UE 该连哪个地址。
2. UE 该怎么登录、拿 token、刷新 token。
3. UE 该怎么请求玩家资料、邮件、好友、聊天。
4. UE 该怎么连 WebSocket 聊天网关。
5. UE 侧推荐按什么模块封装。

## 先说结论

UE **不要直接连 MySQL / Redis**，也**不要把这份后端代码复制进 UE 工程**。

UE 只做两件事：

1. 调 HTTP API
2. 连 WebSocket 聊天网关

也就是说，UE 访问的是“后端接口”，不是“数据库”。

## 当前服务器地址

当前这台阿里云服务器已经部署完成，入口如下：

- HTTP API / 管理后台入口：`http://120.26.173.47/`
- 管理后台页面：`http://120.26.173.47/admin`
- HTTP 直连调试入口：`http://120.26.173.47:8080`
- WebSocket 聊天网关：`ws://120.26.173.47:8090`

UE 客户端建议：

- 正常 HTTP 请求优先走 `http://120.26.173.47/`
- 聊天长连接走 `ws://120.26.173.47:8090`

## UE 侧推荐模块拆分

建议在 UE 里按下面方式封装，不要把所有请求散落到各个 UI Widget：

1. `UBackendConfig`
   - 保存 `BaseHttpUrl`
   - 保存 `ChatWsUrl`
2. `UBackendAuthSubsystem`
   - 注册、登录、刷新 token、登出
   - 维护当前 `AccessToken / RefreshToken / SessionId`
3. `UBackendPlayerSubsystem`
   - 玩家资料
   - 邮件
   - 好友
4. `UBackendChatSubsystem`
   - WebSocket 连接
   - join / say / history / recall
   - 未读、离线消息拉取
5. `UBackendAdminSubsystem`
   - 仅 GM 工具或内置后台调试界面使用

## 统一响应格式

HTTP 接口统一返回 JSON 包裹，结构固定：

```json
{
  "ok": true,
  "meta": {
    "protocol_version": "v1",
    "server_time": 1780670000
  },
  "data": {
  }
}
```

失败时：

```json
{
  "ok": false,
  "meta": {
    "protocol_version": "v1",
    "server_time": 1780670000
  },
  "error": {
    "code": "invalid_login",
    "message": "login_name or password is incorrect",
    "details": {
    }
  }
}
```

UE 侧建议统一做一层解析：

- 先判断 `ok`
- `ok=true` 时读 `data`
- `ok=false` 时读 `error.code / error.message / error.details`

## 鉴权规则

大多数玩家态接口都需要 Bearer Token。

请求头格式：

```http
Authorization: Bearer <token>
```

access token、refresh token 和 session_id 都由服务端生成。UE 必须把两个 token 当作不透明
字符串原样保存和回传，不要依赖其中的长度或内部格式，也不要自行截断、拆分或拼接。
服务端可以继续优化 token 的内部表示，但接口字段名和客户端使用方式保持不变。

登录成功后，后端会返回：

- `session.token`
- `session.refresh_token`
- `session.session_id`
- `session.expires_at`

UE 至少要本地保存：

1. `token`
2. `refresh_token`
3. `player_id`
   当前默认是 8 位数字字符串，例如 `10000001`，方便玩家手输和加好友。
4. `account_id`
5. `expires_at`

## 推荐接入顺序

建议 UE 按下面顺序接：

1. `/health`
2. `/auth/register` 或 `/auth/login`
3. `/players/me`
4. `/mail`
5. `/friends`
6. `ws://...:8090` 聊天
7. `/chat/unread` / `/chat/offline/pull`
8. `/shop/products` / `/shop/buy`

## 1. 健康检查

### `GET /health`

用途：

- 启动游戏时探活
- 登录前检查后端是否在线

示例：

```http
GET http://120.26.173.47/health
```

## 2. 账号注册与登录

### `POST /auth/register`

用途：

- 正式账号注册

请求体：

```json
{
  "login_name": "alice",
  "display_name": "Alice",
  "password": "Passw0rd!123",
  "email": "",
  "phone_number": "",
  "platform_channel": "ue",
  "register_source": "game_client"
}
```

成功返回：

- `data.account`
- `data.player`
- `data.session`

### `POST /auth/login`

用途：

- 正式密码登录

请求体：

```json
{
  "login_name": "alice",
  "password": "Passw0rd!123",
  "single_session": false
}
```

说明：

- `single_session=false` 是普通登录推荐值，允许多端会话，登录更快
- `single_session=true` 表示新登录时踢掉旧会话，会增加 Redis 清理开销

### `POST /auth/quick-login`

用途：

- 开发期快速登录
- 只用于本地开发或测试服联调
- 正式环境 `HELLO_SERVICE_ENV=production` 会直接禁用，返回 `403 quick_login_disabled_in_production`

请求体：

```json
{
  "login_name": "alice_dev",
  "display_name": "AliceDev"
}
```

### 登录成功后你会拿到什么

`/auth/login` 与 `/auth/quick-login` 只返回 `data.account` 和 `data.session`，不返回 `player`、货币或背包。进入大厅后用 `data.session.token` 请求 `GET /players/me`，再用返回的最新 `player_id/nickname/gold/gem/currencies/inventory` 初始化资料和资产 UI；资料、好友、公告等请求可以并行。

`data.session` 结构大致如下：

```json
{
  "token": "......",
  "session_id": "......",
  "refresh_token": "......",
  "account_id": "acc_xxx",
  "player_id": "10000001",
  "nickname": "Alice",
  "client_ip": "......",
  "user_agent": "......",
  "issued_at": 1780670000,
  "expires_at": 1780673600
}
```

## 3. 刷新 token / 登出 / 会话管理

### `POST /auth/refresh`

请求体：

```json
{
  "refresh_token": "......"
}
```

建议：

- UE 在 `token` 快过期时主动刷新
- 不要等所有请求都 401 再刷

### `POST /auth/logout`

请求头：

```http
Authorization: Bearer <token>
```

### `GET /auth/sessions`

用途：

- 拉当前玩家所有会话

### `POST /auth/sessions/revoke`

用途：

- 踢指定会话

请求体：

```json
{
  "session_id": "......"
}
```

## 4. 玩家资料

### `GET /players/me`

用途：

- 登录后读取当前玩家主资料

返回核心字段：

```json
{
  "player_id": "10000001",
  "account_id": "acc_xxx",
  "nickname": "Alice",
  "gold": 1000,
  "gem": 50,
  "currencies": [
    {
      "currency_id": "ticket",
      "amount": 3
    }
  ],
  "inventory": [
    {
      "instance_id": "inv_xxx",
      "item_id": "potion_small",
      "quantity": 5
    }
  ]
}
```

UE 侧建议：

- `gold / gem / currencies / inventory` 都本地建明确结构体
- 不要把 `inventory` 当字符串数组处理

### `POST /players/me/inventory/item`

用途：

- 设置当前登录玩家自己的某个道具数量
- `quantity = 0` 表示从背包移除该道具

请求体：

```json
{
  "item_id": "potion_small",
  "quantity": 10
}
```

成功后返回：

- `player`
- `inventory`

注意：这个接口只适合开发调试或服务端已验证的受控玩法流程。正式经济系统不要让客户端凭空决定奖励，发奖仍应优先走任务、掉落、邮件附件或后台 GM 流程。

## 5. 邮件系统

邮件分两类：

- 玩家邮件：UE 普通客户端使用，接口是 `/mail/send-player`，只能发文字邮件，不能带奖励。
- GM/后台邮件：只给管理员工具使用，接口是 `/mail/send`，可以带奖励附件，也可以批量发给多个玩家。

### `GET /mail`

用途：

- 拉当前玩家收件箱

说明：

- 默认不带已删除邮件
- 如果你要调试已删除邮件，可以请求 `GET /mail?include_deleted=true`
- 支持分页：`GET /mail?limit=20&offset=0`；`limit=0` 表示不限制条数，保留旧的全量读取行为
- 深分页建议用 keyset 游标：把上一页最后一条邮件的 `sent_at` 和 `mail_id` 分别传给 `last_sent_at` / `last_mail_id`，可避免 `offset` 深翻页扫描
- 分页响应为对象，不再直接返回数组：

```json
{
  "data": {
    "items": [],
    "total": 25,
    "limit": 20,
    "offset": 0,
    "has_more": true
  }
}
```

- `total` 是当前筛选下的可见邮件总数，`has_more` 表示 `offset` 之后是否还有下一页
- 邮件是按 `player_id` 持久化到后端数据库的，不要求收件人在线。
- UE 每次登录后、打开邮箱页时都应该重新调用 `GET /mail`，不要只依赖本地缓存。
- 如果同一个真实玩家每次登录拿到不同 `player_id`，就会看到“邮件不一样”。这种情况通常是 UE 每次 quick-login 传了不同 `login_name`。

离线收件规则：

- GM 或玩家给 `player_id=10000002` 发邮件时，后端直接写入 `mail_message` 表。
- `10000002` 离线不会影响投递。
- `10000002` 下次登录后，只要还是同一个 `player_id`，调用 `GET /mail` 就能看到邮件。
- 如果 UE 重新生成了新的 quick-login 账号，例如从 `Quick_随机数A` 变成 `Quick_随机数B`，后端会认为是新玩家，旧邮件不会出现在新玩家邮箱。

UE quick-login 必须稳定：

```json
{
  "login_name": "ue_device_固定设备ID或账号ID",
  "display_name": "玩家昵称"
}
```

不要这样做：

```json
{
  "login_name": "Quick_每次启动随机时间戳",
  "display_name": "玩家昵称"
}
```

否则账号、背包、邮件、好友都会像“每次登录都不一样”。

### `POST /mail/send-player`

用途：

- 当前登录玩家给另一个玩家写普通站内信
- 普通玩家邮件不允许携带奖励附件

请求体：

```json
{
  "to_player_id": "10000002",
  "title": "一起组队",
  "body": "晚上八点打副本吗？",
  "expires_in_seconds": 604800
}
```

说明：

- `expires_in_seconds` 可不传或传 `0`，表示不过期
- 收件人必须是已存在的 `player_id`

### `POST /mail/send`

用途：

- 管理员给玩家发系统邮件
- 支持奖励附件
- 支持单个玩家、多个玩家、后台页面“一键全部玩家”

权限：

- 需要管理员登录后的 `Bearer Token`
- 普通玩家客户端不要接这个接口

单人请求体：

```json
{
  "player_id": "32234345",
  "sender_name": "System",
  "title": "维护补偿",
  "body": "感谢你的等待，附件是本次维护补偿。",
  "expires_in_seconds": 604800,
  "rewards": [
    {
      "kind": "gold",
      "amount": 1000
    },
    {
      "kind": "item",
      "resource_id": "item_potion_small",
      "amount": 5
    }
  ]
}
```

批量请求体：

```json
{
  "player_ids": [
    "21706635",
    "32234345"
  ],
  "sender_name": "System",
  "title": "活动奖励",
  "body": "这是本次活动的结算奖励。",
  "expires_in_seconds": 604800,
  "rewards": [
    {
      "kind": "currency",
      "resource_id": "gem",
      "amount": 100
    }
  ]
}
```

奖励字段说明：

| 字段 | 类型 | 说明 |
|---|---|---|
| `kind` | string | `gold` / `currency` / `item` / `attachment_template` |
| `resource_id` | string | `currency`、`item`、`attachment_template` 必须填；`gold` 可不填 |
| `amount` | int64 | 发放数量，必须大于 0 |

常用奖励例子：

```json
[
  {
    "kind": "gold",
    "amount": 1000
  },
  {
    "kind": "currency",
    "resource_id": "gem",
    "amount": 50
  },
  {
    "kind": "item",
    "resource_id": "item_potion_small",
    "amount": 5
  }
]
```

全服邮件说明：

- 当前后台页面的一键全服邮件会先调用玩家列表接口拿到 `player_ids`
- 然后调用 `/mail/send` 的批量模式
- 玩家量很大时，后续应该改成后台任务表异步执行，避免一次 HTTP 请求太重

### `POST /mail/read`

请求体：

```json
{
  "mail_id": "mail_xxx"
}
```

### `POST /mail/claim`

用途：

- 领取邮件附件

请求体：

```json
{
  "mail_id": "mail_xxx"
}
```

成功后返回：

- `mail`
- `grant`
- `player`

也就是说，UE 领取附件后可以直接用返回的 `player` 刷新货币和背包，不一定再额外打一遍 `/players/me`。

### `POST /mail/delete`

请求体：

```json
{
  "mail_id": "mail_xxx"
}
```

删除说明：

- 当前后端会先尝试自动领取未领取附件，再把邮件标记删除。
- 如果历史坏邮件的附件无法正常领取，后端也会允许逻辑删除，避免邮箱卡住。
- 删除后普通 `GET /mail` 不再返回该邮件。

## 6. 抽卡系统

抽卡的核心数据应该放在后端：

- 卡池是否开放、每抽消耗、保底次数、奖品权重由后端控制。
- UE 只负责展示卡池、发起抽卡请求、播放动画、展示结果。
- 普通客户端不要自己计算概率，也不要自己改背包。

UE 抽卡页建议拆成 4 个小模块：

- `GachaApi`：封装 `/gacha/pools`、`/gacha/draw`、`/gacha/history`。
- `GachaPoolView`：显示卡池名称、封面、每抽消耗、保底信息。
- `GachaDrawView`：处理单抽/十连按钮、等待后端结果、播放动画。
- `GachaResultView`：展示本次 `results[]`，并刷新货币、背包或角色获得提示。

### `GET /gacha/pools`

用途：进入抽卡页时拉取当前开放卡池。

需要登录 token，建议和其他玩家接口一样带：

```http
Authorization: Bearer <access_token>
```

当前服务端实现也能不带 token 查询开放卡池，但 UE 正式接入时建议统一带 token，方便以后做“不同玩家可见不同卡池”。

当前可直接用于 UE 联调的卡池：

| pool_id | 用途 |
|---|---|
| `standard_2026` | 手写示例常驻池，奖品数量少，适合看清楚每个奖品配置。 |
| `all_items_2026` | 全物品测试大池，由数据库 `item_definition` 里所有启用物品自动生成，适合先把抽卡流程跑起来。 |

如果你现在只是想在 UE 里先跑通抽卡，优先用 `all_items_2026`。

返回核心字段：

- `items[].pool_id`
- `items[].name`
- `items[].description`
- `items[].banner_icon`
- `items[].cost_currency_id`
- `items[].cost_amount_per_draw`
- `items[].pity_5`
- `items[].pity_4`
- `items[].enabled`

示例返回：

```json
{
  "ok": true,
  "data": {
    "items": [
      {
        "pool_id": "standard_2026",
        "name": "常驻祈愿",
        "description": "常驻角色和道具卡池。",
        "banner_icon": "/Game/UI/Banners/T_Standard",
        "cost_currency_id": "gem",
        "cost_amount_per_draw": 160,
        "pity_5": 90,
        "pity_4": 10,
        "enabled": true
      }
    ]
  }
}
```

UE 显示建议：

- `name` 显示在卡池标题。
- `description` 显示在规则说明或详情弹窗。
- `banner_icon` 对应 UE 里的资源路径或图标 key。
- `cost_currency_id + cost_amount_per_draw` 显示单抽消耗。
- `pity_5 / pity_4` 显示保底说明，例如“90 抽必出 5 星，10 抽必出 4 星及以上”。

### `POST /gacha/draw`

用途：执行单抽或十连。

请求头：

```http
Authorization: Bearer <access_token>
```

请求体：

```json
{
  "pool_id": "standard_2026",
  "count": 10
}
```

`count` 只能是 `1` 或 `10`。

如果要抽全物品测试大池，请求体改成：

```json
{
  "pool_id": "all_items_2026",
  "count": 10
}
```

成功后返回：

- `pool`：本次抽的卡池。
- `results`：本次抽出的每个奖品。
- `state`：抽完后的最新保底计数。
- `grant`：发奖流水。
- `total_cost`：本次总消耗。

`results[]` 里每项会带：

- `history_id`
- `item_key`
- `rarity`
- `is_up`
- `reward`
- `drawn_at`

示例返回：

```json
{
  "ok": true,
  "data": {
    "pool": {
      "pool_id": "standard_2026",
      "name": "常驻祈愿",
      "cost_currency_id": "gem",
      "cost_amount_per_draw": 160
    },
    "results": [
      {
        "history_id": "gacha_10000001_standard_2026_...",
        "player_id": "10000001",
        "pool_id": "standard_2026",
        "item_key": "item_potion_small",
        "rarity": 3,
        "is_up": false,
        "reward": {
          "kind": "item",
          "resource_id": "item_potion_small",
          "amount": 1
        },
        "pity_5_before": 12,
        "pity_4_before": 4,
        "drawn_at": 1780000000
      }
    ],
    "state": {
      "player_id": "10000001",
      "pool_id": "standard_2026",
      "pity_5_count": 13,
      "pity_4_count": 5,
      "total_draws": 34
    },
    "grant": {
      "grant_id": "gacha_10000001_standard_2026_...",
      "source_type": "gacha",
      "source_id": "standard_2026",
      "rewards": [
        {
          "kind": "item",
          "resource_id": "item_potion_small",
          "amount": 1
        }
      ]
    },
    "total_cost": 160
  }
}
```

UE 收到结果后的处理顺序：

1. 先检查 `ok`。
2. 如果成功，播放抽卡结果动画。
3. 用 `results[]` 渲染卡片列表，`rarity` 决定特效强度。
4. 用 `state.pity_5_count`、`state.pity_4_count` 刷新保底显示。
5. 再调用 `GET /players/me` 刷新玩家货币和背包。

注意：当前 `/gacha/draw` 返回里有 `grant.rewards`，但不直接返回完整 `player`。所以 UE 抽完后建议额外请求一次 `/players/me`，拿最新 `gem/currencies/inventory`。

### `POST /gacha/history`

用途：查看玩家自己的抽卡历史。

请求体：

```json
{
  "pool_id": "standard_2026",
  "limit": 20,
  "offset": 0
}
```

`pool_id` 可以为空，表示查询全部卡池历史。

查询全物品测试大池历史时传：

```json
{
  "pool_id": "all_items_2026",
  "limit": 20,
  "offset": 0
}
```

成功返回：

```json
{
  "ok": true,
  "data": {
    "items": [
      {
        "history_id": "gacha_...",
        "pool_id": "standard_2026",
        "item_key": "item_potion_small",
        "rarity": 3,
        "is_up": false,
        "reward": {
          "kind": "item",
          "resource_id": "item_potion_small",
          "amount": 1
        },
        "drawn_at": 1780000000
      }
    ],
    "limit": 20,
    "offset": 0
  }
}
```

UE 用途：

- 抽卡记录页。
- 客服排查时让玩家截图。
- 玩家想看最近出了什么。

### 概率和保底现在怎么做

当前版本是“权重随机 + 硬保底”：

- 平时从整个卡池奖品里按 `weight` 权重随机。
- 如果 `pity_5_count + 1 >= pity_5`，本抽只从 `rarity >= 5` 的奖品里抽。
- 如果没触发 5 星保底，但 `pity_4_count + 1 >= pity_4`，本抽只从 `rarity >= 4` 的奖品里抽。
- 抽到 5 星后，5 星和 4 星保底计数都清零。
- 抽到 4 星后，4 星保底计数清零，5 星保底计数加一。
- 抽到 3 星后，两个保底计数都加一。

`all_items_2026` 的临时概率规则：

- 5 星总权重约 `60`，非保底约 `0.6%`。
- 4 星总权重约 `510`，非保底约 `5.1%`。
- 3 星总权重约 `9430`，非保底约 `94.3%`。
- 同一个星级内部会按数据库当前启用物品数量平均分权重。
- 这个池子是测试大池，不适合直接当正式运营限定池。

举例：

```text
5星角色A weight=1
4星角色B weight=10
3星材料C weight=89
```

非保底时总权重是 `100`：

- 5 星约 `1%`
- 4 星约 `10%`
- 3 星约 `89%`

触发 4 星保底时，只在 `rarity >= 4` 里抽：

- 5 星权重 `1`
- 4 星权重 `10`
- 5 星约 `1 / 11 = 9.09%`
- 4 星约 `10 / 11 = 90.91%`

这版还不是完整“原神规则”。暂时没有：

- 软保底概率曲线。
- 50/50 歪了以后下次必 UP。
- 武器池定轨。
- 新手池折扣。
- 卡池之间共享保底。

这些后续应该继续放在后端做，UE 不要自己算。

### 常见错误码

| error.code | UE 显示建议 | 原因 |
|---|---|---|
| `unauthorized` | 请重新登录 | token 缺失或失效 |
| `invalid_gacha_count` | 抽卡次数只能是 1 或 10 | count 传错 |
| `gacha_pool_not_found` | 卡池不存在 | pool_id 错误或后台未配置 |
| `gacha_pool_closed` | 卡池未开放 | 卡池停用、未到开始时间或已结束 |
| `gacha_pool_empty` | 卡池奖品未配置 | 后台还没配奖品 |
| `currency_not_enough` | 货币不足 | gem 或指定货币不够 |

### UE 侧建议

- 抽卡按钮点击后只调用 `/gacha/draw`，不要先在 UE 本地扣货币。
- 收到成功返回后，用 `results[]` 展示结果，再调用 `/players/me` 刷新资产。
- 抽卡动画可以先播放，再在结果页展示服务端返回的 `results`；但最终结果必须以后端返回为准。
- 如果返回 `currency_not_enough`，UE 直接提示货币不足。
- 抽卡请求发出后，UE 要禁用单抽/十连按钮，直到请求完成，避免玩家连点造成重复抽卡。

## 7. 商城系统

商城系统也必须以后端为准：

- 商品价格、限购、开放时间、奖励内容都在后端配置。
- UE 只负责展示商品、发送购买请求、展示购买结果。
- UE 不要本地扣 `gold/gem`，也不要本地直接改背包。

### `GET /shop/products`

用途：进入商城页时拉取当前开放商品。

请求：

```http
GET /shop/products
```

成功返回示例：

```json
{
  "ok": true,
  "data": {
    "items": [
      {
        "product_id": "daily_gem_pack_001",
        "category": "daily",
        "name": "每日宝石包",
        "description": "花费金币购买少量宝石",
        "icon": "icon_shop_gem_pack",
        "price_currency_id": "gold",
        "price_amount": 1000,
        "rewards": [
          {
            "kind": "currency",
            "resource_id": "gem",
            "amount": 60
          }
        ],
        "purchase_limit": 1,
        "enabled": true,
        "starts_at": null,
        "ends_at": null
      }
    ]
  }
}
```

返回里 UE 主要用这些字段：

| 字段 | UE 用途 |
|---|---|
| `product_id` | 购买时传给 `/shop/buy` |
| `category` | 商品分类页签 |
| `name` | 商品名 |
| `description` | 商品说明 |
| `icon` | 商品图标资源 key |
| `price_currency_id` | 消耗货币，比如 `gold`、`gem` |
| `price_amount` | 价格数量 |
| `rewards[]` | 购买后发放的奖励 |
| `purchase_limit` | 限购次数，`0` 表示不限购 |
| `starts_at / ends_at` | 商品开放时间，玩家接口一般只返回当前开放商品 |

奖励 `rewards[]` 的含义：

| kind | resource_id | amount | 说明 |
|---|---|---:|---|
| `gold` | 可空 | 数量 | 增加玩家基础金币 |
| `currency` | `gem` 或其他货币 ID | 数量 | 增加多货币余额 |
| `item` | 物品 ID | 数量 | 增加背包道具堆叠 |
| `attachment_template` | 奖励模板 ID | 数量 | 展开后台配置的奖励模板 |

### `POST /shop/buy`

用途：购买商品。

请求头：

```http
Authorization: Bearer <token>
```

请求体：

```json
{
  "product_id": "daily_gem_pack_001"
}
```

购买成功后，后端会完成：

1. 校验商品存在且开放。
2. 校验是否达到限购次数。
3. 扣除 `price_currency_id / price_amount`。
4. 调用奖励系统发放 `rewards[]`。
5. 写入 `shop_purchase` 购买流水。
6. 返回最新 `player`。

成功返回里 UE 主要用：

| 字段 | UE 用途 |
|---|---|
| `product` | 本次购买的商品配置快照 |
| `purchase` | 购买流水，可用于购买成功页 |
| `grant` | 奖励发放流水 |
| `player` | 扣费和发奖后的最新玩家资产 |

成功返回示例：

```json
{
  "ok": true,
  "data": {
    "product": {
      "product_id": "daily_gem_pack_001",
      "name": "每日宝石包",
      "price_currency_id": "gold",
      "price_amount": 1000
    },
    "purchase": {
      "purchase_id": "shop_10000001_daily_gem_pack_001_1780670000000000",
      "player_id": "10000001",
      "product_id": "daily_gem_pack_001",
      "price_currency_id": "gold",
      "price_amount": 1000,
      "purchased_at": 1780670000
    },
    "grant": {
      "grant_id": "shop_10000001_daily_gem_pack_001_1780670000000000",
      "source_type": "shop",
      "source_id": "daily_gem_pack_001"
    },
    "player": {
      "player_id": "10000001",
      "gold": 9000,
      "gem": 60,
      "currencies": [
      ],
      "inventory": [
      ]
    }
  }
}
```

常见失败：

| error.code | 含义 |
|---|---|
| `shop_product_not_found` | 商品不存在 |
| `shop_product_closed` | 商品未开放或已下架 |
| `shop_purchase_limit_reached` | 已达到限购次数 |
| `currency_not_enough` | 货币不足 |
| `shop_product_rewards_empty` | 商品没有配置奖励 |
| `player_not_found` | 当前 token 对应玩家不存在 |

### `POST /shop/history`

用途：查看当前玩家自己的商城购买历史。

请求：

```json
{
  "limit": 20,
  "offset": 0
}
```

成功返回示例：

```json
{
  "ok": true,
  "data": {
    "items": [
      {
        "purchase_id": "shop_10000001_daily_gem_pack_001_1780670000000000",
        "player_id": "10000001",
        "product_id": "daily_gem_pack_001",
        "price_currency_id": "gold",
        "price_amount": 1000,
        "rewards": [
          {
            "kind": "currency",
            "resource_id": "gem",
            "amount": 60
          }
        ],
        "purchased_at": 1780670000
      }
    ],
    "limit": 20,
    "offset": 0
  }
}
```

### 后台商品配置接口

管理员页面或 GM 工具可以调用：

| 接口 | 用途 |
|---|---|
| `/admin/shop/products/upsert` | 新增或更新商品 |
| `/admin/shop/products/delete` | 删除商品配置 |
| `/admin/shop/query` | 查询商品配置和指定玩家购买记录 |

如果服务器商品列表为空，可以先让服务器管理员执行：

```bash
cd /opt/ue-game-backend/model
sudo mysql game_backend < db/mysql/001_game_backend.sql
sudo mysql game_backend < db/mysql/007_seed_shop_default_products.sql
systemctl restart hello-service
```

新增商品请求示例：

```json
{
  "product_id": "daily_gem_pack_001",
  "category": "daily",
  "name": "每日宝石包",
  "description": "花费金币购买少量宝石",
  "icon": "icon_shop_gem_pack",
  "price_currency_id": "gold",
  "price_amount": 1000,
  "purchase_limit": 1,
  "enabled": true,
  "rewards": [
    {
      "kind": "currency",
      "resource_id": "gem",
      "amount": 60
    }
  ]
}
```

商城页推荐调用顺序：

1. 打开商城页：`GET /shop/products`
2. 玩家点购买：`POST /shop/buy`
3. 成功后直接用返回的 `player` 刷新货币和背包
4. 打开历史页：`POST /shop/history`

UE 侧处理建议：

- 购买按钮点击后立刻置灰，等 `/shop/buy` 返回后再恢复，避免玩家连点重复购买。
- 成功后优先使用返回的 `data.player` 刷新 UI，不需要马上再请求 `/players/me`。
- 如果失败是 `currency_not_enough`，直接弹“货币不足”。
- 如果失败是 `shop_purchase_limit_reached`，直接把该商品显示为“已达限购”。
- 商品显示顺序可以先按 `category` 分组，再按后端返回顺序展示。

## 8. 任务系统

任务系统已经由后端持久化。UE 不再把 `UBlasterQuestSubsystem` 的进程内状态当作永久事实，登录成功和恢复会话成功后都要重新拉取任务快照。

### 接口清单

| 接口 | 方法 | 用途 |
| --- | --- | --- |
| `/quests` | `GET` | 获取当前启用的任务定义 |
| `/players/me/quests` | `GET` | 获取当前玩家任务、Objective 进度和 StoryFlags |
| `/quests/accept` | `POST` | 接受任务 |
| `/quests/events` | `POST` | 上报受限任务事件并推进匹配目标 |
| `/quests/progress` | `POST` | `/quests/events` 的兼容别名 |
| `/quests/claim` | `POST` | 领取手动或 NPC 提交任务奖励 |
| `/admin/quests/upsert` | `POST` | 管理员新增或修改任务定义 |

以上玩家接口都需要：

```http
Authorization: Bearer <access_token>
```

### 接受任务

```http
POST /quests/accept
Content-Type: application/json

{
  "quest_id": "quest_main_001",
  "request_id": "UE 生成的 UUID"
}
```

后端验证任务是否启用、前置任务是否已领取、RequiredFlags 是否满足。重复接受会返回已有任务状态，不会重复创建。

### 推进任务

```http
POST /quests/events
Content-Type: application/json

{
  "event_id": "UE 生成的 UUID",
  "event_type": "monster_killed",
  "target_id": "monster_slime",
  "amount": 1,
  "source_id": "match-or-scene-event-id"
}
```

重要规则：

- `event_id` 是幂等键，同一事件重复发送不会重复增加进度。
- UE 只提交发生了什么，不提交“任务已完成”或“应该发什么奖励”。
- 后端根据任务定义中的 `event_type/target_id/target_value` 决定推进哪些 Objective。
- 联机游戏优先由 UE 游戏服务器上报事件，不能完全信任普通客户端。
- `claim_mode=auto` 的任务完成后会在同一事务里自动发奖。

### 领取任务奖励

```http
POST /quests/claim
Content-Type: application/json

{
  "quest_id": "quest_main_001",
  "request_id": "UE 生成的 UUID",
  "npc_id": "npc_village_chief"
}
```

`npc_id` 仅 `claim_mode=npc` 时需要。成功响应中的：

- `data.quest`：领奖后的任务状态。
- `data.grant`：任务奖励流水。
- `data.player`：领奖后的最新货币和背包。
- `data.already_claimed`：是否为重复领奖请求。

UE 必须使用 `data.player` 刷新资产界面。相同任务的奖励流水 ID 固定为 `quest:{player_id}:{quest_id}`，重复请求不会重复加资产。

### 管理员配置任务

管理员需要 `admin.quest_config` 权限：

```http
POST /admin/quests/upsert
Authorization: Bearer <admin_access_token>
Content-Type: application/json

{
  "quest_id": "quest_main_001",
  "version": 1,
  "title": "击败史莱姆",
  "description": "击败 3 只史莱姆",
  "claim_mode": "manual",
  "turn_in_npc_id": "",
  "completion_flag": "main_001_done",
  "pre_quest_ids": [],
  "required_flags": [],
  "objectives": [
    {
      "objective_id": "kill_slime",
      "event_type": "monster_killed",
      "target_id": "monster_slime",
      "target_value": 3
    }
  ],
  "rewards": [
    {"kind": "currency", "resource_id": "gold", "amount": 100}
  ],
  "enabled": true
}
```

`claim_mode` 支持 `auto/manual/npc`。任务表会由 MySQL schema migration 自动创建，无需手工执行建表 SQL。

### UE 推荐结构

新增 `UBlasterBackendQuestSubsystem` 专门处理 HTTP 与后端快照：

1. 登录成功后调用 `/quests` 和 `/players/me/quests`。
2. 把快照导入 `UBlasterQuestSubsystem`，原有 Subsystem 继续负责 UI 查询和本地预测。
3. 击杀、采集、对话等事件调用 `/quests/events`。
4. 服务端响应回来后用权威进度覆盖本地预测。
5. 领取成功后同时刷新任务 UI、货币和背包。

常见错误码：`quest_not_found`、`quest_condition_not_met`、`quest_not_accepted`、`quest_not_completed`、`quest_npc_mismatch`、`quest_event_rejected`。

## 9. 拾取系统

拾取也应该以后端为准：

- UE 不要调用背包设置接口给自己加物品。
- UE 只把服务端生成的 `drop_id` 发给后端。
- 后端在事务中锁定掉落，校验归属、过期和拾取状态。
- 校验通过后，奖励流水、玩家资产和拾取标记在同一个 MySQL 事务内提交。

### `POST /loot/pickup`

用途：玩家拾取一个掉落。

请求头：

```http
Authorization: Bearer <token>
```

请求体：

```json
{
  "drop_id": "drop_10000001_default_1780670000000000"
}
```

成功返回里 UE 主要用：

| 字段 | UE 用途 |
|---|---|
| `drop` | 已拾取后的掉落记录 |
| `grant` | 本次拾取奖励流水 |
| `player` | 发奖后的最新玩家资产 |

成功返回示例：

```json
{
  "ok": true,
  "data": {
    "drop": {
      "drop_id": "drop_10000001_default_1780670000000000",
      "owner_player_id": "10000001",
      "scene_id": "default",
      "picked": true,
      "picked_by_player_id": "10000001",
      "rewards": [
        {
          "kind": "item",
          "resource_id": "item_potion_small",
          "amount": 1
        }
      ]
    },
    "grant": {
      "source_type": "loot",
      "source_id": "drop_10000001_default_1780670000000000"
    },
    "player": {
      "player_id": "10000001",
      "gold": 7000,
      "gem": 760,
      "inventory": [
      ]
    }
  }
}
```

常见失败：

| error.code | 含义 |
|---|---|
| `loot_not_found` | 掉落不存在，可能是 drop_id 错误 |
| `loot_already_picked` | 掉落已经被拾取，UE 应隐藏该掉落 |
| `loot_not_owner` | 掉落不属于当前玩家 |
| `loot_expired` | 掉落已过期 |
| `loot_rewards_empty` | 掉落没有配置奖励 |

### 后台/战斗服生成掉落

当前先提供后台联调用接口：

```http
POST /admin/loot/spawn
Authorization: Bearer <admin token>
```

请求体：

```json
{
  "player_id": "10000001",
  "scene_id": "default",
  "ttl_seconds": 3600,
  "rewards": [
    {
      "kind": "item",
      "resource_id": "item_potion_small",
      "amount": 1
    }
  ]
}
```

后台查询掉落：

```http
POST /admin/loot/query
Authorization: Bearer <admin token>
```

```json
{
  "player_id": "10000001",
  "include_picked": true,
  "limit": 20,
  "offset": 0
}
```

UE 侧处理建议：

- 怪物死亡或宝箱开启后，最好由战斗服/后端生成掉落并把 `drop_id` 发给 UE。
- UE 点击拾取时只调用 `/loot/pickup`。
- 拾取成功后，用返回的 `data.player` 刷新背包和货币。
- 如果返回 `loot_already_picked` 或 `loot_expired`，UE 直接移除场景里的掉落表现。
- 当前接口先不做距离校验；后续可以把 `scene_id、坐标、战斗实例 ID` 加进掉落表和请求体。

### 收集到未配置物品时的自动入库规则

现在后端做了一个兜底规则：

如果玩家通过奖励系统获得了一个 `kind=item` 的物品，但 `item_definition` 表里还没有这个 `resource_id`，后端会自动创建一条默认物品配置。

会触发这个规则的来源：

- `/loot/pickup` 拾取掉落
- 邮件附件领取
- 商城购买发货
- 抽卡获得道具
- GM 发奖
- 后续活动奖励，只要最终走 `RewardService`

例如掉落奖励是：

```json
{
  "kind": "item",
  "resource_id": "item_unknown_crystal",
  "amount": 3
}
```

如果 MySQL 的 `item_definition` 里还没有 `item_unknown_crystal`，后端会自动插入一条默认配置：

```json
{
  "item_id": "item_unknown_crystal",
  "name": "item_unknown_crystal",
  "description": "自动收集到的物品，请在后台物品配置页补充正式描述",
  "icon": "",
  "rarity": "3星",
  "max_stack": 9999,
  "item_type": "auto_collected",
  "enabled": true
}
```

这样可以避免一种常见问题：

```text
玩家背包里有 item_id
但后台物品配置表查不到这个 item_id
UE 不知道图标、名字、稀有度怎么显示
```

UE 侧建议：

- 背包显示时优先用物品配置表里的 `name/icon/rarity/description`。
- 如果看到 `item_type=auto_collected`，说明这是后端自动兜底生成的配置，运营后台后续应该补正式名字、图标和描述。
- UE 不要因为自动配置缺图标就丢弃这个物品，玩家已经真实拥有它。
- UE 可以先显示 `item_id` 或默认图标，等后台补完配置后再正常展示。

后端侧注意：

- 自动入库只负责“补一条默认物品配置”，不会自动生成美术图标。
- 如果 `item_definition` 已经存在，后端不会覆盖已有配置。
- 真正的玩家数量仍然写在 `player_profile.inventory_json`，物品配置表只描述“这个物品是什么”。
- 生产配置最好还是提前在后台物品配置页维护好，自动入库只是防止掉落或奖励配置遗漏导致背包数据半残。

## 10. 好友系统

### `POST /friends/request`

发好友申请：

```json
{
  "target_player_id": "10000002"
}
```

### `GET /friends/requests`

返回：

- `incoming`
- `outgoing`

### `POST /friends/respond`

处理好友申请：

```json
{
  "requester_player_id": "10000001",
  "accept": true
}
```

### `GET /friends`

这个接口已经不是纯好友关系表，而是聚合结果。

它会直接带：

- `online`
- `connection_count`
- `unread_count`
- `last_message`
- `friend_nickname`

所以 UE 好友列表页优先直接吃这个接口，不要自己拼多次请求。

### `GET /friends/presence`

用途：

- 单独调试在线态
- 如果你未来想做更轻量的定时 presence 拉取，也可以用它

## 11. 聊天 HTTP 接口

### `GET /chat/history/lobby`

用途：

- 拉大厅最近历史

### `GET /chat/unread`

用途：

- 拉当前玩家各频道未读

### `POST /chat/read`

用途：

- 某个频道标记已读

请求体示例：

```json
{
  "channel_kind": "friend_direct",
  "friend_player_id": "10000002"
}
```

或者：

```json
{
  "channel_kind": "lobby",
  "scope_id": "global"
}
```

### `POST /chat/block`

聊天黑名单：

```json
{
  "target_player_id": "10000002",
  "blocked": true
}
```

### `GET /chat/blocks`

拉聊天黑名单列表。

### `POST /chat/offline/pull`

用途：

- 拉离线消息

请求体：

```json
{
  "limit": 20
}
```

### `POST /chat/recall`

用途：

- 撤回消息

请求体：

```json
{
  "channel_kind": "friend_direct",
  "friend_player_id": "10000002",
  "message_id": "msg_xxx"
}
```

## 12. WebSocket 聊天网关

聊天长连接地址：

```text
ws://120.26.173.47:8090
```

### 接入顺序

1. 先走 HTTP 登录拿 `token`
2. 再建立 WebSocket 连接
3. 连接成功后先发 `auth`
4. 认证成功后再发 `join`
5. 加入频道后才能 `say`

### 连接后客户端发包

#### 1. 认证

```json
{
  "type": "auth",
  "token": "<login token>"
}
```

服务端返回：

```json
{
  "type": "auth_result",
  "ok": true,
  "player_id": "10000001",
  "nickname": "Alice"
}
```

#### 2. 加入大厅

```json
{
  "type": "join",
  "channel_kind": "lobby",
  "scope_id": "global"
}
```

#### 3. 加入房间

```json
{
  "type": "join",
  "channel_kind": "room",
  "scope_id": "room_1001"
}
```

#### 4. 加入好友私聊

```json
{
  "type": "join",
  "channel_kind": "friend_direct",
  "friend_player_id": "10000002"
}
```

#### 5. 发消息

```json
{
  "type": "say",
  "message_kind": "text",
  "text": "hello"
}
```

#### 6. 拉历史

```json
{
  "type": "history",
  "channel_kind": "friend_direct",
  "friend_player_id": "10000002",
  "limit": 20
}
```

#### 7. 撤回

```json
{
  "type": "recall",
  "message_id": "msg_xxx"
}
```

### 服务端主动推送

新消息到达时：

```json
{
  "type": "message",
  "message": {
    "message_id": "msg_xxx",
    "channel": {
      "channel_id": "friend:10000001:10000002",
      "kind": "friend_direct",
      "scope_id": "",
      "display_name": "好友私聊"
    },
    "kind": "text",
    "sender_player_id": "10000001",
    "sender_nickname": "Alice",
    "text": "hello",
    "recalled": false,
    "recalled_by_player_id": "",
    "recalled_at": null,
    "sent_at": 1780670000
  }
}
```

历史返回时：

```json
{
  "type": "history_result",
  "channel": {
  },
  "items": [
  ]
}
```

撤回返回时：

```json
{
  "type": "recall_result",
  "ok": true,
  "message": {
  }
}
```

### UE 侧 WebSocket 封装建议

推荐状态机：

1. `Disconnected`
2. `Connecting`
3. `ConnectedSocket`
4. `Authed`
5. `JoinedChannel`

推荐事件：

- `OnChatAuthed`
- `OnChatJoinSucceeded`
- `OnChatMessage`
- `OnChatHistory`
- `OnChatRecall`
- `OnChatDisconnected`
- `OnChatError`

### 一个重要限制

当前网关一条 WebSocket 连接**只维护一个活跃频道订阅**。

也就是说：

- 你 `join` 新频道时，会离开旧频道
- 如果 UE 想同时看多个频道，需要自己决定：
  - 是做“一个连接只服务当前聊天窗口”
  - 还是后续扩后端为“一连接多订阅”

当前建议先按“当前打开哪个聊天页，就 join 哪个频道”来接。

## 13. 管理后台相关

如果 UE 里要做 GM 面板或内置运营工具，可以接这些：

- `POST /admin/login`
- `GET /admin/me`
- `POST /admin/query/account`
- `POST /admin/query/player`
- `POST /admin/query/players`
- `POST /mail/send`
- `POST /admin/batch/mails`
- `POST /chat/mute`
- `POST /chat/gm/announce`
- `POST /auth/admin/account-status`

但如果只是游戏客户端，通常不需要接这部分。

### GM 邮件对接建议

GM 页面推荐流程：

1. `POST /admin/login` 获取管理员 token
2. `GET /admin/query/players?limit=50&offset=0` 展示玩家表
3. 输入 8 位 `player_id` 时，用 `player_id` 精确筛选
4. 勾选玩家后调用 `/mail/send`，传 `player_ids`
5. 一键全服时先分页拉玩家 ID，再分批调用 `/mail/send` 或接 `/admin/batch/mails`

不要把 GM token 写进普通 UE 客户端包里。GM 工具应该只给内部人员使用，至少要单独打包、单独权限、单独登录。

## 14. UE 侧最小落地清单

建议你先做下面这 8 个接口，就已经能让 UE 跑通基础账号与社交链路：

1. `POST /auth/register`
2. `POST /auth/login`
3. `POST /auth/refresh`
4. `GET /players/me`
5. `GET /mail`
6. `GET /friends`
7. `GET /chat/unread`
8. `POST /chat/offline/pull`

然后再接 WebSocket：

1. `auth`
2. `join`
3. `say`
4. `history`

## 15. 常见错误

### 1. UE 直接连数据库

这是错误用法。

原因：

- 不安全
- 协议不稳定
- 数据库结构不等于客户端协议
- 无法复用登录、会话、风控、审计、权限这些逻辑

### 2. 忘了带 Bearer Token

表现：

- 返回 `unauthorized`

### 3. WebSocket 连上了，但先发 `say`

表现：

- 返回：

```json
{
  "type": "error",
  "message": "join a channel first"
}
```

### 4. WebSocket 连上了，但没先 `auth`

表现：

- `join` 时报：

```json
{
  "type": "error",
  "message": "authenticate first"
}
```

### 5. 聊天网关和 HTTP token 不一致

聊天网关的 `auth.token` 必须用 HTTP 登录拿到的那个 `session.token`。

## 16. 推荐联调方式

联调时建议按这个顺序：

1. 浏览器先确认 `http://120.26.173.47/admin` 可访问
2. Postman / UE 先打 `/auth/login`
3. 用返回的 `token` 打 `/players/me`
4. UE 再连 `ws://120.26.173.47:8090`
5. 先发 `auth`
6. 再发 `join`
7. 最后发 `say`

## 17. 对 UE 团队的建议

1. HTTP 返回包统一解一层，不要每个界面自己解析裸 JSON。
2. token 刷新逻辑放到统一子系统，不要散在各个 Widget。
3. 聊天长连接做状态机，不要只写“连上就发”。
4. `player_id` 和 `account_id` 都要保存，不要只保存昵称。
5. 所有时间字段当前都按 Unix 秒时间戳处理。
6. 正式环境不要依赖 `quick-login` 和开发管理员账号。
7. UE 正式包必须走 `/auth/register`、`/auth/login`、`/auth/refresh`，不要调用 `/auth/quick-login`。

### 运行环境对 UE 的影响

- 开发环境：`HELLO_SERVICE_ENV=development`，可以用 `/auth/quick-login` 快速进游戏。
- 测试环境：`HELLO_SERVICE_ENV=staging`，建议尽量走正式登录，只在必要时保留调试入口。
- 正式环境：`HELLO_SERVICE_ENV=production`，`/auth/quick-login` 禁用，开发管理员自动创建禁用。
- UE 可以通过 `GET /health` 或 `GET /config` 读取 `runtime_environment`，用于登录页或调试面板提示当前连接环境。

### 后端自动建表和后续新增系统

后端现在支持启动时自动执行 MySQL schema migration。

这件事主要是给后端和服务器部署用的，UE 客户端不需要直接操作数据库。

#### 1. 后端启动时会自动做什么

当服务器使用 MySQL + Redis 模式时：

```env
HELLO_SERVICE_STORAGE_MODE=mysql_redis
```

后端启动会自动执行：

```text
db/mysql/001_game_backend.sql
```

它会自动补齐缺失的数据库结构，例如：

- 账号表
- 玩家表
- 背包和道具配置表
- 邮件表
- 奖励流水表
- 好友表
- 聊天记录表
- 抽卡表
- 商城表
- 拾取掉落表
- 后续新增的语音房间表、活动表、配置表

UE 只需要正常调用 HTTP / WebSocket 接口，不需要关心 MySQL 是否已经建表。

#### 2. 相关服务器配置

服务器配置在：

```text
scripts/linux/deploy.env
```

关键配置：

```env
HELLO_SERVICE_MYSQL_AUTO_MIGRATE=1
HELLO_SERVICE_MYSQL_SCHEMA_FILE=db/mysql/001_game_backend.sql
```

含义：

- `HELLO_SERVICE_MYSQL_AUTO_MIGRATE=1` 表示后端启动时自动建表和补字段。
- `HELLO_SERVICE_MYSQL_SCHEMA_FILE` 表示要执行哪份 SQL。
- 如果设置成 `HELLO_SERVICE_MYSQL_AUTO_MIGRATE=0`，后端不会自动改数据库，需要服务器管理员手动执行 SQL。

#### 3. 为什么多个服务同时启动也没问题

服务器上一般会同时跑：

```text
hello_service
hello_chat_gateway
hello_task_worker
```

这三个进程都会用到 MySQL。

为了避免它们同时建表或改表，后端 migration 会先拿 MySQL 命名锁：

```sql
GET_LOCK('mf_game_backend_schema_migration', 30)
```

效果是：

- 同一时间只有一个进程执行建表。
- 其他进程会等待。
- SQL 执行完后释放锁。
- 避免同时 `ALTER TABLE` 导致冲突。

#### 4. 以后新增一个系统怎么做

例如以后要加“语音房间”系统。

后端侧流程：

1. 在 `db/mysql/001_game_backend.sql` 里追加语音房间表。
2. 写对应的 C++ Model。
3. 写 Repository 访问数据库。
4. 写 Service 处理业务规则。
5. 写 HTTP 或 WebSocket 接口。
6. 重启服务器。
7. 后端启动时自动补齐新表。

UE 侧流程：

1. 看后端文档里新增的接口。
2. 按接口传 JSON。
3. 处理返回结果。
4. 不需要直接连 MySQL。

#### 5. 新表 SQL 应该怎么写

新增表必须写成可重复执行的形式。

正确写法：

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

这种写法可以重复执行。

如果表已经存在，不会报错，也不会删数据。

#### 6. 老表补字段应该怎么写

不要直接写：

```sql
ALTER TABLE player_profile ADD COLUMN level BIGINT NOT NULL DEFAULT 1;
```

原因是第二次启动再执行时，MySQL 会提示字段已经存在。

应该写成条件补字段：

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

这样重复启动也不会报错。

#### 7. UE 需要注意什么

UE 不应该做这些事：

- 不要直接连 MySQL。
- 不要在客户端写 SQL。
- 不要假设数据库表结构就是客户端协议。
- 不要把数据库账号密码放进 UE 包里。

UE 应该做这些事：

- 登录走 `/auth/login`。
- 游戏数据走 `/players/me`、`/mail`、`/shop/products`、`/loot/pickup` 等后端接口。
- 新功能等后端暴露接口后再接入。
- 如果接口返回表不存在或服务 500，把日志发给后端查 migration。

#### 8. 服务器怎么验证 migration 是否执行成功

看服务日志：

```bash
journalctl -u hello-service -n 100 --no-pager | grep "mysql schema migration"
```

正常会看到类似：

```text
mysql schema migration starting file=db/mysql/001_game_backend.sql
mysql schema migration completed file=db/mysql/001_game_backend.sql
```

查看表是否存在：

```bash
mysql -ugame_backend -p game_backend -e "SHOW TABLES;"
```

查看某个表结构：

```bash
mysql -ugame_backend -p game_backend -e "DESC loot_drop;"
```

#### 9. 正式服注意事项

早期项目可以保持：

```env
HELLO_SERVICE_MYSQL_AUTO_MIGRATE=1
```

这样迭代快，新增表后重启即可自动补齐。

但以后玩家量变大时要更谨慎：

- 大表加字段可能锁表。
- 大表加索引可能很慢。
- 删除字段、改字段类型属于危险操作，不建议自动执行。
- 生产服最好先在测试服跑一遍 migration。

如果要完全手动控库，可以在正式服设置：

```env
HELLO_SERVICE_MYSQL_AUTO_MIGRATE=0
```

然后由服务器管理员手动执行：

```bash
cd /opt/ue-game-backend/model
mysql -ugame_backend -p game_backend < db/mysql/001_game_backend.sql
```

## 18. 语音聊天第一阶段

语音音频不经过本后端传输。后端只校验游戏登录态和房间成员资格，再签发短期 LiveKit JWT；UE 使用返回的地址和 Token 连接 LiveKit WebRTC。

### 玩家接口

```http
POST /api/voice/token
Authorization: Bearer <access_token>
Content-Type: application/json

{
  "scopeType": "session",
  "scopeId": "session_123"
}
```

成功返回 `server_url / room_id / participant_id / display_name / access_token / can_publish / expires_at`。`participant_id` 和昵称来自后端 Session，UE 不能指定。

```http
POST /api/voice/rooms/members
Authorization: Bearer <access_token>
Content-Type: application/json

{
  "scope_type": "session",
  "scope_id": "session_123"
}
```

只有已登记且未封禁的房间成员可以获取 Token 或查询成员。

### 管理员联调接口

- `POST /admin/voice/members/upsert`：登记 `session/party/guild` 房间成员。
- `POST /admin/voice/moderation`：执行 `mute/unmute/ban/unban`。
- 管理员权限：`admin.voice_manage`。

当前后端还没有权威多人 Session 成员服务，因此第一阶段通过管理员接口登记。正式接入后，应在创建/加入/离开 Session 的后端流程中自动同步，普通 UE 客户端不能自己登记成员。

禁言成员的新 Token 中 `canPublish=false`，仍可订阅其他玩家；封禁成员获取 Token 返回 `voice_banned`。当前在线连接的立即静音/踢出需要下一阶段接 LiveKit RoomService API。

服务器需要配置：

```env
HELLO_SERVICE_LIVEKIT_URL=wss://voice.example.com
HELLO_SERVICE_LIVEKIT_API_KEY=replace_me
HELLO_SERVICE_LIVEKIT_API_SECRET=replace_me
HELLO_SERVICE_VOICE_TOKEN_TTL_SECONDS=900
HELLO_SERVICE_VOICE_ROOM_PREFIX=blaster
```

`HELLO_SERVICE_LIVEKIT_API_SECRET` 绝不能进入 UE 配置、日志或打包文件。

### Windows 本地联调

本机已经安装 LiveKit Server `v1.13.4`：

```text
D:\tools\livekit\server\livekit-server.exe
```

启动和停止命令：

```powershell
& D:\code\model\scripts\windows\start-local-livekit.ps1
& D:\code\model\scripts\windows\stop-local-livekit.ps1
```

VS Code 的 `F5 hello_service (Debug)` 会自动先启动 LiveKit，再构建并启动后端。后端本地开发环境使用 `ws://127.0.0.1:7880`；API Key 和 Secret 由 `.vscode/launch.json` 注入后端进程，UE 只接收 `/api/voice/token` 返回的短期 Token，不能读取 API Secret。

验证服务：

```powershell
Invoke-WebRequest http://127.0.0.1:7880
Get-NetTCPConnection -LocalPort 7880,7881 -State Listen
```

根地址应返回 `200 OK`。本地 LiveKit 只监听环回地址，不能供其他电脑或公网客户端访问。

### 权威多人房间接口

多人房间和语音成员已经联动，普通 UE 客户端不再调用管理员语音成员登记接口：

```http
POST /api/multiplayer/rooms/create
POST /api/multiplayer/rooms/join
POST /api/multiplayer/rooms/get
POST /api/multiplayer/rooms/leave
POST /api/multiplayer/rooms/close
Authorization: Bearer <game_access_token>
Content-Type: application/json
```

创建请求：

```json
{
  "room_name": "测试房间",
  "max_players": 4
}
```

其他接口请求：

```json
{
  "room_id": "room_a83f21"
}
```

创建和加入成功会自动写 `voice_room_member`；退出和关闭会自动删除语音成员。`/api/voice/token` 还会再次检查 `multiplayer_room` 状态和 `multiplayer_room_member`，防止残留语音记录继续获得 Token。

房主不能直接退出，必须关闭房间；MySQL 加入流程使用事务和行锁保证不会超过 `max_players`。

## 19. 相关文档

- 路由拆分导读：[examples/hello/ROUTES.md](/d:/code/model/examples/hello/ROUTES.md:1)
- Linux 部署文档：[docs/linux_deploy.md](/d:/code/model/docs/linux_deploy.md:1)
- MySQL 自动迁移：[docs/mysql_schema_migration.md](/d:/code/model/docs/mysql_schema_migration.md:1)
- MySQL Schema：[db/mysql/001_game_backend.sql](/d:/code/model/db/mysql/001_game_backend.sql:1)
