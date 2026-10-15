# -*- coding: utf-8 -*-
import pytest


pytestmark = pytest.mark.business


def test_shop_product_buy_and_history(api, admin_api, make_user):
    user = make_user("shop")
    product_id = "sword_pytest"

    admin_api.post("/admin/shop/products/upsert", json={
        "product_id": product_id,
        "name": "pytest sword",
        "price_currency_id": "gem",
        "price_amount": 100,
        "rewards": [{"kind": "item", "resource_id": "sword", "amount": 1}],
        "enabled": True,
    }, expected=200)
    admin_api.post("/rewards/grant", json={
        "player_id": user.player_id,
        "source_type": "gm",
        "rewards": [{"kind": "currency", "resource_id": "gem", "amount": 200}],
    }, expected=200)

    products = api.data(api.get("/shop/products", expected=200))
    assert product_id in [product["product_id"] for product in products["items"]]

    purchase = user.api.data(user.api.post(
        "/shop/buy", json={"product_id": product_id}, expected=200
    ))
    assert purchase["product"]["product_id"] == product_id

    history = user.api.data(user.api.post("/shop/history", json={}, expected=200))
    assert any(item["product_id"] == product_id for item in history["items"])

    inventory = user.api.data(user.api.get("/players/me/inventory", expected=200))
    assert any(item["item_id"] == "sword" for item in inventory["items"])
