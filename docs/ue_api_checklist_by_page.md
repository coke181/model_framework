# UE 接口清单表

## 文档目的

这份文档是给 UE 前端程序直接对接用的。

按页面拆分：

1. 登录页
2. 大厅页
3. 邮箱页
4. 抽卡页
5. 商城页
6. 任务页
7. 拾取页
8. 好友页
9. 聊天页

每个接口只回答 5 件事：

- 路径
- 方法
- 是否需要登录 token
- 什么时候调用
- 主要用途

## 基础约定

- HTTP 基础地址：`http://120.26.173.47/`
- 聊天网关地址：`ws://120.26.173.47:8090`
- 需要登录态的 HTTP 接口统一带：

```http
Authorization: Bearer <token>
```

- HTTP 统一响应格式：
  - 成功：`{ ok: true, meta: ..., data: ... }`
  - 失败：`{ ok: false, meta: ..., error: ... }`

## 1. 登录页

| 接口 | 方法 | 需要 Token | 什么时候调 | 用途 |
|---|---|---:|---|---|
| `/health` | `GET` | 否 | 打开游戏、进入登录页时 | 检查后端是否在线 |
| `/auth/captcha` | `POST` | 否 | 需要验证码时 | 获取验证码 challenge |
| `/auth/register` | `POST` | 否 | 玩家注册时 | 创建正式账号并直接返回 session |
| `/auth/login` | `POST` | 否 | 玩家登录时 | 只返回 account 和 session；默认多端会话，进入大厅后再请求玩家资料和资产 |
| `/auth/quick-login` | `POST` | 否 | 仅 development/staging 调试时 | 快速创建/登录测试账号，production 会返回 403 |
| `/auth/refresh` | `POST` | 否 | token 快过期时 | 用 refresh_token 刷新 session |
| `/auth/forgot-password` | `POST` | 否 | 找回密码第一步 | 获取 reset_token |
| `/auth/reset-password` | `POST` | 否 | 找回密码第二步 | 用 reset_token 重置密码 |

### 登录页最小调用顺序

1. `GET /health`
2. `POST /auth/login`
3. 保存 `data.session.token`
4. 保存 `data.session.refresh_token`
5. 保存 `data.session.account_id`，并进入大厅
6. 进入大厅后调 `GET /players/me`，异步加载玩家资料、货币和背包

`GET /health` 会返回 `runtime_environment`，例如 `development`、`staging`、`production`。UE 可以用它在调试面板里显示当前连到的是本地服、测试服还是正式服。

### 登录成功后必须保存

- `session.token`
- `session.refresh_token`
- `session.session_id`
- `session.account_id`
- `session.player_id`
- `session.expires_at`

### quick-login 稳定性要求

- `HELLO_SERVICE_ENV=production` 时后端会禁用 `/auth/quick-login`，正式 UE 包不要调用它。
- `login_name` 必须固定，例如设备 ID、平台账号 ID 或本地账号 ID。
- 不要每次启动都用随机时间戳当 `login_name`。
- 如果每次登录拿到不同 `player_id`，邮件、好友、背包都会像“不持久”。
- 邮件是按 `player_id` 保存的，不是按昵称保存的。

## 2. 大厅页

| 接口 | 方法 | 需要 Token | 什么时候调 | 用途 |
|---|---|---:|---|---|
| `/players/me` | `GET` | 是 | 进入大厅时 | 拉当前玩家资料、货币、背包 |
| `/players/me/inventory/item` | `POST` | 是 | 开发调试或受控玩法结算时 | 设置当前玩家某个道具数量 |
| `/friends` | `GET` | 是 | 进入大厅时 | 拉好友列表，已聚合在线态和未读数 |
| `/chat/unread` | `GET` | 是 | 进入大厅时、回到前台时 | 拉频道未读计数 |
| `/mail` | `GET` | 是 | 进入大厅时 | 拉收件箱摘要 |
| `/auth/sessions` | `GET` | 是 | 设置页/安全页 | 拉当前账号会话列表 |
| `/auth/logout` | `POST` | 是 | 点退出登录时 | 撤销当前 token |
| `/auth/sessions/revoke` | `POST` | 是 | 账号安全页 | 踢指定 session |
| `/chat/history/lobby` | `GET` | 否 | 大厅聊天面板初次打开 | 拉大厅最近消息 |
| `/chat/channels/examples` | `GET` | 否 | 开发联调时 | 看频道模型示例 |

### 大厅页推荐顺序

1. `GET /players/me`
2. `GET /friends`
3. `GET /chat/unread`
4. `GET /mail`
5. 建立 WebSocket 连接

## 3. 邮箱页

### 玩家邮箱接口

| 接口 | 方法 | 需要 Token | 什么时候调 | 用途 |
|---|---|---:|---|---|
| `/mail` | `GET` | 是 | 打开邮箱页时 | 拉收件箱 |
| `/mail/send-player` | `POST` | 是 | 玩家给其他玩家写邮件时 | 发送普通玩家邮件，不带奖励附件 |
| `/mail/read` | `POST` | 是 | 打开某封邮件时 | 标记已读 |
| `/mail/claim` | `POST` | 是 | 点击领取附件时 | 领取附件，返回邮件+奖励流水+最新玩家资料 |
| `/mail/delete` | `POST` | 是 | 删除邮件时 | 标记删除 |

### GM 邮件接口

| 接口 | 方法 | 需要管理员 Token | 什么时候调 | 用途 |
|---|---|---:|---|---|
| `/admin/login` | `POST` | 否 | 打开 GM 页面时 | 管理员登录 |
| `/admin/query/players` | `GET/POST` | 是 | 打开 GM 玩家表时 | 查询全部玩家或按 8 位 player_id 筛选 |
| `/mail/send` | `POST` | 是 | GM 发系统邮件时 | 给单人/多人发邮件，可带奖励 |
| `/admin/batch/mails` | `POST` | 是 | 大批量或定时邮件时 | 创建批量邮件任务 |

GM 邮件奖励 JSON 示例：

```json
[
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
```

### 邮箱页推荐处理

- 打开邮箱页：`GET /mail`
- 每次重新登录后：重新 `GET /mail`
- 写玩家邮件：`POST /mail/send-player`
- 点开邮件：`POST /mail/read`
- 领取附件：`POST /mail/claim`
- 删除邮件：`POST /mail/delete`

`/mail/claim` 成功后会直接返回最新 `player`，所以：

- 可以直接刷新背包和货币 UI
- 不一定要再额外请求一次 `/players/me`

### 离线收件说明

- 后端邮件会持久写入数据库，收件人不在线也能收到。
- 对方下次用同一个 `player_id` 登录后，调用 `GET /mail` 就能看到。
- 如果对方 quick-login 每次生成不同 `login_name`，后端会认为是不同玩家，自然收不到旧玩家的邮件。

## 4. 抽卡页

抽卡页只调用后端接口，不在 UE 本地计算概率、不在 UE 本地扣货币、不在 UE 本地改背包。

| 接口 | 方法 | 需要 Token | 什么时候调 | 用途 |
|---|---|---:|---|---|
| `/gacha/pools` | `GET` | 建议带 | 打开抽卡页时 | 拉当前开放卡池列表 |
| `/gacha/draw` | `POST` | 是 | 点击单抽/十连时 | 执行抽卡，后端扣货币、抽奖、发奖、记录历史 |
| `/gacha/history` | `POST` | 是 | 打开抽卡历史页时 | 拉当前玩家抽卡记录 |
| `/players/me` | `GET` | 是 | 抽卡成功后 | 刷新最新货币和背包 |

### 抽卡页推荐调用顺序

1. 进入抽卡页：`GET /gacha/pools`
2. 玩家选择某个 `pool_id`
3. 点击单抽：`POST /gacha/draw`，`count=1`
4. 点击十连：`POST /gacha/draw`，`count=10`
5. 抽卡成功后：用 `results[]` 播放和展示结果
6. 抽卡成功后：`GET /players/me` 刷新货币和背包
7. 打开历史页：`POST /gacha/history`

当前可直接用于 UE 联调的卡池：

| pool_id | 说明 |
|---|---|
| `standard_2026` | 手写示例常驻池，奖品少，适合检查配置细节 |
| `all_items_2026` | 全物品测试大池，自动读取数据库里所有启用物品，适合先跑通抽卡 |

### `GET /gacha/pools`

请求：

```http
GET /gacha/pools
Authorization: Bearer <token>
```

返回里 UE 主要用这些字段：

| 字段 | UE 用途 |
|---|---|
| `pool_id` | 后续 `/gacha/draw` 传这个 |
| `name` | 卡池标题 |
| `description` | 卡池说明/规则弹窗 |
| `banner_icon` | 卡池封面图资源 key |
| `cost_currency_id` | 消耗货币，比如 `gem` |
| `cost_amount_per_draw` | 每抽消耗数量 |
| `pity_5` | 5 星硬保底次数 |
| `pity_4` | 4 星硬保底次数 |

### `POST /gacha/draw`

单抽请求：

```json
{
  "pool_id": "standard_2026",
  "count": 1
}
```

十连请求：

```json
{
  "pool_id": "standard_2026",
  "count": 10
}
```

如果要先测试全物品大池，把 `pool_id` 换成：

```json
{
  "pool_id": "all_items_2026",
  "count": 10
}
```

返回里 UE 主要用这些字段：

| 字段 | UE 用途 |
|---|---|
| `results[]` | 本次抽卡结果列表 |
| `results[].item_key` | 显示抽到的角色/道具 key |
| `results[].rarity` | 决定 3/4/5 星展示特效 |
| `results[].is_up` | 显示是否 UP |
| `results[].reward.kind` | 奖励类型，`item/currency/gold` |
| `results[].reward.resource_id` | 奖励资源 ID |
| `results[].reward.amount` | 奖励数量 |
| `state.pity_5_count` | 当前 5 星保底计数 |
| `state.pity_4_count` | 当前 4 星保底计数 |
| `total_cost` | 本次总消耗 |

### `POST /gacha/history`

请求：

```json
{
  "pool_id": "standard_2026",
  "limit": 20,
  "offset": 0
}
```

如果 `pool_id` 传空字符串，表示查全部卡池历史。

查全物品测试大池历史时传 `all_items_2026`。

### 抽卡页错误处理

| error.code | UE 提示 |
|---|---|
| `unauthorized` | 登录已失效，请重新登录 |
| `invalid_gacha_count` | 抽卡次数只能是单抽或十连 |
| `gacha_pool_not_found` | 卡池不存在 |
| `gacha_pool_closed` | 卡池暂未开放 |
| `gacha_pool_empty` | 卡池暂未配置奖品 |
| `currency_not_enough` | 货币不足 |

### 抽卡页 UE 注意点

- 玩家点击抽卡后，立即禁用单抽/十连按钮，等接口返回后再恢复。
- 不要在 UE 本地先扣 `gem`，扣费以 `/gacha/draw` 成功为准。
- 不要在 UE 本地算概率，概率和保底以后会继续由后端控制。
- 抽卡动画可以先播放，但最终展示结果必须使用后端返回的 `results[]`。
- 抽完后建议再调一次 `/players/me`，刷新 `gem/currencies/inventory`。
- `all_items_2026` 是快速测试池，正式运营池后续仍建议在后台单独配置 UP、限定类型和每个奖品权重。

## 5. 商城页

商城页只调用后端接口，不在 UE 本地扣货币、不在 UE 本地改背包。

| 接口 | 方法 | 需要 Token | 什么时候调 | 用途 |
|---|---|---:|---|---|
| `/shop/products` | `GET` | 否 | 打开商城页时 | 拉当前开放商品列表 |
| `/shop/buy` | `POST` | 是 | 点击购买按钮时 | 后端校验商品、限购、扣货币、发奖励、写购买流水 |
| `/shop/history` | `POST` | 是 | 打开购买记录页时 | 拉当前玩家购买历史 |
| `/players/me` | `GET` | 是 | 购买成功后 | 刷新最新货币和背包 |

### 商城页推荐调用顺序

1. 进入商城页：`GET /shop/products`
2. 玩家点击商品：拿到 `product_id`
3. 点击购买：`POST /shop/buy`
4. 购买成功后：优先用返回的 `data.player` 刷新货币和背包
5. 打开购买记录：`POST /shop/history`

### `GET /shop/products`

返回里 UE 主要用这些字段：

| 字段 | UE 用途 |
|---|---|
| `product_id` | 后续 `/shop/buy` 传这个 |
| `category` | 商品分类页签，比如 `daily`、`pack` |
| `name` | 商品名 |
| `description` | 商品描述 |
| `icon` | 商品图标资源 key |
| `price_currency_id` | 消耗货币，比如 `gold` 或 `gem` |
| `price_amount` | 价格数量 |
| `rewards[]` | 购买后发放的奖励 |
| `purchase_limit` | 限购次数，`0` 表示不限购 |

最小返回示例：

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
        "purchase_limit": 1
      }
    ]
  }
}
```

### `POST /shop/buy`

请求：

```json
{
  "product_id": "daily_gem_pack_001"
}
```

成功返回里 UE 主要用：

| 字段 | UE 用途 |
|---|---|
| `product` | 本次购买的商品配置快照 |
| `purchase` | 购买流水 |
| `grant` | 奖励发放流水 |
| `player` | 扣费和发奖后的最新玩家资料 |

最小成功返回示例：

```json
{
  "ok": true,
  "data": {
    "product": {
      "product_id": "daily_gem_pack_001",
      "price_currency_id": "gold",
      "price_amount": 1000
    },
    "purchase": {
      "purchase_id": "shop_10000001_daily_gem_pack_001_1780670000000000",
      "product_id": "daily_gem_pack_001",
      "price_currency_id": "gold",
      "price_amount": 1000
    },
    "grant": {
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

### `POST /shop/history`

请求：

```json
{
  "limit": 20,
  "offset": 0
}
```

返回：

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

### 商城页 UE 侧处理规则

- `/shop/products` 不需要 token，但如果 UE 已登录，带 token 也可以。
- `/shop/buy` 必须带 token，否则会返回未登录错误。
- 玩家点击购买后，按钮要临时置灰，等待接口返回，避免重复点击。
- 购买成功后，直接用 `data.player` 刷新金币、宝石、背包。
- 商品奖励展示只读 `rewards[]`，不要在 UE 本地根据商品 ID 写死奖励。
- 限购次数由后端最终判断，UE 可以显示限购文案，但不能只靠本地判断。
- 如果 `/shop/products` 返回空，说明服务器还没配置商品；先执行 `db/mysql/007_seed_shop_default_products.sql` 或在后台创建商品。

## 6. 任务页

任务页在登录成功或恢复会话后先拉定义和玩家快照，战斗/采集过程中只上报事件，不直接提交完成状态或奖励。

| 接口 | 方法 | 鉴权 | 调用时机 | 用途 |
| --- | --- | --- | --- | --- |
| `/quests` | `GET` | 是 | 登录后、任务配置版本变化时 | 获取服务端任务定义 |
| `/players/me/quests` | `GET` | 是 | 登录后、打开任务页时 | 获取任务状态、目标进度和 StoryFlags |
| `/quests/accept` | `POST` | 是 | 玩家点击接受任务 | 验证前置条件并持久化 |
| `/quests/events` | `POST` | 是 | 击杀、采集、对话等事件发生后 | 幂等推进匹配 Objective |
| `/quests/claim` | `POST` | 是 | 手动领奖或 NPC 提交 | 原子发奖并返回最新玩家资产 |

UE 每个接受、事件和领奖请求都生成 UUID。`event_id` 重复时后端返回当前快照，不会重复推进；任务奖励只读取后端定义。

领奖成功后使用 `data.player` 刷新货币和背包，使用 `data.quest` 刷新任务行。常见错误码为 `quest_condition_not_met`、`quest_not_completed`、`quest_npc_mismatch` 和 `quest_event_rejected`。

## 7. 拾取页

拾取页/地图掉落不要直接改背包，只把 `drop_id` 交给后端。

| 接口 | 方法 | 需要 Token | 什么时候调 | 用途 |
|---|---|---:|---|---|
| `/loot/pickup` | `POST` | 是 | 玩家点击拾取物、自动拾取触发时 | 校验掉落并发奖，返回最新玩家资产 |
| `/admin/loot/spawn` | `POST` | 管理员 Token | 后台联调或战斗服生成掉落时 | 生成一个可拾取掉落 |
| `/admin/loot/query` | `POST` | 管理员 Token | 后台排查时 | 查询某玩家掉落和拾取状态 |

### `POST /loot/pickup`

请求：

```json
{
  "drop_id": "drop_10000001_default_1780670000000000"
}
```

成功返回里 UE 主要用：

| 字段 | UE 用途 |
|---|---|
| `drop.picked` | 判断掉落已被后端确认拾取 |
| `drop.rewards[]` | 展示拾取到的奖励 |
| `grant` | 奖励流水，方便调试 |
| `player` | 最新玩家货币和背包 |

常见失败：

| error.code | 含义 |
|---|---|
| `loot_not_found` | 掉落不存在 |
| `loot_already_picked` | 已经拾取过 |
| `loot_not_owner` | 不属于当前玩家 |
| `loot_expired` | 掉落过期 |

### 后台生成掉落请求

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

### 拾取页 UE 侧处理规则

- UE 不要本地给自己加物品。
- UE 只展示后端或战斗服下发的 `drop_id`。
- 点击拾取后，按钮/交互要先禁用，等 `/loot/pickup` 返回。
- 成功后用 `data.player` 刷新背包。
- 如果失败是 `loot_already_picked`、`loot_expired`，直接移除场景里的掉落表现。

## 8. 好友页

| 接口 | 方法 | 需要 Token | 什么时候调 | 用途 |
|---|---|---:|---|---|
| `/friends` | `GET` | 是 | 打开好友页时 | 拉好友总览，含在线、未读、最后消息 |
| `/friends/presence` | `GET` | 是 | 需要单独刷新在线态时 | 拉好友在线态 |
| `/friends/request` | `POST` | 是 | 发好友申请时 | 给目标玩家发申请 |
| `/friends/requests` | `GET` | 是 | 打开申请列表时 | 拉 incoming / outgoing 申请 |
| `/friends/respond` | `POST` | 是 | 处理申请时 | 接受或拒绝好友申请 |
| `/friends/add` | `POST` | 是 | 直接加好友场景 | 直接建立好友关系 |
| `/chat/block` | `POST` | 是 | 拉黑好友聊天时 | 设置聊天黑名单 |
| `/chat/blocks` | `GET` | 是 | 打开聊天黑名单页时 | 拉聊天黑名单列表 |

### 好友页优先用哪个接口

好友列表页优先直接吃：

- `GET /friends`

因为它已经聚合了：

- `online`
- `connection_count`
- `unread_count`
- `last_message`
- `friend_nickname`

不要自己拆成多次请求再本地拼。

## 9. 聊天页

聊天分两部分：

1. HTTP：历史、未读、离线消息、黑名单、已读
2. WebSocket：实时收发

### 8.1 聊天页 HTTP 接口

| 接口 | 方法 | 需要 Token | 什么时候调 | 用途 |
|---|---|---:|---|---|
| `/chat/unread` | `GET` | 是 | 进入聊天页时 | 拉未读计数 |
| `/chat/read` | `POST` | 是 | 打开某频道时 | 标记频道已读 |
| `/chat/offline/pull` | `POST` | 是 | 登录后、回前台时 | 拉离线消息 |
| `/chat/blocks` | `GET` | 是 | 打开聊天设置页 | 拉黑名单 |
| `/chat/block` | `POST` | 是 | 拉黑/取消拉黑时 | 修改黑名单 |
| `/chat/recall` | `POST` | 是 | 撤回消息时 | 撤回指定消息 |
| `/chat/presence/player` | `POST` | 否 | 调试或查单人在线态时 | 查询单玩家是否在线 |
| `/chat/presence/channel` | `POST` | 否 | 调试或房间成员查看时 | 查询频道成员 |
| `/chat/history/lobby` | `GET` | 否 | 大厅频道初次打开时 | 拉大厅历史 |

### 8.2 聊天页 WebSocket 接口

连接地址：

- `ws://120.26.173.47:8090`

连接成功后按这个顺序发：

| 步骤 | 发包 | 用途 |
|---|---|---|
| 1 | `type=auth` | 用 HTTP 登录拿到的 token 做 WS 鉴权 |
| 2 | `type=join` | 进入某个频道 |
| 3 | `type=say` | 发消息 |
| 4 | `type=history` | 通过 WS 拉某频道历史 |
| 5 | `type=recall` | 撤回消息 |

### WebSocket 发包示例

#### 1. 鉴权

```json
{
  "type": "auth",
  "token": "<login token>"
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

### WebSocket 服务端回包类型

| 回包 type | 含义 |
|---|---|
| `auth_result` | 鉴权结果 |
| `join_result` | 入频道结果 |
| `message` | 收到新消息 |
| `history_result` | 历史消息结果 |
| `recall_result` | 撤回结果 |
| `error` | 协议错误或业务错误 |

## 10. 页面到接口的最小映射

### 登录页

- `/health`
- `/auth/login`
- `/auth/register`
- `/auth/refresh`

### 大厅页

- `/players/me`
- `/friends`
- `/chat/unread`
- `/mail`
- `/gacha/pools`
- `/shop/products`

### 邮箱页

- `/mail`
- `/mail/read`
- `/mail/claim`
- `/mail/delete`

### 抽卡页

- `/gacha/pools`
- `/gacha/draw`
- `/gacha/history`

### 商城页

- `/shop/products`
- `/shop/buy`
- `/shop/history`

### 拾取页

- `/loot/pickup`

### 好友页

- `/friends`
- `/friends/request`
- `/friends/requests`
- `/friends/respond`
- `/chat/block`
- `/chat/blocks`

### 聊天页

- `/chat/unread`
- `/chat/read`
- `/chat/offline/pull`
- `ws://120.26.173.47:8090`

## 11. 最推荐前端先接的接口

如果 UE 程序想先最小跑通，建议优先做这 16 个：

1. `POST /auth/login`
2. `POST /auth/refresh`
3. `GET /players/me`
4. `GET /mail`
5. `POST /mail/claim`
6. `GET /gacha/pools`
7. `POST /gacha/draw`
8. `GET /shop/products`
9. `POST /shop/buy`
10. `POST /shop/history`
11. `POST /loot/pickup`
12. `GET /friends`
13. `GET /chat/unread`
14. `POST /chat/offline/pull`
15. WebSocket `auth`
16. WebSocket `join + say`

## 12. 相关文档

- UE 接入总说明：[docs/ue_backend_integration.md](/d:/code/model/docs/ue_backend_integration.md:1)
- 路由拆分导读：[examples/hello/ROUTES.md](/d:/code/model/examples/hello/ROUTES.md:1)
