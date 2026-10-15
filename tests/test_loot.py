# -*- coding: utf-8 -*-
import pytest


pytestmark = pytest.mark.business


def test_loot_spawn_and_pickup_once(admin_api, make_user):
    user = make_user("loot")
    drop = admin_api.data(admin_api.post("/admin/loot/spawn", json={
        "player_id": user.player_id,
        "ttl_seconds": 3600,
        "rewards": [{"kind": "currency", "resource_id": "gold", "amount": 7}],
    }, expected=200))
    assert drop["picked"] is False

    pickup = user.api.data(user.api.post(
        "/loot/pickup", json={"drop_id": drop["drop_id"]}, expected=200
    ))
    assert pickup["drop"]["picked"] is True
    assert pickup["drop"]["picked_by_player_id"] == user.player_id
    assert pickup["grant"]["rewards"][0]["amount"] == 7

    response = user.api.post(
        "/loot/pickup", json={"drop_id": drop["drop_id"]}, expected=409
    )
    assert user.api.error_code(response)
