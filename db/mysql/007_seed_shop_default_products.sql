-- 文件职责：
-- 初始化一组可直接用于 UE 联调的商城商品。
-- 调用流程：
-- 1. 先执行 001_game_backend.sql，确保 shop_product / shop_purchase 表存在。
-- 2. 再执行本文件，写入默认商品配置。
-- 3. UE 调 GET /shop/products 能看到这些商品。
-- 4. UE 调 POST /shop/buy 时，后端会按 product_id 扣货币、发奖励、写购买流水。
--
-- 说明：
-- 这些商品都是“后端托管商品”。UE 只展示和购买，不要在客户端本地扣 gold/gem。

INSERT INTO shop_product(
    product_id,
    category,
    name,
    description,
    icon,
    price_currency_id,
    price_amount,
    rewards_json,
    purchase_limit,
    enabled,
    starts_at,
    ends_at)
VALUES
    (
        'daily_gem_pack_001',
        'daily',
        '1600 钻石礼包',
        '文档默认商品。后端扣费与发奖，客户端只负责展示和购买。',
        'icon_shop_gem_pack_1600',
        'gold',
        1600,
        JSON_ARRAY(JSON_OBJECT('kind', 'currency', 'resource_id', 'gem', 'amount', 1600)),
        1,
        1,
        NULL,
        NULL
    ),
    (
        'gem_pack_1600',
        'daily',
        '1600 钻石礼包',
        '兼容 UE 默认商品 ID：gem_pack_1600。',
        'icon_shop_gem_pack_1600',
        'gold',
        1600,
        JSON_ARRAY(JSON_OBJECT('kind', 'currency', 'resource_id', 'gem', 'amount', 1600)),
        1,
        1,
        NULL,
        NULL
    ),
    (
        'doc_default_gem_pack_1600',
        'daily',
        '1600 钻石礼包',
        '兼容文档默认商品 ID：doc_default_gem_pack_1600。',
        'icon_shop_gem_pack_1600',
        'gold',
        1600,
        JSON_ARRAY(JSON_OBJECT('kind', 'currency', 'resource_id', 'gem', 'amount', 1600)),
        1,
        1,
        NULL,
        NULL
    )
ON DUPLICATE KEY UPDATE
    category = VALUES(category),
    name = VALUES(name),
    description = VALUES(description),
    icon = VALUES(icon),
    price_currency_id = VALUES(price_currency_id),
    price_amount = VALUES(price_amount),
    rewards_json = VALUES(rewards_json),
    purchase_limit = VALUES(purchase_limit),
    enabled = VALUES(enabled),
    starts_at = VALUES(starts_at),
    ends_at = VALUES(ends_at),
    updated_at = CURRENT_TIMESTAMP;
