-- 文件职责：
-- 初始化一套可直接用于 UE 联调的正式抽卡配置。
-- 调用流程：
-- 1. 先执行 001_game_backend.sql 确保表结构存在。
-- 2. 再执行本文件，写入物品配置、卡池配置和卡池奖品。
-- 3. UE 调 GET /gacha/pools 能看到 standard_2026。
-- 4. UE 调 POST /gacha/draw 时，会从 gacha_pool_item 按权重和保底规则抽奖。
--
-- 概率说明：
-- 当前后端使用“权重随机 + 硬保底”。
-- 非保底时，本文件把总权重配置成 10000：
-- 5 星总权重 60，约 0.6%。
-- 4 星总权重 510，约 5.1%。
-- 3 星总权重 9430，约 94.3%。
-- 4 星保底触发时，只在 rarity >= 4 的奖品里按权重抽。
-- 5 星保底触发时，只在 rarity >= 5 的奖品里按权重抽。

INSERT INTO item_definition(item_id, name, description, icon, rarity, max_stack, item_type, enabled)
VALUES
    ('char_aurora_knight', '极光骑士', '5星测试角色。用于验证高稀有度抽卡展示、背包入账和历史记录。', '/Game/UI/Icons/Characters/T_AuroraKnight', '5星', 1, 'character', 1),
    ('char_shadow_mage', '影焰法师', '5星测试角色。用于验证常驻池第二个高稀有度奖品。', '/Game/UI/Icons/Characters/T_ShadowMage', '5星', 1, 'character', 1),
    ('char_wind_archer', '风语弓手', '4星测试角色。', '/Game/UI/Icons/Characters/T_WindArcher', '4星', 1, 'character', 1),
    ('char_iron_guard', '铁壁守卫', '4星测试角色。', '/Game/UI/Icons/Characters/T_IronGuard', '4星', 1, 'character', 1),
    ('weapon_silver_sword', '银纹长剑', '4星测试武器。', '/Game/UI/Icons/Weapons/T_SilverSword', '4星', 1, 'weapon', 1),
    ('weapon_starlight_staff', '星辉法杖', '4星测试武器。', '/Game/UI/Icons/Weapons/T_StarlightStaff', '4星', 1, 'weapon', 1),
    ('item_exp_book_small', '初级经验书', '3星养成材料。', '/Game/UI/Icons/Items/T_ExpBookSmall', '3星', 999, 'material', 1),
    ('item_exp_book_mid', '中级经验书', '3星养成材料。', '/Game/UI/Icons/Items/T_ExpBookMid', '3星', 999, 'material', 1),
    ('item_coin_pack_small', '小袋金币', '3星货币奖励，对应 gold。', '/Game/UI/Icons/Items/T_CoinPackSmall', '3星', 999, 'currency_pack', 1),
    ('item_potion_small', '小型生命药水', '3星消耗品。', '/Game/UI/Icons/Items/T_PotionSmall', '3星', 99, 'consumable', 1)
ON DUPLICATE KEY UPDATE
    name = VALUES(name),
    description = VALUES(description),
    icon = VALUES(icon),
    rarity = VALUES(rarity),
    max_stack = VALUES(max_stack),
    item_type = VALUES(item_type),
    enabled = VALUES(enabled),
    updated_at = CURRENT_TIMESTAMP;

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
    'standard_2026',
    'standard',
    '常驻祈愿',
    '常驻测试卡池。非保底约 0.6% 出 5 星，约 5.1% 出 4 星，90 抽 5 星硬保底，10 抽 4 星硬保底。',
    '/Game/UI/Banners/T_Gacha_Standard_2026',
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

INSERT INTO gacha_pool_item(
    pool_id,
    item_key,
    rarity,
    weight,
    is_up,
    reward_kind,
    reward_resource_id,
    reward_amount)
VALUES
    ('standard_2026', 'char_aurora_knight', 5, 30, 0, 'item', 'char_aurora_knight', 1),
    ('standard_2026', 'char_shadow_mage', 5, 30, 0, 'item', 'char_shadow_mage', 1),
    ('standard_2026', 'char_wind_archer', 4, 130, 0, 'item', 'char_wind_archer', 1),
    ('standard_2026', 'char_iron_guard', 4, 130, 0, 'item', 'char_iron_guard', 1),
    ('standard_2026', 'weapon_silver_sword', 4, 125, 0, 'item', 'weapon_silver_sword', 1),
    ('standard_2026', 'weapon_starlight_staff', 4, 125, 0, 'item', 'weapon_starlight_staff', 1),
    ('standard_2026', 'item_exp_book_small', 3, 2600, 0, 'item', 'item_exp_book_small', 3),
    ('standard_2026', 'item_exp_book_mid', 3, 2100, 0, 'item', 'item_exp_book_mid', 1),
    ('standard_2026', 'item_coin_pack_small', 3, 2500, 0, 'gold', '', 1000),
    ('standard_2026', 'item_potion_small', 3, 2230, 0, 'item', 'item_potion_small', 2)
ON DUPLICATE KEY UPDATE
    rarity = VALUES(rarity),
    weight = VALUES(weight),
    is_up = VALUES(is_up),
    reward_kind = VALUES(reward_kind),
    reward_resource_id = VALUES(reward_resource_id),
    reward_amount = VALUES(reward_amount),
    updated_at = CURRENT_TIMESTAMP;
