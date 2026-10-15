# -*- coding: utf-8 -*-
import pytest


pytestmark = pytest.mark.business


def test_mail_send_read_claim_delete(admin_api, make_user):
    user = make_user("mail")
    initial_gold = int(user.player["gold"])

    mail = admin_api.data(admin_api.post("/mail/send", json={
        "player_id": user.player_id,
        "title": "welcome",
        "body": "hello from pytest",
        "sender_name": "System",
        "rewards": [{"kind": "currency", "resource_id": "gold", "amount": 5}],
        "expires_in_seconds": 3600,
    }, expected=201))
    assert mail["read"] is False
    assert mail["claimed"] is False

    read_mail = user.api.data(user.api.post(
        "/mail/read", json={"mail_id": mail["mail_id"]}, expected=200
    ))
    assert read_mail["read"] is True

    claim = user.api.data(user.api.post(
        "/mail/claim", json={"mail_id": mail["mail_id"]}, expected=200
    ))
    assert claim["mail"]["claimed"] is True
    assert claim["grant"]["rewards"][0]["amount"] == 5

    profile = user.api.data(user.api.get("/players/me", expected=200))
    assert int(profile["gold"]) == initial_gold + 5

    deleted = user.api.data(user.api.post(
        "/mail/delete", json={"mail_id": mail["mail_id"]}, expected=200
    ))
    assert deleted["deleted"] is True


def test_mail_send_requires_admin(make_user):
    user = make_user("mail_perm")
    response = user.api.post("/mail/send", json={
        "player_id": user.player_id,
        "title": "x",
        "body": "y",
        "rewards": [{"kind": "currency", "resource_id": "gold", "amount": 1}],
    }, expected=403)
    assert user.api.error_code(response)
