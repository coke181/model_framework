# -*- coding: utf-8 -*-
import pytest


pytestmark = pytest.mark.admin


@pytest.mark.ui
def test_management_page_is_served_on_both_routes(api):
    for path in ("/admin", "/admin/ui"):
        response = api.get(path, expected=200)
        assert "text/html" in response.headers.get("content-type", "")
        assert "游戏后台管理台" in response.text
        assert "管理员登录" in response.text
        assert "/admin/query/players" in response.text


def test_admin_login_rejects_invalid_password(api):
    response = api.post(
        "/admin/login",
        json={"login_name": "pytest_admin", "password": "wrong-password"},
        expected=401,
    )
    assert api.error_code(response)


def test_admin_me_and_dashboard(admin_api):
    me = admin_api.data(admin_api.get("/admin/me", expected=200))
    assert "admin" in me

    dashboard = admin_api.data(admin_api.get("/admin/dashboard", expected=200))
    assert dashboard["home"] == "admin_dashboard"


def test_admin_endpoints_require_authentication(api):
    response = api.post("/admin/query/players", json={}, expected=401)
    assert api.error_code(response)


def test_player_token_cannot_use_admin_query(make_user):
    user = make_user("not_admin")
    response = user.api.post("/admin/query/players", json={}, expected=403)
    assert user.api.error_code(response)


def test_admin_query_player_and_inventory(admin_api, make_user):
    user = make_user("adminq")

    player = admin_api.data(admin_api.post(
        "/admin/query/player",
        json={"player_id": user.player_id},
        expected=200,
    ))
    assert player["player_id"] == user.player_id

    players = admin_api.data(admin_api.post(
        "/admin/query/players",
        json={"player_id": user.player_id, "limit": 10, "offset": 0},
        expected=200,
    ))
    assert players["total"] >= 1
    assert any(item["player_id"] == user.player_id for item in players["items"])

    inventory = admin_api.data(admin_api.post(
        "/admin/query/inventory",
        json={"player_id": user.player_id},
        expected=200,
    ))
    assert "inventory" in inventory


def test_admin_paginated_queries_have_stable_shape(admin_api, make_user):
    user = make_user("admin_page")
    cases = [
        ("/admin/query/reward-grants", {"player_id": user.player_id}),
        ("/admin/query/mail", {"player_id": user.player_id}),
        ("/admin/query/announcements", {}),
    ]
    for path, payload in cases:
        page = admin_api.data(admin_api.post(path, json=payload, expected=200))
        assert isinstance(page["items"], list)
        assert isinstance(page["total"], int)

    blacklists = admin_api.data(admin_api.post(
        "/admin/query/blacklists", json={}, expected=200
    ))
    assert isinstance(blacklists, list)


def test_admin_audit_logs(admin_api):
    page = admin_api.data(admin_api.get("/admin/audit-logs", expected=200))
    assert isinstance(page["items"], list)
    assert isinstance(page["total"], int)
