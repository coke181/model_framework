# 抽卡卡池配置说明

## 文件用途

`db/mysql/002_seed_gacha_standard_pool.sql` 是一份正式可执行的卡池种子文件。

它会写入：

- `item_definition`：角色、武器、材料等展示配置。
- `gacha_pool`：卡池主配置。
- `gacha_pool_item`：卡池里的奖品、星级、权重、发放奖励。

## 执行顺序

服务器上先执行表结构：

```bash
cd /opt/ue-game-backend/model
sudo mysql game_backend < db/mysql/001_game_backend.sql
```

再执行卡池内容：

```bash
sudo mysql game_backend < db/mysql/002_seed_gacha_standard_pool.sql
```

如果你暂时不想一个个配置卡池奖品，可以直接用数据库现有物品生成一个测试大池：

```bash
sudo mysql game_backend < db/mysql/003_seed_gacha_all_items_pool.sql
```

这个脚本会读取 `item_definition` 里所有 `enabled = 1` 的物品，自动写入 `all_items_2026` 卡池。

执行后重启后端不是必须的，因为卡池配置从 MySQL 查询；但如果服务刚启动时数据库连接异常，重启一次更稳：

```bash
systemctl restart hello-service
```

## 当前默认卡池

默认卡池：

```text
pool_id = standard_2026
pool_type = standard
name    = 常驻祈愿
cost    = gem x160
pity_5  = 90
pity_4  = 10
```

快速测试大池：

```text
pool_id = all_items_2026
pool_type = standard
name    = 全物品测试大池
cost    = gem x160
pity_5  = 90
pity_4  = 10
```

`all_items_2026` 不需要手动逐个填写奖品，它会从 `item_definition` 自动生成。

UE 抽卡时传：

```json
{
  "pool_id": "standard_2026",
  "count": 10
}
```

如果要测试全物品大池，把 `pool_id` 换成：

```json
{
  "pool_id": "all_items_2026",
  "count": 10
}
```

## 当前默认概率

当前后端是“权重随机 + 硬保底”。

`002_seed_gacha_standard_pool.sql` 把总权重配成 `10000`：

| 星级 | 总权重 | 非保底概率 |
|---|---:|---:|
| 5 星 | 60 | 0.6% |
| 4 星 | 510 | 5.1% |
| 3 星 | 9430 | 94.3% |

`003_seed_gacha_all_items_pool.sql` 也是同样的星级总权重，但每个星级内部会按物品数量平均分。

例如：

- 数据库里有 2 个 5 星物品，每个 5 星物品权重大约是 `30`。
- 数据库里有 5 个 4 星物品，每个 4 星物品权重大约是 `102`。
- 数据库里有 20 个 3 星物品，每个 3 星物品权重大约是 `471`。

注意：

- 这只是非保底概率。
- 触发 4 星保底时，只会在 `rarity >= 4` 的奖品里按权重抽。
- 触发 5 星保底时，只会在 `rarity >= 5` 的奖品里按权重抽。

## 怎么改概率

后台页面现在可以直接改：

- `池子种类`：`standard / limited_role / limited_weapon / newbie / event`
- `奖品 item_key`
- `星级`
- `权重`
- `是否 UP`
- `奖励类型 / 奖励资源 ID / 奖励数量`

页面里的“概率”是按当前池子所有奖品的 `weight` 自动算出来的非保底概率。

如果直接改 SQL，则改 `gacha_pool_item.weight`。

例如：

```sql
UPDATE gacha_pool_item
SET weight = 50
WHERE pool_id = 'standard_2026'
  AND item_key = 'char_aurora_knight';
```

概率计算方式：

```text
某奖品概率 = 该奖品 weight / 当前候选奖品总 weight
```

如果你希望非保底 5 星总概率是 `1%`，可以把所有 5 星奖品的 `weight` 总和改成 `100`，并让全池总权重保持 `10000`。

## 怎么加新奖品

先确保物品配置存在：

```sql
INSERT INTO item_definition(item_id, name, description, icon, rarity, max_stack, item_type, enabled)
VALUES ('char_new_hero', '新角色', '新 5 星角色。', '/Game/UI/Icons/Characters/T_NewHero', '5星', 1, 'character', 1)
ON DUPLICATE KEY UPDATE
    name = VALUES(name),
    description = VALUES(description),
    icon = VALUES(icon),
    rarity = VALUES(rarity),
    max_stack = VALUES(max_stack),
    item_type = VALUES(item_type),
    enabled = VALUES(enabled),
    updated_at = CURRENT_TIMESTAMP;
```

再加入卡池：

```sql
INSERT INTO gacha_pool_item(pool_id, item_key, rarity, weight, is_up, reward_kind, reward_resource_id, reward_amount)
VALUES ('standard_2026', 'char_new_hero', 5, 30, 0, 'item', 'char_new_hero', 1)
ON DUPLICATE KEY UPDATE
    rarity = VALUES(rarity),
    weight = VALUES(weight),
    is_up = VALUES(is_up),
    reward_kind = VALUES(reward_kind),
    reward_resource_id = VALUES(reward_resource_id),
    reward_amount = VALUES(reward_amount),
    updated_at = CURRENT_TIMESTAMP;
```

## 怎么新建不同种类的池子

在后台页面里新增卡池时，先填：

```text
pool_id: limited_role_2026_07
池子种类: limited_role
卡池名称: 限定角色祈愿
消耗货币: gem
每抽消耗: 160
5星硬保底: 90
4星硬保底: 10
是否启用: 启用
```

然后点“保存卡池”。

再逐个填写奖品，点“保存奖品”。页面会自动刷新表格，并显示每个奖品的当前权重概率。

## UE 验证方式

1. 登录拿 token。
2. 调 `GET /gacha/pools`，确认能看到 `standard_2026`。
   如果你执行了全物品大池脚本，也应该能看到 `all_items_2026`。
3. 调 `POST /gacha/draw`：

```json
{
  "pool_id": "standard_2026",
  "count": 1
}
```

4. 看返回的 `results[0].item_key`、`rarity`、`reward`。
5. 再调 `GET /players/me`，确认 `gem` 已扣除，背包或金币已增加。

## 后续要做的高级规则

当前还没做：

- 软保底曲线。
- 50/50 歪了以后下次必 UP。
- 武器池定轨。
- 新手池折扣。
- 卡池之间共享保底。

这些都应该继续放后端做，不应该放 UE 客户端。
