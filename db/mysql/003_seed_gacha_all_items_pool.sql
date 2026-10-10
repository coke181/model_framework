-- 文件职责：
-- 用当前数据库 item_definition 表里的“已启用物品”自动生成一个大卡池，方便先跑通抽卡。
-- 调用流程：
-- 1. 先执行 001_game_backend.sql，确保 item_definition / gacha_pool / gacha_pool_item 表存在。
-- 2. 再执行已有的物品配置导入，保证 item_definition 里有物品。
-- 3. 执行本文件，会创建或更新 all_items_2026 卡池。
-- 4. 本文件会先清空 all_items_2026 旧奖品，再按当前 item_definition 重新生成奖品列表。
-- 5. UE 调 GET /gacha/pools 可以看到 all_items_2026，调 POST /gacha/draw 即可测试。
--
-- 概率规则：
-- 当前后端抽卡使用“权重随机 + 硬保底”。
-- 本脚本把大池子的非保底总权重按稀有度分成：
-- 5 星总权重 60，约 0.6%。
-- 4 星总权重 510，约 5.1%。
-- 3 星总权重 9430，约 94.3%。
-- 同一个星级下，脚本会按当前物品数量平均分配权重。
-- 例如数据库有 2 个 5 星物品，则每个 5 星物品权重约 30。
--
-- 注意：
-- 这个脚本适合“先跑起来看效果”。
-- 后续正式运营池建议仍然用后台页面精确配置 UP、限定角色、限定武器和每个奖品权重。

INSERT INTO gacha_pool(
    pool_id,
    pool_type,
    name,
    description,
    banner_icon,
    cost_currency_id,
    cost_amount_per_draw,
    pity_5,
    pity_4,
    enabled,
    starts_at,
    ends_at)
VALUES (
    'all_items_2026',
    'standard',
    '全物品测试大池',
    '自动读取 item_definition 里所有已启用物品生成的大池子，用于快速跑通抽卡、背包入账和历史记录。',
    '/Game/UI/Banners/T_Gacha_All_Items_2026',
    'gem',
    160,
    90,
    10,
    1,
    NULL,
    NULL)
ON DUPLICATE KEY UPDATE
    pool_type = VALUES(pool_type),
    name = VALUES(name),
    description = VALUES(description),
    banner_icon = VALUES(banner_icon),
    cost_currency_id = VALUES(cost_currency_id),
    cost_amount_per_draw = VALUES(cost_amount_per_draw),
    pity_5 = VALUES(pity_5),
    pity_4 = VALUES(pity_4),
    enabled = VALUES(enabled),
    starts_at = VALUES(starts_at),
    ends_at = VALUES(ends_at),
    updated_at = CURRENT_TIMESTAMP;

-- 重新生成大池子奖品，避免旧物品已禁用或配置已改但卡池里还残留旧数据。
DELETE FROM gacha_pool_item
WHERE pool_id = 'all_items_2026';

INSERT INTO gacha_pool_item(
    pool_id,
    item_key,
    rarity,
    weight,
    is_up,
    reward_kind,
    reward_resource_id,
    reward_amount)
SELECT
    'all_items_2026' AS pool_id,
    item.item_id AS item_key,
    CASE item.rarity
        WHEN '5星' THEN 5
        WHEN '4星' THEN 4
        ELSE 3
    END AS rarity,
    CASE item.rarity
        WHEN '5星' THEN GREATEST(1, FLOOR(60 / GREATEST(counter.count_5, 1)))
        WHEN '4星' THEN GREATEST(1, FLOOR(510 / GREATEST(counter.count_4, 1)))
        ELSE GREATEST(1, FLOOR(9430 / GREATEST(counter.count_3, 1)))
    END AS weight,
    0 AS is_up,
    'item' AS reward_kind,
    item.item_id AS reward_resource_id,
    1 AS reward_amount
FROM item_definition AS item
CROSS JOIN (
    SELECT
        SUM(CASE WHEN rarity = '5星' THEN 1 ELSE 0 END) AS count_5,
        SUM(CASE WHEN rarity = '4星' THEN 1 ELSE 0 END) AS count_4,
        SUM(CASE WHEN rarity NOT IN ('4星', '5星') THEN 1 ELSE 0 END) AS count_3
    FROM item_definition
    WHERE enabled = 1
) AS counter
WHERE item.enabled = 1
ORDER BY
    CASE item.rarity
        WHEN '5星' THEN 5
        WHEN '4星' THEN 4
        ELSE 3
    END DESC,
    item.item_id ASC;
