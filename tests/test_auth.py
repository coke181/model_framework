# -*- coding: utf-8 -*-
import uuid

import pytest


pytestmark = pytest.mark.auth


def test_protected_endpoint_rejects_missing_token(api):
    response = api.get("/players/me", expected=401)
    assert api.error_code(response)


def test_register_then_login_returns_identity_and_profile(make_user):
    user = make_user("alice")
    assert user.account["login_name"].startswith("alice_")
    assert user.session["token"]
    assert user.player_id
    assert user.player["account_id"] == user.account_id


def test_players_me_and_list(api, make_user):
    user = make_user("mecheck")
    profile = user.api.data(user.api.get("/players/me", expected=200))
    assert profile["account_id"] == user.account_id

    players = api.data(api.get("/players", expected=200))
    assert isinstance(players, list)
    assert any(player["player_id"] == user.player_id for player in players)


def test_duplicate_registration_is_rejected(api):
    login_name = "duplicate_{}".format(uuid.uuid4().hex[:8])
    payload = {
        "login_name": login_name,
        "display_name": login_name,
        "password": "Passw0rd!",
    }
    api.post("/auth/register", json=payload, expected=201)
    response = api.post("/auth/register", json=payload, expected=409)
    assert api.error_code(response)


def test_invalid_password_is_rejected(api, make_user):
    user = make_user("badpw")
    response = api.post(
        "/auth/login",
        json={"login_name": user.login_name, "password": "WrongPassw0rd!"},
        expected=401,
    )
    assert api.error_code(response)


def test_change_password_then_relogin(api, make_user):
    user = make_user("pw")
    user.api.post(
        "/auth/change-password",
        json={"current_password": user.password, "new_password": "NewPassw0rd!"},
        expected=200,
    )
    login = api.data(api.post(
        "/auth/login",
        json={"login_name": user.login_name, "password": "NewPassw0rd!"},
        expected=200,
    ))
    assert login["account"]["account_id"] == user.account_id


def test_refresh_token_returns_new_token(api, make_user):
    user = make_user("refresh")
    data = api.data(api.post(
        "/auth/refresh",
        json={"refresh_token": user.session["refresh_token"]},
        expected=200,
    ))
    assert data["token"]


def test_sessions_returns_list(make_user):
    user = make_user("sessions")
    sessions = user.api.data(user.api.get("/auth/sessions", expected=200))
    assert isinstance(sessions, list)
    assert sessions


def test_quick_login_dev(api):
    data = api.data(api.post(
        "/auth/quick-login",
        json={"login_name": "quick_demo", "display_name": "QuickDemo"},
        expected=200,
    ))
    assert data["account"]["account_id"]
