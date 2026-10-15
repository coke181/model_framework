"""Verify rejected requests leave the target player's assets and mail intact."""

import pytest


pytestmark = [pytest.mark.auth, pytest.mark.business]


@pytest.mark.parametrize("action", ["read", "claim", "delete"])
def test_player_cannot_modify_another_players_mail(admin_api, make_user, action):
    owner, attacker = make_user("owner"), make_user("attacker")
    mail = admin_api.data(admin_api.post("/mail/send", json={
        "player_id": owner.player_id, "title": "private", "body": "owner only",
        "rewards": [{"kind": "currency", "resource_id": "gold", "amount": 11}],
    }, expected=201))
    denied = attacker.api.post("/mail/" + action, json={"mail_id": mail["mail_id"]}, expected=404)
    assert attacker.api.error_code(denied) == "mail_not_found"
    for user in (owner, attacker):
        profile = user.api.data(user.api.get("/players/me", expected=200))
        assert int(profile["gold"]) == int(user.player["gold"])
    # The owner's subsequent claim proves the attacker did not delete/consume the mail.
    claim = owner.api.data(owner.api.post("/mail/claim", json={"mail_id": mail["mail_id"]}, expected=200))
    assert claim["mail"]["claimed"] is True
    assert int(claim["player"]["gold"]) == int(owner.player["gold"]) + 11


@pytest.mark.parametrize("path", ["/rewards/grant", "/admin/shop/products/upsert"])
def test_player_cannot_write_admin_business_data(make_user, path):
    user = make_user("deny_write")
    response = user.api.post(path, json={"player_id": user.player_id}, expected=403)
    assert user.api.error_code(response)
    profile = user.api.data(user.api.get("/players/me", expected=200))
    assert (profile["gold"], profile["gem"]) == (user.player["gold"], user.player["gem"])


@pytest.mark.parametrize("amount", [0, -1, 2**63])
def test_invalid_reward_amount_does_not_change_assets(admin_api, make_user, amount):
    user = make_user("invalid_reward")
    response = admin_api.post("/rewards/grant", json={
        "player_id": user.player_id,
        "rewards": [{"kind": "currency", "resource_id": "gem", "amount": amount}],
    }, expected=400)
    assert admin_api.error_code(response)
    profile = user.api.data(user.api.get("/players/me", expected=200))
    assert int(profile["gem"]) == int(user.player["gem"])
    assert user.api.data(user.api.get("/rewards/grants", expected=200)) == []
