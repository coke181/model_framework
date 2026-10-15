"""Shared fixtures for explicit MySQL, Redis, and WebSocket integration tests."""

from __future__ import annotations

import os
import uuid

import pytest

from framework import ApiClient, TestUser


def required_env(name):
    value = os.environ.get(name)
    if not value:
        pytest.skip("{} is required for integration tests".format(name))
    return value


@pytest.fixture(scope="session", autouse=True)
def require_explicit_integration_target():
    required_env("HELLO_SERVICE_INTEGRATION_BASE_URL")


@pytest.fixture(scope="session")
def integration_api():
    client = ApiClient(required_env("HELLO_SERVICE_INTEGRATION_BASE_URL"), timeout=20)
    config = client.data(client.get("/config", expected=200))
    assert config["storage_mode"] == "mysql_redis"
    yield client
    client.close()


@pytest.fixture(scope="session")
def integration_admin_api(integration_api):
    identity = integration_api.data(integration_api.post(
        "/admin/login",
        json={
            "login_name": required_env("HELLO_SERVICE_INTEGRATION_ADMIN_LOGIN"),
            "password": required_env("HELLO_SERVICE_INTEGRATION_ADMIN_PASSWORD"),
        },
        expected=200,
    ))
    client = integration_api.with_token(identity["session"]["token"])
    yield client
    client.close()


@pytest.fixture(scope="session")
def integration_ws_url():
    return required_env("HELLO_SERVICE_INTEGRATION_WS_URL")


@pytest.fixture(scope="session")
def mysql_connection():
    try:
        import pymysql
    except ImportError:
        pytest.skip("PyMySQL is not installed; install tests/requirements-integration.txt")

    connection = pymysql.connect(
        host=os.environ.get("HELLO_SERVICE_MYSQL_HOST", "127.0.0.1"),
        port=int(os.environ.get("HELLO_SERVICE_MYSQL_PORT", "3306")),
        user=os.environ.get("HELLO_SERVICE_MYSQL_USER", "game_backend"),
        password=required_env("HELLO_SERVICE_MYSQL_PASSWORD"),
        database=os.environ.get("HELLO_SERVICE_MYSQL_DATABASE", "game_backend"),
        charset="utf8mb4",
        autocommit=True,
        cursorclass=pymysql.cursors.DictCursor,
    )
    yield connection
    connection.close()


@pytest.fixture(scope="session")
def redis_client():
    try:
        import redis
    except ImportError:
        pytest.skip("redis is not installed; install tests/requirements-integration.txt")

    client = redis.Redis(
        host=os.environ.get("HELLO_SERVICE_REDIS_HOST", "127.0.0.1"),
        port=int(os.environ.get("HELLO_SERVICE_REDIS_PORT", "6379")),
        db=int(os.environ.get("HELLO_SERVICE_REDIS_DB", "0")),
        password=os.environ.get("HELLO_SERVICE_REDIS_PASSWORD") or None,
        decode_responses=True,
        socket_timeout=5,
    )
    client.ping()
    yield client
    client.close()


@pytest.fixture
def make_integration_user(integration_api):
    clients = []

    def create(prefix="it"):
        login_name = "{}_{}".format(prefix, uuid.uuid4().hex[:12])
        password = "Passw0rd!"
        registered = integration_api.data(integration_api.post(
            "/auth/register",
            json={
                "login_name": login_name,
                "display_name": login_name,
                "password": password,
            },
            expected=201,
        ))
        identity = integration_api.data(integration_api.post(
            "/auth/login",
            json={"login_name": login_name, "password": password},
            expected=200,
        ))
        client = integration_api.with_token(identity["session"]["token"])
        clients.append(client)
        profile = client.data(client.get("/players/me", expected=200))
        user = TestUser(
            login_name=login_name,
            password=password,
            account=identity["account"],
            player=profile,
            session=identity["session"],
            api=client,
        )
        assert registered["player"]["player_id"] == user.player_id
        return user

    yield create
    for client in clients:
        client.close()


@pytest.fixture
def integration_user(make_integration_user):
    return make_integration_user()
