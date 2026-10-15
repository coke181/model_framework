# -*- coding: utf-8 -*-
import pytest


pytestmark = pytest.mark.business


def test_admin_grant_then_player_sees_grant_and_balance(admin_api, make_user):
    user = make_user("reward")
    initial_gem = int(user.player["gem"])
    grant = admin_api.data(admin_api.post("/rewards/grant", json={
        "player_id": user.player_id,
        "source_type": "gm",
        "source_id": "pytest",
        "rewards": [{"kind": "currency", "resource_id": "gem", "amount": 123}],
    }, expected=200))
    assert grant["grant_id"]
    assert grant["rewards"][0]["amount"] == 123

    grants = user.api.data(user.api.get("/rewards/grants", expected=200))
    assert any(item["grant_id"] == grant["grant_id"] for item in grants)

    profile = user.api.data(user.api.get("/players/me", expected=200))
    assert int(profile["gem"]) == initial_gem + 123
