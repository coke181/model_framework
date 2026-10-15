# -*- coding: utf-8 -*-
import pytest


pytestmark = pytest.mark.business


def test_gacha_pool_draw_and_history(api, admin_api, make_user):
    user = make_user("gacha")
    pool_id = "pool_pytest"

    admin_api.post("/admin/gacha/pools/upsert", json={
        "pool_id": pool_id,
        "name": "pytest pool",
        "cost_currency_id": "gem",
        "cost_amount_per_draw": 1,
        "enabled": True,
    }, expected=200)
    admin_api.post("/admin/gacha/items/upsert", json={
        "pool_id": pool_id,
        "item_key": "sword",
        "reward_kind": "item",
        "reward_resource_id": "sword",
        "reward_amount": 1,
        "rarity": 3,
        "weight": 1,
    }, expected=200)
    admin_api.post("/rewards/grant", json={
        "player_id": user.player_id,
        "source_type": "gm",
        "rewards": [{"kind": "currency", "resource_id": "gem", "amount": 10}],
    }, expected=200)

    pools = api.data(api.get("/gacha/pools", expected=200))
    assert pool_id in [pool["pool_id"] for pool in pools["items"]]

    draw = user.api.data(user.api.post(
        "/gacha/draw", json={"pool_id": pool_id, "count": 1}, expected=200
    ))
    assert draw["results"]

    history = user.api.data(user.api.post(
        "/gacha/history", json={"pool_id": pool_id}, expected=200
    ))
    assert history["items"]
